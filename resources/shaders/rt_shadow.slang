// resources/shaders/rt_shadow.slang
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Dedicated ray-traced sun shadow mask.
//
// The lighting pass used to trace its shadow ray inline, one ray per fragment,
// and write the resulting 1-bit visibility straight into the shading term. That
// has two consequences this pass exists to remove:
//
//  1. A binary value cannot be filtered where it is produced -- the only
//     smoothing available was TAA, which also has to resolve geometry edges and
//     motion. Any spatial filter needs a *mask* to work on.
//  2. Cost is paid per shaded fragment rather than once per pixel, and cannot
//     be traded against a denoiser budget.
//
// So this pass evaluates visibility into an R8 target at 1-2 SPP with
// blue-noise/Vogel-disk jitter, and shadow_denoise_atrous.slang smooths it
// before lighting reads a single load instead of a ray.
//
// Writes 1.0 for fully lit, 0.0 for fully shadowed, so the lighting pass can
// multiply it straight into the sun term.

import instance_data;
import pbr_helpers;
import uniforms;

// Only the knobs the host actually varies per quality tier. Everything else
// (invViewProj, lightDir, sunSize, screenResolution, camPos) already lives in
// FrameUniforms, so duplicating it here would just create a second source of
// truth for the same values.
struct RTShadowPushConstants {
    uint samples;        // SPP for this frame (RayTracingConfig::shadowSamples)
    uint enableAlphaTest; // non-zero -> evaluate cutout masks on candidates
    float normalBias;    // world units of N offset to avoid self-shadowing
    float maxDistance;   // ray TMax; longer rays cost traversal, not quality
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

// Cutout alpha lookup. This is the engine's bindless array: the host must map
// it onto the same reserved slot range WriteTextureSlotToHeap writes, so
// inst.albedoIndex resolves to the same textures the raster path sees.
Texture2D globalTextures[];

// Blue noise tile for the sun-disk jitter (NEAREST + REPEAT sampler).
Texture2D<float4> blueNoiseTex;
SamplerState blueNoiseSampler;

/**
 * @brief Visibility of one ray, honouring cutout alpha when enabled.
 *
 * Opaque-only tracing turns every alpha-tested card -- foliage, hair, chain
 * link -- into a solid rectangle of shadow: the BVH reports the triangle, the
 * ray stops, and the transparent part of the card occludes like the opaque
 * part. Dropping RAY_FLAG_FORCE_OPAQUE makes traversal hand back
 * CANDIDATE_NON_OPAQUE_TRIANGLE hits so the mask can be evaluated and the
 * transparent parts rejected.
 *
 * Two details that differ from the opaque path:
 *
 *  - No RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH. That flag makes traversal
 *    auto-commit the first candidate and never return control, so there is no
 *    opportunity to evaluate a mask; without it the query keeps walking to the
 *    closest *committed* hit, which is what alpha testing needs anyway.
 *  - The commit call is Slang's CommitNonOpaqueTriangleHit(), not the HLSL/DXR
 *    spelling ConfirmIntersection(). Slang maps its own name onto
 *    OpRayQueryConfirmIntersectionKHR for SPIR-V; the HLSL name is not a member
 *    of RayQuery<T> here and fails to compile.
 *
 * The geometry only reaches this path if its BLAS was built without
 * VK_GEOMETRY_OPAQUE_BIT_KHR (RayTracingConfig::alphaTestingInBVH); an
 * opaque-flagged geometry is never offered as a candidate.
 */
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

        // Geometry flagged non-opaque in the BVH but not actually masked
        // (blend modes, decals) still arrives as a candidate; it occludes
        // fully and needs no texture fetch.
        if (inst.alphaMode != 1u) {
            q.CommitNonOpaqueTriangleHit();
            continue;
        }

        // Rebuild the hit UV from the mesh's own vertex stream. Barycentric
        // interpolation happens in object space, so the instance matrix never
        // applies and no transform is needed.
        uint primIndex = q.CandidatePrimitiveIndex();
        float2 bary    = q.CandidateTriangleBarycentrics();
        uint i0        = inst.fetchIndex(primIndex * 3u + 0u);
        uint i1        = inst.fetchIndex(primIndex * 3u + 1u);
        uint i2        = inst.fetchIndex(primIndex * 3u + 2u);

        float2 uv0 = UnpackHalf2(inst.fetchAttributes(i0).uv);
        float2 uv1 = UnpackHalf2(inst.fetchAttributes(i1).uv);
        float2 uv2 = UnpackHalf2(inst.fetchAttributes(i2).uv);
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
        // Sky: nothing to shadow, and tracing from a reconstructed position at
        // the far plane would only generate garbage rays.
        outShadow[tid.xy] = 1.0f;
        return;
    }

    float2 uv       = (float2(tid.xy) + 0.5f) / float2(frame.screenResolution);
    float3 worldPos = ReconstructWorldPos(uv, depth, frame.invViewProj);

    float4 normRoughRaw = texNormalRoughness.Load(int3(tid.xy, 0));
    float3 N            = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);
    float3 L            = normalize(frame.lightDir.xyz);

    // Back-facing surfaces get full visibility: the cascade path already
    // handles their NdotL <= 0 case and tracing into them only produces acne.
    if (dot(N, L) <= 0.0f) {
        outShadow[tid.xy] = 1.0f;
        return;
    }

    float3 up        = abs(L.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
    float3 right     = normalize(cross(up, L));
    float3 bitangent = cross(L, right);

    uint frameIdx = FrameIndexFromCamPosW(frame.camPos.w);
    uint spp      = max(pc.samples, 1u);
    float spread  = max(frame.sunSize, 0.00001f);
    float3 origin = worldPos + N * max(pc.normalBias, 0.0f);
    bool alphaTest = pc.enableAlphaTest != 0u;

    float visibility = 0.0f;
    for (uint i = 0u; i < spp; ++i) {
        float4 u    = SampleBlueNoiseAt(blueNoiseTex, blueNoiseSampler, tid.xy, frameIdx, i);
        // Vogel disk: uniform in area over the sun's angular radius, so even a
        // 1 SPP estimate is unbiased across the disk rather than sampling one
        // fixed direction per dither angle.
        float2 disk = VogelDisk(i, spp, u.x * 6.2831853f) * spread;

        float3 dir = normalize(L + right * disk.x + bitangent * disk.y);
        visibility += TraceVisibility(origin, dir, pc.maxDistance, alphaTest);
    }

    outShadow[tid.xy] = visibility / float(spp);
}
