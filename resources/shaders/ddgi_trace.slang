// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_push_constants;
import ddgi_common;
import gpu_buffer;
import instance_data;
import pbr_helpers;
import uniforms;

[vk::push_constant]
DdgiTracePushConstants pc;

ConstantBuffer<FrameUniforms>   frame;
StructuredBuffer<InstanceData>  g_instances;
Texture2D<float4>               texDdgiIrradiance;
Texture2D<float2>               texDdgiDistance;
SamplerState                    clampSampler;
RWTexture2D<float4>             outRays;
RaytracingAccelerationStructure tlas;

static const float PI            = 3.14159265358979323846f;
static const float MAX_PROBE_LUM = 16.0f;

float3 ClampProbeFireflies(float3 color, float maxLum) {
    float lum = dot(max(color, float3(0.0f)), float3(0.2126f, 0.7152f, 0.0722f));
    if (lum > maxLum) {
        color *= maxLum / max(lum, 1e-4f);
    }
    return color;
}

float3 DirectSunProbeHit(float3 hitPos, float3 hitNormal, float3 diffuseReflectance) {
    float3 L     = SafeNormalize(frame.lightDir.xyz, float3(0.0f, 1.0f, 0.0f));
    float  nDotL = saturate(dot(hitNormal, L));
    if (nDotL <= 0.0f || all(frame.sunRadiance.xyz <= 0.0f)) {
        return float3(0.0f);
    }

    RayDesc shadowRay;
    shadowRay.Origin    = hitPos + hitNormal * 0.04f;
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
    return diffuseReflectance * frame.sunRadiance.xyz * (nDotL / PI);
}

[shader("compute")]
[numthreads(1, 64, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint probeIndex = tid.x;
    uint rayIndex   = tid.y;
    if (probeIndex >= pc.totalProbes || rayIndex >= pc.raysPerProbe) {
        return;
    }

    DDGIUniforms ddgi       = GetDDGIUniforms(frame);
    int3         probeCoord = GetProbeCoord(probeIndex, ddgi.gridCounts);
    float3       probePos   = GetProbeWorldPos(probeCoord, ddgi);
    float3       rayDir     = GetDDGIRayDirection(rayIndex, ddgi.raysPerProbe, frame.frameIndex);

    RayDesc ray;
    ray.Origin    = probePos;
    ray.Direction = rayDir;
    ray.TMin      = 0.01f;
    ray.TMax      = max(pc.maxRayDistance, ddgi.probeSpacing * 2.5f);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
    }

    float3 radiance    = float3(0.0f);
    float  hitDistance = ray.TMax;

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        hitDistance           = q.CommittedRayT();
        float3       hitPos   = ray.Origin + ray.Direction * hitDistance;
        InstanceData hitInst  = g_instances[q.CommittedInstanceID()];
        float3       hitNormal = -rayDir;

        if (hitInst.posAddress != 0) {
            uint primitive = q.CommittedPrimitiveIndex();
            uint i0        = primitive * 3u;
            uint i1        = i0 + 1u;
            uint i2        = i0 + 2u;
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
            float3x3 world3x3    = (float3x3) hitInst.world;
            float3   crossNormal = cross(mul(world3x3, p1 - p0), mul(world3x3, p2 - p0));
            float    lenSq       = dot(crossNormal, crossNormal);
            if (lenSq > 1e-7f) {
                hitNormal = crossNormal * rsqrt(lenSq);
            }
        }
        if (dot(hitNormal, rayDir) > 0.0f) {
            hitNormal = -hitNormal;
        }

        float3 baseColor = max(hitInst.baseColorFactor.rgb, float3(0.0f));
        float3 emissive  = max(hitInst.emissiveFactor.rgb, float3(0.0f));
        if (hitInst.isUnlit) {
            radiance = baseColor + emissive;
        } else {
            float  metallic           = saturate(hitInst.metallicFactor);
            float3 diffuseReflectance = baseColor * (1.0f - metallic);

            // Direct sun contribution at the probe ray hit
            radiance = DirectSunProbeHit(hitPos, hitNormal, diffuseReflectance) + emissive;

            // Infinite multi-bounce: sample previous frame's DDGI irradiance at the hit point
            // Hysteresis zero marks a fresh/relocated grid. Its atlas still
            // contains the previous grid until the update pass later this
            // frame, so it cannot be used as a multi-bounce source yet.
            if (ddgi.hysteresis > 0.0f) {
                float3 prevBounce = SampleDDGIIrradiance(
                    hitPos, hitNormal, -rayDir, ddgi, texDdgiIrradiance, texDdgiDistance, clampSampler
                );
                radiance += diffuseReflectance * prevBounce * 0.85f;
            }
        }
    } else {
        // Ray escaped to the sky: evaluate baseline environment SH
        radiance = max(EvaluateSH(rayDir, frame.sh) * frame.ambientExposure, float3(0.0f));
    }

    outRays[uint2(probeIndex, rayIndex)] = float4(ClampProbeFireflies(max(radiance, float3(0.0f)), MAX_PROBE_LUM), hitDistance);
}
