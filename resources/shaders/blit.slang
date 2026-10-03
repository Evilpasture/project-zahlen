
import pbr_helpers;
import uniforms;

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID: SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);

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
Texture2D<float4> texBloom;
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

// Stephen Hill's fitted ACES RRT+ODT, ported from KhronosGroup/glTF-Sample-Renderer
// source/Renderer/shaders/tonemapping.glsl (Apache-2.0). The fidelity generator
// selects its historical 1/0.6 exposure boost variant. Matrices are explicit rows
// here to avoid shader-language layout ambiguity.
float3 ACESHillFilm(float3 color) {
    color = float3(
        dot(float3(0.59719f, 0.35458f, 0.04823f), color),
        dot(float3(0.07600f, 0.90834f, 0.01566f), color),
        dot(float3(0.02840f, 0.13383f, 0.83777f), color)
    );

    float3 a = color * (color + 0.0245786f) - 0.000090537f;
    float3 b = color * (0.983729f * color + 0.4329510f) + 0.238081f;
    color = a / b;

    color = float3(
        dot(float3(1.60475f, -0.53108f, -0.07367f), color),
        dot(float3(-0.10208f, 1.10813f, -0.00605f), color),
        dot(float3(-0.00327f, -0.07276f, 1.07602f), color)
    );
    return saturate(color);
}

// tonemapper push-constant IDs: 0 linear, 1 Narkowicz ACES, 2 Reinhard,
// 3 Khronos PBR Neutral, 4 ACES Hill with the generator's 1/0.6 boost.
float4 PSMain(VSOutput input) : SV_Target0 {
    float4 hdrSample = texInput.SampleLevel(smp, input.uv, 0);
    float3 hdrColor  = hdrSample.rgb;
    float alpha = hdrSample.a;

    if (pc.fullBright != 0) {
        return float4(hdrColor, alpha);
    }

    float3 bloom = texBloom.SampleLevel(smp, input.uv, 0).rgb;
    hdrColor += bloom * pc.bloomStrength;

    hdrColor *= pc.exposure;

    float3 finalColor;
    switch (pc.tonemapper) {
        case 1: finalColor = ACESFilm(hdrColor); break;
        case 2: finalColor = ReinhardTonemap(hdrColor); break;
        case 3: finalColor = NeutralTonemap(hdrColor); break;
        // Khronos glTF Render Fidelity Generator's glTF Sample Viewer path:
        // exposure *= 1/0.6 before Hill's RRT+ODT. The SRGB target performs
        // the display encoding after this linear-light tone curve.
        case 4: finalColor = ACESHillFilm(hdrColor / 0.6f); break;
        default: finalColor = hdrColor; break;
    }

    finalColor *= pc.colorFilter;
    finalColor = (finalColor - 0.5f) * pc.contrast + 0.5f;
    float luma = dot(finalColor, float3(0.2126f, 0.7152f, 0.0722f));
    finalColor = lerp(luma.xxx, finalColor, pc.saturation);
    finalColor = max(finalColor, 0.0f);

    if (pc.vignetteIntensity > 0.0f) {
        float2 uvDist  = abs(input.uv - 0.5f) * pc.vignetteIntensity;
        float vignette = saturate(1.0f - dot(uvDist, uvDist));
        vignette       = pow(vignette, max(pc.vignettePower, 0.01f));
        finalColor *= vignette;
    }

    return float4(finalColor, alpha);
}
