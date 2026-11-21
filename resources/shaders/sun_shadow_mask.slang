// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_push_constants;
import pbr_helpers;
import uniforms;

// The sun shadow mask prepass.
//
// Broad scene shadows are Cascade PCSS: an unjittered blocker search plus a
// dithered Poisson PCF, evaluated per lit pixel. That is affordable for one
// pixel and ruinous for a full screen of them. The fragment stage fans out
// across quads that shade together and cannot retire a pixel before the POISSON
// taps are issued, so a deferred full-screen pass pays the whole PCSS cost plus
// the quad's register footprint even where the surface is the caster's own
// straight-on face and always resolves to 1.0 -- and straight-on faces are half
// of a lit frame.
//
// A compute dispatch has no quads to protect: a wave retires a whole tile of
// pixels, and a thread with no sun response -- back-facing, zero-length sun,
// sky -- returns 1.0 without touching the shadow map or the BVH at all. The
// result is one R8_UNORM texel per pixel that the lighting pass multiplies in,
// so its fragment stage is free of both the PCSS fan-out and the divergent
// hardware traversal of the contact ray.
//
// This pass is also where a spatial denoiser for soft ray-traced sun shadows
// belongs: it runs after the depth and normal the G-buffer wrote and before the
// single consumer that reads the mask, so filtering the mask would need no new
// target, no history, and no change to lighting.slang.
//
// Deterministic by construction: the PCSS dither is InterleavedGradientNoise of
// the *pixel centre*, not of gl_FragCoord, so a compute thread at pixel (x, y)
// dithers exactly as the fragment at that pixel did.

[vk::push_constant]
SunShadowPushConstants pc;

Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
ConstantBuffer<FrameUniforms> frame;
Texture2DArray<float> shadowMap;
SamplerComparisonState shadowSampler;
SamplerState pointSampler;
RWTexture2D<float> outSunShadowMask;

#ifndef DISABLE_RTR
RaytracingAccelerationStructure tlas;
#endif

#ifndef DISABLE_RTR
// Short-range (0 -> handoff) sun contact shadow ray along exact L_sun.
// Cascade PCSS owns broad scene shadows and distant soft penumbras with zero
// temporal lag; this unjittered ray closes the Peter-Panning / sub-texel contact
// gap right at caster bases with zero stochastic noise and zero history buffers.
float TraceSunContactShadow(float3 worldPos, float3 N, float3 L_sun, float NdotL_sun, float viewDepth) {
    float contactBias = pc.contactBias.x + pc.contactBias.y * max(viewDepth, 0.0f) + pc.contactBias.z * (1.0f - NdotL_sun);

    RayDesc ray;
    ray.Origin    = worldPos + N * contactBias;
    ray.Direction = L_sun;
    ray.TMin      = 0.005f;
    ray.TMax      = pc.contactRange.y;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
    // Main world only (kRayMaskWorld): Source's scaled skybox world never shadows the map.
    q.TraceRayInline(tlas, RAY_FLAG_NONE, kRayMaskWorld, ray);
    while (q.Proceed()) {
    }

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        return 1.0f;
    }

    // Full contact occlusion over [0 .. contactRange.x] seals all CSM
    // Peter-Panning and grounds thin sub-texel geometry; the smooth handoff to
    // contactRange.y lets Cascade PCSS's distance-hardening penumbra take over
    // seamlessly. The contact term itself then retires across the far fade.
    float rtContact = smoothstep(pc.contactRange.x, pc.contactRange.y, q.CommittedRayT());
    return lerp(rtContact, 1.0f, smoothstep(pc.contactRange.z, pc.contactRange.w, viewDepth));
}
#endif

// The cascade whose ortho volume a view depth lands in. Four split distances,
// compared in order; cascade 0 owns everything below the first split.
uint SelectCascade(float viewDepth) {
    uint cascadeIndex = 0u;
    if (viewDepth > frame.cascadeSplits.x)
        cascadeIndex = 1u;
    if (viewDepth > frame.cascadeSplits.y)
        cascadeIndex = 2u;
    if (viewDepth > frame.cascadeSplits.z)
        cascadeIndex = 3u;
    return cascadeIndex;
}

// Cascade PCSS for one receiver. The tangent-plane slope is derived from the
// cascade matrix, so the depth compared and the depth written are the same
// quantity; the rest is pbr_helpers' CalculateShadowPCSS, unchanged.
float EvaluateCascadeShadow(float3 worldPos, float3 N, float3 L_sun, float NdotL_sun, float viewDepth, float2 pixelPos) {
    const uint cascadeIndex = SelectCascade(viewDepth);

    float4x4 cascadeM         = frame.lightSpaceMatrices[cascadeIndex];
    float3   rowX             = mul(float4(1.0f, 0.0f, 0.0f, 0.0f), cascadeM).xyz;
    float3   rowY             = mul(float4(0.0f, 1.0f, 0.0f, 0.0f), cascadeM).xyz;
    float3   rowZ             = mul(float4(0.0f, 0.0f, 1.0f, 0.0f), cascadeM).xyz;
    float    rowXLenSq        = max(dot(rowX, rowX), 1e-12f);
    float    rowYLenSq        = max(dot(rowY, rowY), 1e-12f);
    float    rowZLen          = max(length(rowZ), 1e-8f);
    float    ndcPerWorldX     = sqrt(rowXLenSq);
    float    texelSizeWorld   = 2.0f / (ndcPerWorldX * float(frame.shadowResolution));
    float    worldDepthPerNdc = 1.0f / rowZLen;

    // Exact receiver tangent-plane slope in [0..1] shadow UV coordinates:
    // e_u, e_v, e_z are the world-space vectors for +1 step in (u, v, z_ndc).
    float3   eu             = (2.0f / rowXLenSq) * rowX;
    float3   ev             = (2.0f / rowYLenSq) * rowY;
    float3   wHat           = rowZ * worldDepthPerNdc;
    float    nDotW          = dot(N, wHat);
    float    safeNDotW      = (nDotW < 0.0f ? -1.0f : 1.0f) * max(abs(nDotW), 0.15f);
    float    denomEz        = safeNDotW * worldDepthPerNdc;
    float2   dzDuv          = -float2(dot(N, eu), dot(N, ev)) / denomEz;

    float    normalBias     = saturate(1.0f - NdotL_sun) * 1.25f + 0.35f;
    float3   biasedWorldPos = worldPos + N * (normalBias * texelSizeWorld);

    float4 shadowPos = mul(cascadeM, float4(biasedWorldPos, 1.0f));

    shadowPos.xy = shadowPos.xy * 0.5f + 0.5f * shadowPos.w;

    return CalculateShadowPCSS(
        shadowPos, N, L_sun, pixelPos, shadowMap, shadowSampler, pointSampler, frame.shadowResolution, cascadeIndex, worldDepthPerNdc,
        texelSizeWorld, dzDuv, frame.sunSize, false
    );
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint maskWidth, maskHeight;
    outSunShadowMask.GetDimensions(maskWidth, maskHeight);
    if (any(tid.xy >= uint2(maskWidth, maskHeight)))
        return;

    // A thread that bails below leaves this: the sun reaches a pixel with no
    // shadowing term, which is what a lit pixel with no caster there means.
    outSunShadowMask[tid.xy] = 1.0f;

    const uint2 pixel = tid.xy;
    const float depth = texDepth.Load(int3(pixel, 0));
    // One world, one depth range: 1.0 still means no layer rasterized the pixel.
    if (depth >= 1.0f)
        return;

    const float3 normal    = UnpackNormalOctahedron(texNormalRoughness.Load(int3(pixel, 0)).xy * 2.0f - 1.0f);
    const float2 uv        = (float2(pixel) + 0.5f) / max(frame.screenResolution, float2(1.0f, 1.0f));
    const float3 worldPos  = ReconstructWorldPos(uv, depth, pc.invViewProj);
    const float  viewDepth = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;

    const float3 L_sun     = SafeNormalize(frame.lightDir.xyz, float3(0.0f, 1.0f, 0.0f));
    const float  NdotL_sun = saturate(dot(normal, L_sun));
    // Sun response lower bound. The lighting pass owns the exact specular and
    // diffuse response and still scales this mask by it; all this thread needs
    // to know is whether the pixel can receive sun at all. A back-facing normal
    // or a disabled sun cannot, and `0 * shadow` would be 0, so neither the
    // shadow map nor the BVH is ever consulted for such a pixel.
    if (NdotL_sun <= 1e-4f || frame.lightDir.w <= 0.0f)
        return;

    float shadow = EvaluateCascadeShadow(worldPos, normal, L_sun, NdotL_sun, viewDepth, float2(pixel) + 0.5f);

#ifndef DISABLE_RTR
    // Pixels already in Cascade PCSS umbra (`shadow <= 0.001f`) skip the hardware
    // ray completely: `0 * rtContact == 0`.
    if (frame.enableRTR != 0 && shadow > 0.001f && viewDepth < pc.contactRange.w) {
        shadow *= TraceSunContactShadow(worldPos, normal, L_sun, NdotL_sun, viewDepth);
    }
#endif

    outSunShadowMask[pixel] = saturate(shadow);
}
