// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_buffer;
import instance_data;
import pbr_helpers;
import uniforms;

struct RtgiHalfPushConstants {
    uint2 halfRes;
    uint2 _pad;
};
[vk::push_constant]
RtgiHalfPushConstants pc;

Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
Texture2D<float4> texAnisotropy;
Texture2D<float4> texLighting;
ConstantBuffer<FrameUniforms> frame;
StructuredBuffer<InstanceData> g_instances;
Texture2D<float4> blueNoiseTex;
SamplerState blueNoiseSampler;
RWTexture2D<float4> outImage;
RaytracingAccelerationStructure tlas;

static const float PI = 3.14159265358979323846f;

float3 DirectSunFallback(float3 hitPos, float3 hitNormal, float3 baseColor, float3 diffuseWeight) {
    float3 L = SafeNormalize(frame.lightDir.xyz, float3(0.0f, 1.0f, 0.0f));
    float nDotL = saturate(dot(hitNormal, L));
    if (nDotL <= 0.0f || all(frame.sunRadiance.xyz <= 0.0f)) {
        return float3(0.0f);
    }

    RayDesc shadowRay;
    shadowRay.Origin    = hitPos + hitNormal * 0.035f;
    shadowRay.Direction = L;
    shadowRay.TMin      = 0.01f;
    shadowRay.TMax      = 300.0f;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> shadowQuery;
    shadowQuery.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, shadowRay);
    while (shadowQuery.Proceed()) {
    }

    if (shadowQuery.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        return float3(0.0f);
    }
    return baseColor * diffuseWeight * frame.sunRadiance.xyz * (nDotL / PI);
}

float3 FallbackHitRadiance(InstanceData hitInst, float3 hitPos, float3 hitNormal, float3 viewDirection) {
    float3 baseColor = max(hitInst.baseColorFactor.rgb, float3(0.0f));
    float3 emissive  = max(hitInst.emissiveFactor.rgb, float3(0.0f));
    if (hitInst.isUnlit) {
        return baseColor + emissive;
    }

    float metallic = saturate(hitInst.metallicFactor);
    float roughness = saturate(hitInst.roughnessFactor);
    float3 F0 = lerp(float3(0.04f), baseColor, metallic);
    float3 diffuseWeight = (1.0f - FresnelSchlickRoughness(saturate(dot(hitNormal, viewDirection)), F0, roughness)) * (1.0f - metallic);
    float3 ambientIrradiance = EvaluateSH(hitNormal, frame.sh) * frame.ambientExposure;
    float3 outgoing = baseColor * diffuseWeight * ambientIrradiance;
    outgoing += DirectSunFallback(hitPos, hitNormal, baseColor, diffuseWeight);
    return outgoing + emissive;
}

bool TryReadVisibleLighting(float3 hitPos, float3 hitNormal, out float3 radiance) {
    radiance = float3(0.0f);
    float4 hitClipPos = mul(frame.viewProj, float4(hitPos, 1.0f));
    if (hitClipPos.w <= 0.1f) {
        return false;
    }

    float2 uv = hitClipPos.xy / hitClipPos.w * 0.5f + 0.5f;
    if (any(uv < 0.0f) || any(uv >= 1.0f)) {
        return false;
    }

    uint2 dimensions = uint2(frame.screenResolution);
    uint2 pixel = min(uint2(uv * float2(dimensions)), dimensions - 1u);
    float sampledDepth = texDepth.Load(int3(pixel, 0));
    if (sampledDepth >= 1.0f) {
        return false;
    }

    float4 linearClip = mul(frame.invProj, float4(0.0f, 0.0f, sampledDepth, 1.0f));
    float sampledViewDepth = abs(linearClip.z / max(abs(linearClip.w), 1e-6f));
    float maxDepthDiff = max(0.04f, hitClipPos.w * 0.015f);
    if (abs(hitClipPos.w - sampledViewDepth) > maxDepthDiff) {
        return false;
    }

    float4 hitNormalRaw = texNormalRoughness.Load(int3(pixel, 0));
    float3 screenNormal = UnpackNormalOctahedron(hitNormalRaw.xy * 2.0f - 1.0f);
    if (dot(screenNormal, hitNormal) < 0.45f) {
        return false;
    }

    radiance = max(texLighting.Load(int3(pixel, 0)).rgb, float3(0.0f));
    return true;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (any(tid.xy >= pc.halfRes)) {
        return;
    }
    outImage[tid.xy] = float4(0.0f);

    if (frame.fullBright != 0) {
        return;
    }

    uint2 fullPos = min(tid.xy * 2u, uint2(frame.screenResolution) - 1u);
    float depth = texDepth.Load(int3(fullPos, 0));
    if (depth >= 1.0f) {
        return;
    }

    float4 anisoRaw = texAnisotropy.Load(int3(fullPos, 0));
    if (IsGBufferUnlit(anisoRaw.z)) {
        return;
    }

    float4 normalRaw = texNormalRoughness.Load(int3(fullPos, 0));
    float3 N = UnpackNormalOctahedron(normalRaw.xy * 2.0f - 1.0f);
    float2 uv = (float2(fullPos) + 0.5f) / frame.screenResolution;
    float3 worldPos = ReconstructWorldPos(uv, depth, frame.invViewProj);

    float4 random = SampleBlueNoiseAt(
        blueNoiseTex, blueNoiseSampler, fullPos, FrameIndexFromCamPosW(frame.camPos.w), 0u
    );
    float radius = sqrt(saturate(random.z));
    float angle = 2.0f * PI * random.w;
    float3 localDir = float3(radius * cos(angle), radius * sin(angle), sqrt(saturate(1.0f - random.z)));

    float3 up = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
    float3 tangent = SafeNormalize(cross(up, N), float3(1.0f, 0.0f, 0.0f));
    float3 bitangent = cross(N, tangent);
    float3 rayDirection = SafeNormalize(tangent * localDir.x + bitangent * localDir.y + N * localDir.z, N);

    RayDesc ray;
    ray.Origin    = worldPos + N * 0.025f;
    ray.Direction = rayDirection;
    ray.TMin      = 0.01f;
    ray.TMax      = 100.0f;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed()) {
    }

    float3 radiance = float3(0.0f);
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float rayT = query.CommittedRayT();
        float3 hitPos = ray.Origin + ray.Direction * rayT;
        float3 hitNormal = float3(0.0f, 1.0f, 0.0f);

        InstanceData hitInst = g_instances[query.CommittedInstanceID()];
        if (hitInst.posAddress != 0) {
            uint primitive = query.CommittedPrimitiveIndex();
            uint i0 = primitive * 3u;
            uint i1 = i0 + 1u;
            uint i2 = i0 + 2u;
            if (hitInst.iboAddress != 0) {
                GPUBuffer<uint> indices = GPUBuffer<uint>(hitInst.iboAddress);
                i0 = indices.load(i0);
                i1 = indices.load(i1);
                i2 = indices.load(i2);
            }

            GPUBuffer<float> positions = GPUBuffer<float>(hitInst.posAddress);
            float3 p0 = float3(positions.load(i0 * 3u), positions.load(i0 * 3u + 1u), positions.load(i0 * 3u + 2u));
            float3 p1 = float3(positions.load(i1 * 3u), positions.load(i1 * 3u + 1u), positions.load(i1 * 3u + 2u));
            float3 p2 = float3(positions.load(i2 * 3u), positions.load(i2 * 3u + 1u), positions.load(i2 * 3u + 2u));
            float3x3 world3x3 = (float3x3)hitInst.world;
            float3 crossNormal = cross(mul(world3x3, p1 - p0), mul(world3x3, p2 - p0));
            float normalLengthSquared = dot(crossNormal, crossNormal);
            if (normalLengthSquared > 1e-7f) {
                hitNormal = crossNormal * rsqrt(normalLengthSquared);
            }
        }
        if (dot(hitNormal, ray.Direction) > 0.0f) {
            hitNormal = -hitNormal;
        }

        if (!TryReadVisibleLighting(hitPos, hitNormal, radiance)) {
            radiance = FallbackHitRadiance(hitInst, hitPos, hitNormal, -ray.Direction);
        }
    } else {
        // Clustered lighting already supplies baseline SH sky and direct sun.
        // An escaped GI ray must not add that environment a second time; only
        // geometry hits contribute the diffuse bounce here.
        radiance = float3(0.0f);
    }

    outImage[tid.xy] = float4(max(radiance, float3(0.0f)), 1.0f);
}
