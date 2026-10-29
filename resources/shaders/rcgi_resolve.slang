// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_push_constants;
import pbr_helpers;
import rcgi_common;
import uniforms;

[vk::push_constant]
RcgiResolvePushConstants pc;

Texture2D<float4>   texLighting;
Texture2D<float>    texLightingAo;
Texture2D<float4>   texAlbedo;
Texture2D<float>    texDepth;
Texture2D<float4>   texNormalRoughness;
Texture2D<float4>   inProbes;
Texture2D<float4>   inCascade0;
RWTexture2D<float4> outLighting;

ConstantBuffer<FrameUniforms> frame;

// Cooperative 4x4 C0 probe neighborhood cache for an 8x8 full-res pixel block:
//   16 probe headers  (256 B)
//   16 probes x 16 C0 cones (4,096 B) -> exact 4 global loads per thread!
groupshared float4 s_c0Probes[16];
groupshared float4 s_c0Cones[16][16];

// Precomputed 16 C0 octahedral unit directions so inner loops avoid repeated unpacking.
groupshared float3 s_c0Dirs[16];

float ComputeCMinus1ContactVisibility(int2 pixel, float3 worldPos, float3 N) {
    if (pc.cMinus1Radius <= 1e-4f || pc.fullRes.x == 0u || pc.fullRes.y == 0u)
        return 1.0f;

    float3 upRef = (abs(N.z) < 0.99f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 T     = SafeNormalize(cross(upRef, N), float3(1.0f, 0.0f, 0.0f));
    float3 B     = cross(N, T);

    float3 dirs[4] = {
        normalize( T * 0.65f + B * 0.25f + N * 0.71f),
        normalize(-T * 0.25f + B * 0.65f + N * 0.71f),
        normalize(-T * 0.65f - B * 0.25f + N * 0.71f),
        normalize( T * 0.25f - B * 0.65f + N * 0.71f)
    };

    float3 startPos  = worldPos + N * 0.01f;
    float  thickness = pc.cMinus1Radius * 1.25f;
    float  occSum    = 0.0f;

    [ForceUnroll]
    for (uint d = 0u; d < 4u; ++d) {
        float occluded = 0.0f;
        [ForceUnroll]
        for (uint s = 1u; s <= 3u; ++s) {
            float  t      = pc.cMinus1Radius * (float(s) * (1.0f / 3.0f));
            float3 rayPos = startPos + dirs[d] * t;
            float4 clip   = mul(frame.unjitteredViewProj, float4(rayPos, 1.0f));
            if (clip.w > 1e-4f) {
                float2 uv  = clip.xy / clip.w * 0.5f + 0.5f;
                float  ndc = clip.z / clip.w;
                if (all(uv >= 0.0f) && all(uv <= 1.0f) && ndc > 0.0f && ndc < 1.0f) {
                    int2  sPx = clamp(int2(uv * float2(pc.fullRes)), int2(0, 0), int2(pc.fullRes) - int2(1, 1));
                    if (any(sPx != pixel)) {
                        float sDepth = texDepth.Load(int3(sPx, 0));
                        if (sDepth < kSkyDepthSplit && sDepth < ndc - 1e-4f) {
                            float3 surfPos = ReconstructWorldPos((float2(sPx) + 0.5f) / float2(pc.fullRes), sDepth, frame.invViewProj);
                            float  gap     = length(surfPos - rayPos);
                            if (gap < thickness) {
                                occluded = max(occluded, 1.0f - saturate(gap / thickness));
                            }
                        }
                    }
                }
            }
        }
        occSum += occluded;
    }

    return lerp(1.0f, 1.0f - (occSum * 0.25f), 0.45f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID, uint3 gid: SV_GroupID, uint3 gtid: SV_GroupThreadID) {
    // Workgroup-uniform fast passthrough when GI is disabled or fullBright is active:
    // skips all LDS preloads, barriers, and reads of transient cascade targets.
    if (pc.giIntensity <= 0.0f || frame.fullBright != 0) {
        if (tid.x < pc.fullRes.x && tid.y < pc.fullRes.y) {
            int2   px        = int2(tid.xy);
            float4 base      = texLighting.Load(int3(px, 0));
            float  geomAlpha = (texDepth.Load(int3(px, 0)) < kSkyDepthSplit) ? 1.0f : 0.0f;
            outLighting[px]  = float4(IsFiniteFloat3(base.rgb) ? max(base.rgb, float3(0.0f)) : float3(0.0f), geomAlpha);
        }
        return;
    }

    uint  lidx       = gtid.y * 8u + gtid.x;
    uint2 c0ProbeDim = RcgiProbeDim(0u, pc.alignedRes);
    uint2 c0DirDim   = RcgiDirDim(0u);
    uint2 c0Offset   = RcgiProbeAtlasOffset(0u, pc.alignedRes);
    int2  tileOrigin = int2(gid.xy) * 2 - int2(1, 1);

    // 1. Cooperative LDS preload (strictly uniform across all 64 threads before barrier).
    if (lidx < 16u) {
        int2  localP  = int2(int(lidx & 3u), int(lidx >> 2u));
        uint2 globalP = uint2(clamp(tileOrigin + localP, int2(0, 0), int2(c0ProbeDim) - int2(1, 1)));
        s_c0Probes[lidx] = inProbes.Load(int3(int2(c0Offset + globalP), 0));

        uint2 dirCoord  = uint2(lidx & 3u, lidx >> 2u);
        s_c0Dirs[lidx]  = RcgiDecodeDir(dirCoord, c0DirDim);
    }

    [ForceUnroll]
    for (uint k = 0u; k < 4u; ++k) {
        uint  flatIdx   = lidx * 4u + k;
        uint  pIdx      = flatIdx >> 4u;
        uint  dIdx      = flatIdx & 15u;
        int2  localP    = int2(int(pIdx & 3u), int(pIdx >> 2u));
        uint2 globalP   = uint2(clamp(tileOrigin + localP, int2(0, 0), int2(c0ProbeDim) - int2(1, 1)));
        uint2 dirCoord  = uint2(dIdx & 3u, dIdx >> 2u);
        int2  atlasPx   = RcgiAtlasCoord(dirCoord, globalP, c0ProbeDim);
        s_c0Cones[pIdx][dIdx] = inCascade0.Load(int3(atlasPx, 0));
    }

    GroupMemoryBarrierWithGroupSync();

    // 2. Bounds check AFTER group barrier to maintain SPIR-V barrier uniformity.
    if (tid.x >= pc.fullRes.x || tid.y >= pc.fullRes.y)
        return;

    int2   px           = int2(tid.xy);
    float4 baseLighting = texLighting.Load(int3(px, 0));
    if (!IsFiniteFloat3(baseLighting.rgb)) {
        baseLighting.rgb = float3(0.0f);
    }

    float depth = texDepth.Load(int3(px, 0));
    if (depth >= kSkyDepthSplit || frame.fullBright != 0 || pc.giIntensity <= 0.0f) {
        float geomAlpha = (depth < kSkyDepthSplit) ? 1.0f : 0.0f;
        outLighting[px] = float4(baseLighting.rgb, geomAlpha);
        return;
    }

    float2 uv          = (float2(tid.xy) + 0.5f) / float2(pc.fullRes);
    float4 albedoRaw   = texAlbedo.Load(int3(px, 0));
    float4 normRough   = texNormalRoughness.Load(int3(px, 0));
    float3 albedo      = saturate(albedoRaw.rgb);
    float3 N           = UnpackNormalOctahedron(normRough.xy * 2.0f - 1.0f);
    float  roughness   = saturate(normRough.z);
    float  metallic    = saturate(normRough.w);
    float3 worldPos    = ReconstructWorldPos(uv, depth, frame.invViewProj);

    float  camDist        = length(worldPos - frame.camPos.xyz);
    float  c0WorldSpacing = camDist * float(kRcgiBaseProbeSpacing) * 2.5f / max(float(pc.fullRes.x), 1.0f);
    float  bilateralScale = max(c0WorldSpacing * max(pc.depthRejectScale, 0.25f), 0.15f);

    // Precompute per-pixel hemisphere cosine weights once (16 dot products total instead of 256),
    // then gather weighted C0 probes first and integrate the blended cone distribution once!
    float cosWeights[16];
    float cosWeightSum = 0.0f;
    [ForceUnroll]
    for (uint d = 0u; d < 16u; ++d) {
        float cw = max(dot(s_c0Dirs[d], N), 0.0f);
        cosWeights[d] = cw;
        cosWeightSum += cw;
    }

    // 3. Bilateral 4x4 C0 Gaussian gather in LDS against full-resolution per-pixel normal N:
    // Uses all 16 cooperatively prefetched C0 probes (zero extra VRAM reads!) to produce
    // C-infinity smooth gradients on flat/curved surfaces while hard-stopping across
    // 90-degree corners, creases, and depth silhouettes at 1-pixel full-res precision.
    float2 c0Cont = (float2(tid.xy) + 0.5f) * 0.25f - 0.5f;
    float  probeW[16];
    float  weightSum = 0.0f;

    [ForceUnroll]
    for (uint pIdx = 0u; pIdx < 16u; ++pIdx) {
        int2      localCoord = int2(int(pIdx & 3u), int(pIdx >> 2u));
        float2    probeCoord = float2(tileOrigin + localCoord);
        float2    diff       = probeCoord - c0Cont;
        float     spatialW   = exp(-dot(diff, diff) * 0.70f);
        RcgiProbe probe      = UnpackRcgiProbe(s_c0Probes[pIdx]);
        float     w          = spatialW * RcgiBilateralProbeWeight(worldPos, N, probe, bilateralScale);
        probeW[pIdx] = w;
        weightSum   += w;
    }

    // Fallback for sub-4px isolated thin geometry where all 16 probes sat on another plane
    if (weightSum <= 1e-5f) {
        [ForceUnroll]
        for (uint pIdx = 0u; pIdx < 16u; ++pIdx) {
            RcgiProbe probe = UnpackRcgiProbe(s_c0Probes[pIdx]);
            float     w     = 0.0f;
            if (probe.valid && dot(N, probe.normal) > -0.1f) {
                int2   localCoord = int2(int(pIdx & 3u), int(pIdx >> 2u));
                float2 diff       = float2(tileOrigin + localCoord) - c0Cont;
                w = exp(-dot(diff, diff) * 0.5f);
            }
            probeW[pIdx] = w;
            weightSum   += w;
        }
    }

    float4 resolvedGi = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (weightSum > 1e-6f && cosWeightSum > 1e-5f) {
        float  invProbeW = 1.0f / weightSum;
        float  invCosW   = 1.0f / cosWeightSum;
        float3 radSum    = float3(0.0f);
        float  skyVisSum = 0.0f;

        [ForceUnroll]
        for (uint d = 0u; d < 16u; ++d) {
            float cw = cosWeights[d];
            if (cw > 0.0f) {
                float4 blendedCone = float4(0.0f);
                [ForceUnroll]
                for (uint pIdx = 0u; pIdx < 16u; ++pIdx) {
                    float pw = probeW[pIdx];
                    if (pw > 1e-5f) {
                        blendedCone += s_c0Cones[pIdx][d] * pw;
                    }
                }
                blendedCone *= invProbeW;
                radSum      += blendedCone.rgb * cw;
                skyVisSum   += blendedCone.a * cw;
            }
        }
        resolvedGi = float4(radSum * invCosW, saturate(skyVisSum * invCosW));
    }
    float3 incidentIrrOverPi = max(resolvedGi.rgb, float3(0.0f));
    float  skyTransmittance  = saturate(resolvedGi.a);

    // 5. Sub-probe C(-1) screen-space micro-interval contact occlusion.
    float microVis   = ComputeCMinus1ContactVisibility(px, worldPos, N);
    float gtao       = saturate(texLightingAo.Load(int3(px, 0)));
    float combinedAo = lerp(1.0f, gtao * microVis, 0.65f);

    float3 V         = SafeNormalize(frame.camPos.xyz - worldPos, N);
    float  NdotV     = saturate(dot(N, V));
    float  reflScale = saturate(albedoRaw.a);
    float3 F0        = lerp(float3(kDielectricF0), albedo, metallic) * reflScale;
    float  f90       = reflScale;
    float3 kD        = (1.0f - SpecularFresnelRoughness(NdotV, F0, f90, roughness)) * (1.0f - metallic);

    // Optional ray-traced sky-leak attenuation on baseline ambient for deep interiors,
    // plus deterministic multi-cascade indirect geometry bounce.
    float  baseAtten     = lerp(1.0f, lerp(0.55f, 1.0f, skyTransmittance), saturate(pc.skyOcclusionStrength));
    float3 diffuseBounce = incidentIrrOverPi * albedo * kD * combinedAo * max(pc.giIntensity, 0.0f);
    float3 finalColor    = baseLighting.rgb * baseAtten + diffuseBounce;

    if (!IsFiniteFloat3(finalColor)) {
        finalColor = baseLighting.rgb;
    }

    outLighting[px] = float4(max(finalColor, float3(0.0f)), 1.0f);
}
