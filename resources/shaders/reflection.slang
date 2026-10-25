// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import common;
import ddgi_common;
import gpu_buffer;
import instance_data;
import material_model;
import math.transform;
import pbr_helpers;
import rcgi_common;
import uniforms;

[[vk::constant_id(0)]]
const int ENABLE_SSR = 1;
#ifndef DISABLE_RTR
[[vk::constant_id(1)]]
const int ENABLE_RTR = 1;
#else
static const int ENABLE_RTR = 0;
#endif

struct PushConstants {
    float4x4 invViewProj;
    float4x4 viewProj;
    float4   camPos;
    int      giMode;
    float    aoRadius;
    float    aoBias;
    float    aoPower;
    float    giIntensity;
    int      giSamples;
    int      enableSSR;
    int      enableRTR;
    int      translucentSurface;
};
[vk::push_constant]
PushConstants pc;

Texture2D<float4>              texInput;
SamplerState                   smp;
Texture2D<float>               texDepth;
Texture2D<float4>              texNormalRoughness;
Texture2D<float>               texHiZ;
SamplerState                   pointSampler;
TextureCube<float4>            texEnvMap;      // conditioned IBL, never used as the cooked visible sky
Texture2D<float4>              texSkyEquirect; // original HDR only when environmentMode == 3
SamplerState                   skySampler;
ConstantBuffer<FrameUniforms>  frame;
Texture2D                      brdfLUT;
SamplerState                   clampSampler;
Texture2D<float4>              texLighting;
Texture2D<float>               texLightingAo;
Texture3D<float4>              texVoxelIntegrated;
StructuredBuffer<InstanceData> g_instances;

#ifndef DISABLE_RTR
RaytracingAccelerationStructure tlas;
Texture2D<float4>               texDdgiIrradiance;
Texture2D<float2>               texDdgiDistance;
Texture2D<float4>               texRcgiProbes;
Texture2D<float4>               texRcgiCascade0;
#endif

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID: SV_VertexID) {
    VSOutput output;
    output.uv  = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
    float  depth     = texDepth.SampleLevel(pointSampler, input.uv, 0).r;
    float3 litColor;
    float  viewDepth = 250.0f;

    // The translucent composite exists only where the prepass drew a
    // translucent surface. Everywhere else its job is to mirror the opaque
    // scene (what a transmitted ray sees behind the glass), never to
    // re-shade the sky fill -- the opaque pass already composited that.
    if (pc.translucentSurface != 0 && depth >= 1.0f) {
        return float4(texInput.SampleLevel(pointSampler, input.uv, 0).rgb, 1.0f);
    }

    // Pixels no layer drew fall through to the authored sky.
    if (depth >= 1.0f) {
        if (frame.environmentMode == 2) {
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        }

        float3 worldPos = ReconstructWorldPos(input.uv, 1.0f, pc.invViewProj);
        float3 rayDir   = SafeNormalize(worldPos - pc.camPos.xyz);

        if (frame.environmentMode == 3) {
            float2 uv = float2(
                atan2(rayDir.z, rayDir.x) * (0.5f / 3.141592653589793f) + 0.5f,
                acos(clamp(rayDir.y, -1.0f, 1.0f)) / 3.141592653589793f);
            litColor = texSkyEquirect.SampleLevel(skySampler, uv, 0).rgb * frame.ambientExposure;
        } else if (frame.environmentMode == 1) {
            litColor = texEnvMap.SampleLevel(clampSampler, rayDir, 0).rgb * frame.ambientExposure;
        } else {
            rayDir         = RotateVector(rayDir, frame.lightDir.xyz);
            float  dy      = rayDir.y;
            float3 skyGrad = (dy >= 0.0f) ? lerp(frame.skyHorizon.rgb, frame.skyZenith.rgb, pow(saturate(dy), 1.2f))
                                          : lerp(frame.skyHorizon.rgb, frame.skyGround.rgb, pow(saturate(-dy), 0.5f));
            litColor       = skyGrad * frame.ambientExposure;
        }
    } else {
        float4 litColorRaw = texLighting.SampleLevel(pointSampler, input.uv, 0);
        if (frame.fullBright != 0) {
            return float4(litColorRaw.rgb, 1.0f);
        }

        float4 normRoughRaw = texNormalRoughness.SampleLevel(pointSampler, input.uv, 0);
        float  roughness    = normRoughRaw.z;
        float  metallic     = normRoughRaw.w;
        float4 albedoRaw    = texInput.SampleLevel(pointSampler, input.uv, 0);
        float3 N            = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);

        float3 worldPos = ReconstructWorldPos(input.uv, depth, pc.invViewProj);
        viewDepth       = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;

        float3 V = SafeNormalize(pc.camPos.xyz - worldPos, N);
        float3 R = reflect(-V, N);

        const float environmentReflectionScale = saturate(albedoRaw.a);
        float       NdotV                      = saturate(dot(N, V));
        float3      F0                         = lerp(float3(kDielectricF0), albedoRaw.rgb, metallic) * environmentReflectionScale;
        float2      envBRDF                    = brdfLUT.SampleLevel(clampSampler, float2(NdotV, roughness), 0.0f).rg;
        float3      FssEss                     = F0 * envBRDF.x + float3(EffectiveF90(environmentReflectionScale, metallic)) * envBRDF.y;

        // Rough GGX lobe bends toward N for environment / cascade cone lookup,
        // while deterministic ray traversal (Tier 1 & Tier 2) traces exact mirror R.
        float3 R_ibl   = SafeNormalize(lerp(R, N, roughness * roughness), N);
        float3 R_corr  = R_ibl;
        float  boxFade = 0.0f;
        if (frame.probeMin.w > 0.0f) {
            float3 boxCenter      = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
            float3 boxExtent      = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
            float3 distFromCenter = abs(worldPos - boxCenter);
            float  normDist       = max(
                max(abs(distFromCenter.x) / max(boxExtent.x, 0.0001f), abs(distFromCenter.y) / max(boxExtent.y, 0.0001f)),
                abs(distFromCenter.z) / max(boxExtent.z, 0.0001f));
            boxFade = smoothstep(1.0f, 0.9f, normDist);
        }
        if (boxFade > 0.0f) {
            float3 boxR = BoxParallaxCorrection(worldPos, R_ibl, frame.probeMin.xyz, frame.probeMax.xyz, frame.probePos.xyz);
            R_corr      = lerp(R_ibl, boxR, boxFade);
        }

        float ao     = saturate(texLightingAo.Load(int3(uint2(input.pos.xy), 0)));
        float specAo = saturate(pow(max(NdotV + ao, 1e-4f), exp2(-16.0f * roughness - 1.0f)) - 1.0f + ao);

        float3 prefilteredColor = texEnvMap.SampleLevel(clampSampler, R_corr, roughness * 5.0f).rgb * frame.ambientExposure;

        // Tier 3 (roughness > 0.40, and background fallback behind Tier 2's smooth
        // handoff): evaluate wide specular lobes directly from Radiance Cascades C0
        // directional cones + Split-Sum IBL (`brdfLUT`) with 0 extra rays!
        float3 roughSpecIrr = prefilteredColor;
#ifndef DISABLE_RTR
        if (roughness >= 0.30f && pc.translucentSurface == 0 && pc.giIntensity > 0.0f) {
            float3 rcSpec = SampleRcgiRoughSpecular(
                input.uv, frame.screenResolution, worldPos, N, R_ibl, roughness, viewDepth, prefilteredColor, pc.giIntensity,
                texRcgiProbes, texRcgiCascade0);
            float rcBlend = smoothstep(0.30f, kReflMaxTrace, roughness);
            roughSpecIrr  = lerp(prefilteredColor, rcSpec, rcBlend);
        }
#endif
        float3 baseSpecularIBL = roughSpecIrr * FssEss * specAo;

        // Multiscattering metallic energy compensation via cosine-weighted SH.
        float  Ems              = saturate(1.0f - envBRDF.x - envBRDF.y);
        float3 Favg             = F0 + (1.0f - F0) * (1.0f / 21.0f);
        float3 Fms              = FssEss * Favg / max(1.0f - Ems * Favg, float3(1e-4f));
        float3 multiSpecularIBL = EvaluateSH(N, frame.sh) * frame.ambientExposure * Fms * Ems * specAo * metallic;

        litColor = litColorRaw.rgb;

        const bool wantRTR = (ENABLE_RTR != 0) && (pc.enableRTR != 0);
        const bool wantSSR = (ENABLE_SSR != 0) && (pc.enableSSR != 0);

        // Tier 1 (roughness < 0.20) & Tier 2 (0.20 <= roughness <= 0.40):
        //   1 deterministic ray along R = reflect(-V, N)
        //   Step A: Hierarchical min-depth Hi-Z SSR -> cone-footprint mip of texLighting
        //   Step B: On Hi-Z miss / screen-edge fade, hardware TLAS inline fallback along R
        if (roughness <= kReflMaxTrace && NdotV > 1e-4f && (wantSSR || wantRTR)) {
            uint lightW = 0u, lightH = 0u, lightingMips = 1u;
            texLighting.GetDimensions(0u, lightW, lightH, lightingMips);
            float projScaleY = 1.0f / max(abs(frame.invProj[1][1]), 0.1f);

            float  ssrConfidence = 0.0f;
            float3 ssrColor      = float3(0.0f);

            if (wantSSR) {
                float3       biasedStart = worldPos + N * max(0.025f, viewDepth * 0.002f);
                const bool   useHiZ      = (pc.translucentSurface == 0);
                SsrHitResult ssrHit      = RaymarchHiZDeterministic(
                    worldPos, biasedStart, R, N, roughness, useHiZ, texHiZ, texDepth, texNormalRoughness, pointSampler,
                    pc.viewProj, pc.invViewProj, frame.invProj);

                if (ssrHit.confidence > 0.0f) {
                    float coneLod = ComputeReflectionConeLod(
                        roughness, ssrHit.hitDist, ssrHit.hitViewW, frame.screenResolution, projScaleY, lightingMips);
                    ssrColor      = texLighting.SampleLevel(smp, ssrHit.uv, coneLod).rgb;
                    ssrConfidence = ssrHit.confidence;
                }
            }

            float  finalConfidence = ssrConfidence;
            float3 reflectionColor = ssrColor;

#ifndef DISABLE_RTR
            // Fire the TLAS ray only when SSR is disabled or did not resolve a
            // full-confidence on-screen hit (off-screen, behind camera, or grazing border).
            if (wantRTR && ssrConfidence < 0.98f) {
                RtrWorldHitResult tlasHit = TraceDeterministicRtrFallback(
                    worldPos, N, R, roughness, prefilteredColor, projScaleY, lightingMips, tlas, frame, g_instances,
                    texDepth, texNormalRoughness, texLighting, texDdgiIrradiance, texDdgiDistance, smp, pointSampler, clampSampler);

                if (tlasHit.confidence > 0.0f) {
                    reflectionColor = lerp(tlasHit.radiance, ssrColor, ssrConfidence);
                    finalConfidence = max(ssrConfidence, tlasHit.confidence);
                }
            }
#endif

            // Smooth handoff across [0.30, 0.405] into Tier 3 (RC cones + Split-Sum IBL)
            // so roughness maps transition without a visible contour step at 0.40.
            float tierFade = smoothstep(0.405f, 0.30f, roughness);
            finalConfidence *= tierFade;

            float3 specReflect      = lerp(baseSpecularIBL, reflectionColor * FssEss * specAo, finalConfidence);
            float  horizonOcclusion = saturate(1.0f + dot(R, N));
            horizonOcclusion *= horizonOcclusion;

            litColor += specReflect * horizonOcclusion + multiSpecularIBL;
        } else {
            litColor += baseSpecularIBL + multiSpecularIBL;
        }
    }

    if (frame.fullBright == 0) {
        float zSlice = log(max(viewDepth, frame.nearZ) / frame.nearZ) / log(frame.farZ / frame.nearZ);
        zSlice -= 0.5f / 64.0f;

        float  ignFrame = float(WrapFrameIndex(frame.frameIndex));
        float2 noisePos = input.pos.xy;
        noisePos.x += ignFrame * 5.588238f;
        noisePos.y += ignFrame * 5.588238f;
        float3 magic  = float3(0.06711056f, 0.00583715f, 52.9829189f);
        float  dither = frac(magic.z * frac(dot(noisePos, magic.xy))) - 0.5f;

        zSlice += (dither * 1.5f) / 64.0f;

        float4 volumetrics = texVoxelIntegrated.SampleLevel(smp, float3(input.uv, zSlice), 0);
        litColor           = litColor * volumetrics.a + volumetrics.rgb;
    }

    return float4(litColor, 1.0f);
}
