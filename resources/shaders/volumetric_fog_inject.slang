
import gpu_push_constants;
import hash;
import uniforms;
import volumetric_grid;

namespace Dispatch {
    [[vk::constant_id(1000)]] const uint SizeX = Volumetric.Width;
    [[vk::constant_id(1001)]] const uint SizeY = Volumetric.Height;
    [[vk::constant_id(1002)]] const uint SizeZ = Volumetric.Depth;
}

[vk::push_constant]
VolumetricFogPushConstants fogPC;

RWTexture3D<float4> outVoxelMedia;
Texture3D<float4> noiseTexture;
SamplerState noiseSampler;
ConstantBuffer<FrameUniforms> frame;

float3 GetVoxelWorldPos(uint3 tid, float2 resolution, out float outDepthVS) {
    float2 uv     = (float2(tid.xy) + 0.5f) / resolution;
    float jitter  = TemporalHash2D(tid.xy, frame.camPos.w) - 0.5f;
    float depthVS = frame.nearZ * pow(frame.farZ / frame.nearZ, (float(tid.z) + 0.5f + jitter * 0.5f) / float(Volumetric.Depth));
    outDepthVS    = depthVS;

    float4 clip          = float4(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, 0.5f, 1.0f);
    float4 worldSpacePos = mul(frame.invViewProj, clip);
    float3 dir           = normalize(worldSpacePos.xyz / worldSpacePos.w - frame.camPos.xyz);
    return frame.camPos.xyz + dir * depthVS;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint3 gridDim;
    outVoxelMedia.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (any(tid >= gridDim) || any(tid >= uint3(Dispatch.SizeX, Dispatch.SizeY, Dispatch.SizeZ)))
        return;

    float depthVS   = 0.0f;
    float3 worldPos = GetVoxelWorldPos(tid, float2(gridDim.xy), depthVS);
    float time      = frame.camPos.w;

    float boundedY   = max(worldPos.y - fogPC.heightOffset, -10.0f);
    float fogDensity = fogPC.density * exp(-fogPC.heightFalloff * boundedY);

    if (fogPC.enableNoise != 0 && fogDensity > 0.00001f) {
        float3 p        = worldPos * fogPC.noiseScale +
                          float3(time * fogPC.noiseSpeed * 0.2f, time * fogPC.noiseSpeed * 0.1f, time * fogPC.noiseSpeed * 0.15f);
        float3 noisePos = p * (1.0f / 64.0f);
        float noiseVal  = noiseTexture.SampleLevel(noiseSampler, noisePos, 0).r;
        fogDensity *= lerp(1.0f, noiseVal * 2.0f, fogPC.noiseIntensity);
    }

    float3 scatteringCoeff = fogPC.scatteringColor * fogDensity;
    float3 extinctionCoeff = (fogPC.scatteringColor + fogPC.absorptionColor) * fogDensity;

    if (frame.probeMin.w > 0.0f) {
        float3 center  = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
        float3 extents = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
        float3 dist    = abs(worldPos - center) / max(extents, 0.001f);
        float mask     = saturate(1.0f - max(dist.x, max(dist.y, dist.z)));
        scatteringCoeff *= mask;
        extinctionCoeff *= mask;
    }

    float scalarExtinction = dot(extinctionCoeff, float3(0.2126f, 0.7152f, 0.0722f));
    outVoxelMedia[tid]     = float4(scatteringCoeff, scalarExtinction);
}
