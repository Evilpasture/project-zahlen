// resources/shaders/volumetric_fog_inject.hlsl

import uniforms;
import volumetric_grid;

namespace Dispatch {
    [[vk::constant_id(1000)]] const uint SizeX = Volumetric.Width;
    [[vk::constant_id(1001)]] const uint SizeY = Volumetric.Height;
    [[vk::constant_id(1002)]] const uint SizeZ = Volumetric.Depth;
}

struct GPUVolumetricVolume {
    float4x4 invTransform;
    float4 extentsAndType;   // xyz = extents, w = type (0=Box, 1=Sphere)
    float4 colorAndDensity;  // xyz = color, w = density
    float4 emissiveAndAniso; // xyz = emissive, w = anisotropy
};

struct VolumetricFogPushConstants {
    float density;
    float heightFalloff;
    float heightOffset;
    float anisotropy;

    float3 scatteringColor;
    float noiseScale;

    float3 absorptionColor;
    float noiseSpeed;

    float3 emissiveColor;
    float noiseIntensity;

    uint volumeCount;
    uint enableNoise;
    uint _pad0;
    uint _pad1;
};

[vk::push_constant]
VolumetricFogPushConstants fogPC;

RWTexture3D<float4> outVoxelMedia;
Texture3D<float4> noiseTexture;
SamplerState noiseSampler;
ConstantBuffer<FrameUniforms> frame;
StructuredBuffer<GPUVolumetricVolume> fogVolumes;

float TemporalHash2D(uint2 tid, float timeOffset) {
    float spatial = frac(float(tid.x * 12664589 + tid.y * 9546283) * 0.6180339887f);
    return frac(spatial + timeOffset * 60.0f * 0.6180339887f);
}

float3 GetVoxelWorldPos(uint3 tid, float2 resolution, out float outDepthVS) {
    float2 uv     = (float2(tid.xy) + 0.5f) / resolution;
    float jitter  = TemporalHash2D(tid.xy, frame.camPos.w) - 0.5f;
    float depthVS = 0.1f * pow(10000.0f, (float(tid.z) + 0.5f + jitter * 0.5f) / float(Volumetric.Depth));
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

    // --- 1. GLOBAL HEIGHT FOG + NOISE ---
    float boundedY   = max(worldPos.y - fogPC.heightOffset, -10.0f);
    float fogDensity = fogPC.density * exp(-fogPC.heightFalloff * boundedY);

    if (fogPC.enableNoise != 0 && fogDensity > 0.00001f) {
        // Tileable 3D noise is baked at startup; one hardware trilinear fetch
        // replaces three octaves of procedural value noise per voxel.
        float3 noisePos = worldPos * fogPC.noiseScale * (1.0f / 32.0f) +
                          float3(time * fogPC.noiseSpeed * 0.2f, time * fogPC.noiseSpeed * 0.1f, time * fogPC.noiseSpeed * 0.15f);
        float noiseVal  = noiseTexture.SampleLevel(noiseSampler, noisePos, 0).r;
        fogDensity *= lerp(1.0f, noiseVal * 2.0f, fogPC.noiseIntensity);
    }

    float3 scatteringCoeff = fogPC.scatteringColor * fogDensity;
    float3 extinctionCoeff = (fogPC.scatteringColor + fogPC.absorptionColor) * fogDensity;

    // Local Probe Clipping (Indoor/Tunnel Masking)
    if (frame.probeMin.w > 0.0f) {
        float3 center  = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
        float3 extents = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
        float3 dist    = abs(worldPos - center) / max(extents, 0.001f);
        float mask     = saturate(1.0f - max(dist.x, max(dist.y, dist.z)));
        scatteringCoeff *= mask;
        extinctionCoeff *= mask;
    }

    // --- 2. LOCAL FOG VOLUMES INJECTION ---
    for (uint i = 0; i < fogPC.volumeCount; ++i) {
        GPUVolumetricVolume vol = fogVolumes[i];
        float3 localPos         = mul(vol.invTransform, float4(worldPos, 1.0f)).xyz;

        float volMask = 0.0f;
        if (vol.extentsAndType.w == 0.0f) { // Box Volume
            float3 d   = abs(localPos) / vol.extentsAndType.xyz;
            float maxD = max(d.x, max(d.y, d.z));
            if (maxD < 1.0f) {
                volMask = smoothstep(1.0f, 0.8f, maxD);
            }
        } else { // Sphere Volume
            float dist = length(localPos) / vol.extentsAndType.x;
            if (dist < 1.0f) {
                volMask = smoothstep(1.0f, 0.0f, dist);
            }
        }

        if (volMask > 0.0f) {
            float vDensity = vol.colorAndDensity.w * volMask;
            scatteringCoeff += vol.colorAndDensity.xyz * vDensity;
            extinctionCoeff += vol.colorAndDensity.xyz * vDensity;
        }
    }

    float scalarExtinction = dot(extinctionCoeff, float3(0.2126f, 0.7152f, 0.0722f));
    outVoxelMedia[tid]     = float4(scatteringCoeff, scalarExtinction);
}
