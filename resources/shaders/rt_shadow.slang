// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_push_constants;
import pbr_helpers;
import uniforms;

[vk::push_constant]
RTShadowPushConstants pc;

Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
RWTexture2D<float> outShadow;
ConstantBuffer<FrameUniforms> frame;

RaytracingAccelerationStructure tlas;

Texture2D<float4> blueNoiseTex;
SamplerState blueNoiseSampler;

// Must match kShadowTraceDivisor in TargetManager.hpp. outShadow is traced below
// native resolution, so a dispatch coordinate names a trace texel while every
// guide this shader reads is a native pixel. Tracing without that mapping makes
// each trace texel hold the shadow of the native pixel at its own coordinate --
// i.e. the top-left quadrant's shadow field, which the lighting pass then reads
// back as if it covered the whole screen. The denoiser is handed a signal and a
// set of guides describing different pixels, so its bilateral weights reject
// every neighbour and the raw throwaway noise reaches the screen. Same mapping
// as rtgi_half.slang.
static const uint kShadowTraceScale = 2u;

float TraceVisibility(float3 origin, float3 direction, float tMax) {
    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = direction;
    ray.TMin      = 0.01f;
    ray.TMax      = tMax;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
    }
    return q.CommittedStatus() != COMMITTED_TRIANGLE_HIT ? 1.0f : 0.0f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint traceW, traceH;
    outShadow.GetDimensions(traceW, traceH);
    if (any(tid.xy >= uint2(traceW, traceH))) {
        return;
    }

    uint2 fullPos = min(tid.xy * kShadowTraceScale, uint2(frame.screenResolution) - 1u);
    float depth   = texDepth.Load(int3(fullPos, 0)).r;
    if (depth >= 1.0f) {
        outShadow[tid.xy] = 1.0f;
        return;
    }

    float2 uv       = (float2(fullPos) + 0.5f) / float2(frame.screenResolution);
    float3 worldPos = ReconstructWorldPos(uv, depth, frame.invViewProj);

    float viewDepth = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;
    if (viewDepth >= 160.0f) {
        outShadow[tid.xy] = 1.0f;
        return;
    }

    float4 normRoughRaw = texNormalRoughness.Load(int3(fullPos, 0));
    float3 N            = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);
    float3 L            = SafeNormalize(frame.lightDir.xyz, float3(0.0f, 1.0f, 0.0f));

    if (dot(N, L) <= 0.0f) {
        outShadow[tid.xy] = 1.0f;
        return;
    }

    float3 up        = abs(L.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
    float3 right     = SafeNormalize(cross(up, L), float3(1.0f, 0.0f, 0.0f));
    float3 bitangent = cross(L, right);

    uint   frameIdx = WrapFrameIndex(frame.frameIndex);
    uint   spp      = max(pc.samples, 1u);
    float  spread   = clamp(frame.sunSize, 0.00001f, 0.25f);
    float3 origin   = worldPos + N * max(pc.normalBias, 0.0f);

    float visibility = 0.0f;
    for (uint i = 0u; i < spp; ++i) {
        float4 u    = SampleBlueNoiseAt(blueNoiseTex, blueNoiseSampler, fullPos, frameIdx, i);
        float2 disk = VogelDisk(i, spp, u.x * 6.2831853f) * spread;

        float3 dir = SafeNormalize(L + right * disk.x + bitangent * disk.y, L);
        visibility += TraceVisibility(origin, dir, pc.maxDistance);
    }

    outShadow[tid.xy] = visibility / float(spp);
}
