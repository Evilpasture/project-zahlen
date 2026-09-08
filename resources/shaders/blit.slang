// resources/shaders/blit.slang

import pbr_helpers;
import uniforms;

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID: SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);

    // Flip-free projection; top-left of the texture maps straight to top-left of clip space (-1, -1)
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

struct PushConstants {
    float vignetteIntensity;
    float vignettePower;
    int fullBright;
    float exposure;
    float bloomStrength;
    float contrast;
    float saturation;
    int tonemapper;
    float3 colorFilter;
    float _padding;
};
[vk::push_constant]
PushConstants pc;

Texture2D<float4> texInput;
SamplerState smp;
Texture2D<float4> texBloom; // Bound dynamically by BlitPass
Texture2D<float> texDepth;
ConstantBuffer<FrameUniforms> frame;

float3 ACESFilm(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float3 ReinhardTonemap(float3 color) {
    return color / (1.0f + color);
}

float3 NeutralTonemap(float3 color) {
    const float startCompression = 0.76f;
    const float desaturation = 0.15f;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    color -= offset;
    float peakValue = max(color.r, max(color.g, color.b));
    if (peakValue >= startCompression) {
        float d = 1.0f - startCompression;
        float newPeak = 1.0f - d * d / (peakValue + d - startCompression);
        color *= newPeak / peakValue;
        float g = 1.0f - 1.0f / (desaturation * (peakValue - newPeak) + 1.0f);
        color = lerp(color, newPeak.xxx, g);
    }
    return color;
}

float4 PSMain(VSOutput input) : SV_Target0 {
    float3 hdrColor = texInput.SampleLevel(smp, input.uv, 0).rgb;

    // Bypasses all final colour processing when fullbright is enabled.
    if (pc.fullBright != 0) {
        return float4(hdrColor, 1.0f);
    }

    // Sample bloom and blend additively.
    float3 bloom = texBloom.SampleLevel(smp, input.uv, 0).rgb;
    hdrColor += bloom * pc.bloomStrength;

    // Exposure control: scales the HDR values before tonemapping.
    hdrColor *= pc.exposure;

    // Tonemap HDR -> LDR. Unknown values intentionally fall back to Linear.
    float3 finalColor;
    switch (pc.tonemapper) {
        case 1: finalColor = ACESFilm(hdrColor); break;
        case 2: finalColor = ReinhardTonemap(hdrColor); break;
        case 3: finalColor = NeutralTonemap(hdrColor); break;
        default: finalColor = hdrColor; break;
    }

    // Grade after tonemapping: RGB tint, contrast around middle grey, then
    // Rec.709-luma saturation. Negative values cannot reach the swapchain.
    finalColor *= pc.colorFilter;
    finalColor = (finalColor - 0.5f) * pc.contrast + 0.5f;
    float luma = dot(finalColor, float3(0.2126f, 0.7152f, 0.0722f));
    finalColor = lerp(luma.xxx, finalColor, pc.saturation);
    finalColor = max(finalColor, 0.0f);

    // Apply Vignette overlay
    if (pc.vignetteIntensity > 0.0f) {
        float2 uvDist  = abs(input.uv - 0.5f) * pc.vignetteIntensity;
        float vignette = saturate(1.0f - dot(uvDist, uvDist));
        vignette       = pow(vignette, max(pc.vignettePower, 0.01f));
        finalColor *= vignette;
    }

    return float4(finalColor, 1.0f);
}
