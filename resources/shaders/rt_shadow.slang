// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


import instance_data;
import vertex_format;
import pbr_helpers;
import uniforms;

struct RTShadowPushConstants {
    uint samples;
    uint enableAlphaTest;
    float normalBias;
    float maxDistance;
};

[vk::push_constant]
RTShadowPushConstants pc;

Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
RWTexture2D<float> outShadow;
ConstantBuffer<FrameUniforms> frame;
SamplerState pointSampler;

RaytracingAccelerationStructure tlas;
StructuredBuffer<InstanceData> g_instances;

Texture2D globalTextures[];

Texture2D<float4> blueNoiseTex;
SamplerState blueNoiseSampler;

float TraceVisibility(float3 origin, float3 direction, float tMax, bool alphaTest) {
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = 0.01f;
    ray.TMax      = tMax;

    if (!alphaTest) {
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
        q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
        while (q.Proceed()) {
        }
        return q.CommittedStatus() != COMMITTED_TRIANGLE_HIT ? 1.0f : 0.0f;
    }

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) {
            continue;
        }

        uint instanceID  = q.CandidateInstanceID();
        InstanceData inst = g_instances[instanceID];

        if (inst.alphaMode != 1u) {
            q.CommitNonOpaqueTriangleHit();
            continue;
        }

        uint primIndex = q.CandidatePrimitiveIndex();
        float2 bary    = q.CandidateTriangleBarycentrics();
        uint i0        = inst.fetchIndex(primIndex * 3u + 0u);
        uint i1        = inst.fetchIndex(primIndex * 3u + 1u);
        uint i2        = inst.fetchIndex(primIndex * 3u + 2u);

        float2 uv0 = inst.fetchSurface(i0).unpackedUV;
        float2 uv1 = inst.fetchSurface(i1).unpackedUV;
        float2 uv2 = inst.fetchSurface(i2).unpackedUV;
        float2 uv  = uv0 * (1.0f - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;

        float alpha = globalTextures[inst.albedoIndex].SampleLevel(pointSampler, uv, 0.0f).a * inst.baseColorFactor.a;
        if (alpha >= inst.alphaCutoff) {
            q.CommitNonOpaqueTriangleHit();
        }
    }

    return q.CommittedStatus() != COMMITTED_TRIANGLE_HIT ? 1.0f : 0.0f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (any(tid.xy >= uint2(frame.screenResolution))) {
        return;
    }

    float depth = texDepth.Load(int3(tid.xy, 0)).r;
    if (depth >= 1.0f) {
        outShadow[tid.xy] = 1.0f;
        return;
    }

    float2 uv       = (float2(tid.xy) + 0.5f) / float2(frame.screenResolution);
    float3 worldPos = ReconstructWorldPos(uv, depth, frame.invViewProj);

    float4 normRoughRaw = texNormalRoughness.Load(int3(tid.xy, 0));
    float3 N            = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);
    float3 L            = SafeNormalize(frame.lightDir.xyz, float3(0.0f, 1.0f, 0.0f));

    if (dot(N, L) <= 0.0f) {
        outShadow[tid.xy] = 1.0f;
        return;
    }

    float3 up        = abs(L.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
    float3 right     = SafeNormalize(cross(up, L), float3(1.0f, 0.0f, 0.0f));
    float3 bitangent = cross(L, right);

    uint frameIdx = FrameIndexFromCamPosW(frame.camPos.w);
    uint spp      = max(pc.samples, 1u);
    float spread  = max(frame.sunSize, 0.00001f);
    float3 origin = worldPos + N * max(pc.normalBias, 0.0f);
    bool alphaTest = pc.enableAlphaTest != 0u;

    float visibility = 0.0f;
    for (uint i = 0u; i < spp; ++i) {
        float4 u    = SampleBlueNoiseAt(blueNoiseTex, blueNoiseSampler, tid.xy, frameIdx, i);
        float2 disk = VogelDisk(i, spp, u.x * 6.2831853f) * spread;

        float3 dir = SafeNormalize(L + right * disk.x + bitangent * disk.y, L);
        visibility += TraceVisibility(origin, dir, pc.maxDistance, alphaTest);
    }

    outShadow[tid.xy] = visibility / float(spp);
}
