// resources/shaders/blit.hlsl
#pragma pack_matrix(column_major)
#include "pbr_helpers.hlsl"
#include "uniforms.hlsl"

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);

    // Flip-free projection; top-left of the texture maps straight to top-left of clip space (-1, -1)
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

struct PushConstants {
    float vignetteIntensity;
    float vignettePower;
    int   fullBright;
};
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] Texture2D<float4>             texInput;
[[vk::binding(1, 0)]] SamplerState                  smp;
[[vk::binding(2, 0)]] Texture2D<float4>             texBloom; // <-- Bound dynamically by BlitPass
[[vk::binding(3, 0)]] Texture2D<float>              texDepth;
[[vk::binding(4, 0)]] Texture3D<float4>             texVoxelIntegrated;
[[vk::binding(5, 0)]] ConstantBuffer<FrameUniforms> frame;

float3 ACESFilm(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PSMain(VSOutput input): SV_Target0 {
    float3 hdrColor = texInput.SampleLevel(smp, input.uv, 0).rgb;

    // Bypasses HDR exposure, bloom, and tonemapping if fullbright is enabled
    if (pc.fullBright != 0) {
        return float4(hdrColor, 1.0f);
    }

    // 1. Fetch Geometry Depth and Compute Volumetric Coordinates
    float rawDepth  = texDepth.SampleLevel(smp, input.uv, 0).r;
    float viewDepth = 1000.0f; // Default for skybox
    if (rawDepth < 1.0f) {
        float3 worldPos = ReconstructWorldPos(input.uv, rawDepth, frame.invViewProj);
        viewDepth       = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;
    }

    // 2. Apply Volumetric Scattering
    float  zSlice      = log(max(viewDepth, 0.1f) / 0.1f) / log(10000.0f);
    float4 volumetrics = texVoxelIntegrated.SampleLevel(smp, float3(input.uv, zSlice), 0);
    hdrColor           = hdrColor * volumetrics.a + volumetrics.rgb;

    // Sample Bloom and blend additively (adjust the 0.5f intensity multiplier as needed)
    float3 bloom = texBloom.SampleLevel(smp, input.uv, 0).rgb;
    hdrColor += bloom * 0.5f;

    // 3. Exposure Control: Scale down the massive HDR values (Sun is 250.0)
    hdrColor *= 0.015f;

    // 4. Tonemap HDR -> LDR
    float3 finalColor = ACESFilm(hdrColor);

    // 5. Apply Vignette overlay
    if (pc.vignetteIntensity > 0.0f) {
        float2 uvDist   = abs(input.uv - 0.5f) * pc.vignetteIntensity;
        float  vignette = saturate(1.0f - dot(uvDist, uvDist));
        vignette        = pow(vignette, max(pc.vignettePower, 0.01f));
        finalColor *= vignette;
    }

    return float4(finalColor, 1.0f);
}
