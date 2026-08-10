// resources/shaders/lighting.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from lighting.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/lighting.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
import pbr_helpers; // modern Slang import (was #include "pbr_helpers.hlsl")
import uniforms; // modern Slang import (was #include "uniforms.hlsl")
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
    int      _pad;
};
// SLANG WARNING: This push constant block size (~180 bytes) exceeds Vulkan's guaranteed minimum maxPushConstantsSize (128).
// DXC silently allows it, but Slang's SPIR-V legalization validates against VkPhysicalDeviceLimits::maxPushConstantsSize.
// Desktop GPUs typically support 256, but mobile/portable targets may fail. Consider splitting into ConstantBuffer for portability or verify device limit.
// Modern Slang idiom: use ParameterBlock<FrameUniforms> or ConstantBuffer for large per-frame data, keep push constants <128 bytes.
struct ClusterVolume {
    uint offset;
    uint count;
};

// --- G-Buffer & Lighting Inputs ---
Texture2D<float4>                texInput;
SamplerState                     smp;
Texture2D<float>                 texDepth;
Texture2D<float4>                texNormalRoughness;
StructuredBuffer<Light>          lights;
ConstantBuffer<FrameUniforms>    frame;
Texture2DArray<float>            shadowMap;
SamplerComparisonState           shadowSampler;
Texture2D                        ltc_mat;
Texture2D                        ltc_amp;
SamplerState                    clampSampler;
StructuredBuffer<ClusterVolume> clusterGrid;
StructuredBuffer<uint>          clusterIndexList;
Texture2D<float4>               texAmbient;
SamplerState                    pointSampler;
#ifndef DISABLE_RTR
RaytracingAccelerationStructure tlas;
#endif

// --- Punctual Shadow Maps ---
TextureCubeArray<float> punctualShadowCube; //  Omni-directional shadows
Texture2DArray<float>   punctualShadow2D;   // Spotlight shadows

[[SpecializationConstant]] const int ENABLE_RTR = 1;

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[[push_constant]] PushConstants pc;

[shader("vertex")]

VSOutput VSMain(uint vertexID : SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);

    // Flip-free projection; top-left of the texture maps straight to top-left of clip space (-1, -1)
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

[shader("fragment")]

float4 PSMain(VSOutput input): SV_Target0 {
    float depth = texDepth.SampleLevel(pointSampler, input.uv, 0);
    if (depth >= 1.0f)
        discard;

    float4 albedoRaw = texInput.SampleLevel(smp, input.uv, 0);
    if (frame.fullBright != 0) {
        return float4(albedoRaw.rgb, 1.0f);
    }
    float4 normRoughRaw = texNormalRoughness.SampleLevel(smp, input.uv, 0);
    float3 N            = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);
    float  roughness    = normRoughRaw.z;
    float  metallic     = normRoughRaw.w;

    float3 worldPos = ReconstructWorldPos(input.uv, depth, pc.invViewProj);

    float viewDepth = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;

    float3 V     = normalize(pc.camPos.xyz - worldPos);
    float3 F0    = lerp(float3(0.04f, 0.04f, 0.04f), albedoRaw.rgb, metallic);
    float  NdotV = saturate(dot(N, V));

    float4 ambientData         = texAmbient.SampleLevel(smp, input.uv, 0);
    float3 ambientDiffuseAndGI = ambientData.rgb;
    float  ao                  = ambientData.a;

    // --- 1. Direct Sun Light ---
    float3 L_sun     = normalize(frame.lightDir.xyz);
    float3 H_sun     = normalize(V + L_sun + 1e-5f);
    float  NdotL_sun = saturate(dot(N, L_sun));

    float shadow = 1.0f;

#ifndef DISABLE_RTR
    if (ENABLE_RTR != 0 && pc.enableRTR != 0) {
        shadow = CalculateShadowRayTraced(worldPos, N, L_sun, input.pos.xy, pc.camPos.w, tlas, frame.sunSize);
    } else
#endif
    {
        uint cascadeIndex = 0;
        if (viewDepth > frame.cascadeSplits.x)
            cascadeIndex = 1;
        if (viewDepth > frame.cascadeSplits.y)
            cascadeIndex = 2;
        if (viewDepth > frame.cascadeSplits.z)
            cascadeIndex = 3;

        float  texelSizeWorld = (frame.shadowWidth * (cascadeIndex + 1)) / frame.shadowResolution;
        float  normalBias     = saturate(1.0 - dot(N, L_sun)) * 1.5 + 0.2;
        float3 biasedWorldPos = worldPos + N * (normalBias * texelSizeWorld);

        float4 shadowPos = mul(frame.lightSpaceMatrices[cascadeIndex], float4(biasedWorldPos, 1.0f));

        shadowPos.xy = shadowPos.xy * 0.5f + 0.5f * shadowPos.w;

        shadow = CalculateShadowPCSS(shadowPos, N, L_sun, input.pos.xy, shadowMap, shadowSampler, pointSampler, frame.shadowResolution, cascadeIndex);
    }

    float  D      = DistributionGGX(N, H_sun, roughness);
    float  G_term = GeometrySmith(N, V, L_sun, roughness);
    float3 F      = FresnelSchlick(saturate(dot(H_sun, V)), F0);

    float3 spec   = (D * G_term * F) / max(4.0f * NdotV * NdotL_sun, 0.001f);
    float3 kD_sun = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0f - metallic);

    float  sunIntensity = frame.lightDir.w;
    float3 directSun    = (kD_sun * albedoRaw.rgb / 3.14159265f + spec) * sunIntensity * NdotL_sun * shadow;

    // --- 2. Punctual and Area Lights (Clustered) ---
    float3 directPunctual = float3(0.0f, 0.0f, 0.0f);

    uint sliceZ = uint(max(0.0f, log(viewDepth) * frame.zScale + frame.zBias));
    uint cIdx   = min(uint(input.uv.x * 16.0f), 15u) + (min(uint(input.uv.y * 9.0f), 8u) * 16) + (min(sliceZ, 23u) * 144);

    ClusterVolume cluster        = clusterGrid[cIdx];
    uint          lightListCount = min(cluster.count, 64u); // Safety clamp against out-of-bounds reads

    for (uint i = 0; i < lightListCount; ++i) {
        uint l = clusterIndexList[cluster.offset + i];
        if (l >= frame.lightCount)
            continue; // Out-of-bounds guard

        Light light = lights[l];
        if (light.type == 3) { // AREA LIGHT
            float2   uv      = float2(roughness, sqrt(1.0f - NdotV));
            float4   t1      = ltc_mat.SampleLevel(clampSampler, uv, 0.0f);
            float3x3 Minv    = float3x3(float3(t1.x, 0.0f, t1.y), float3(0.0f, 1.0f, 0.0f), float3(t1.z, 0.0f, t1.w));
            float4   t2      = ltc_amp.SampleLevel(clampSampler, uv, 0.0f);
            float2   schlick = float2(t2.x, t2.y);

            float3 specLTC = LTC_Evaluate(N, V, worldPos, Minv, light.points, light.twoSided == 1);
            specLTC *= F0 * schlick.x + (1.0f - F0) * schlick.y;

            float3 diffLTC = LTC_Evaluate(N, V, worldPos, float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1), light.points, light.twoSided == 1);

            float shadowVisibility = 1.0f;
#ifndef DISABLE_RTR
            if (ENABLE_RTR != 0 && pc.enableRTR != 0) {
                float3 L_center     = ((light.points[0].xyz + light.points[1].xyz + light.points[2].xyz + light.points[3].xyz) * 0.25f) - worldPos;
                float  distToCenter = length(L_center);
                float3 L_p          = L_center / (distToCenter + 1e-5f);

                RayDesc ray;
                ray.Origin    = worldPos + N * 0.01f;
                ray.Direction = L_p;
                ray.TMin      = 0.01f;
                ray.TMax      = distToCenter;

                RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
                q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
                while (q.Proceed()) {
                }

                if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                    shadowVisibility = 0.0f;
                }
            }
#endif

            directPunctual += ((1.0f - metallic) * albedoRaw.rgb * diffLTC + specLTC) * light.color * light.intensity * shadowVisibility;
        } else { // POINT/SPOT LIGHT
            if (light.type == 0 || light.type == 4)
                continue;
            float3 L_unnorm     = light.position - worldPos;
            float  distToCenter = length(L_unnorm);
            float3 L_p          = L_unnorm / (distToCenter + 1e-5f);
            float  NdotL        = max(dot(N, L_p), 0.0f);

            float distRatio = distToCenter / max(light.range, 0.001f);
            float window    = saturate(1.0f - distRatio * distRatio);
            window *= window;
            float atten = window / (distToCenter * distToCenter + 1.0f);

            if (NdotL > 0.0f) {
                float3 H   = normalize(V + L_p);
                float  D_p = DistributionGGX(N, H, roughness);
                float  G_p = GeometrySmith(N, V, L_p, roughness);
                float3 F_p = FresnelSchlick(max(dot(H, V), 0.0f), F0);

                float3 spec_p = (D_p * G_p * F_p) / (4.0f * max(dot(N, V), 0.0f) * NdotL + 0.0001f);
                float3 kD_p   = (1.0f - F_p) * (1.0f - metallic);

                float shadowVisibility = 1.0f;
#ifndef DISABLE_RTR
                if (ENABLE_RTR != 0 && pc.enableRTR != 0) {
                    RayDesc ray;
                    ray.Origin    = worldPos + N * 0.01f;
                    ray.Direction = L_p;
                    ray.TMin      = 0.01f;
                    ray.TMax      = distToCenter;

                    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
                    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
                    while (q.Proceed()) {
                    }

                    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
                        shadowVisibility = 0.0f;
                    }
                } else
#endif
                    if (light.shadowLayer >= 0) {
                    if (light.type == 1) { // POINT LIGHT (Omni-directional Cubemap)
                        float3 r = -L_unnorm;

                        float currentDepth = (distToCenter / max(light.range, 0.001f)) - 0.005f;

                        shadowVisibility = punctualShadowCube.SampleCmpLevelZero(shadowSampler, float4(r, light.shadowLayer), currentDepth);
                    } else if (light.type == 2) { // SPOT LIGHT (2D Perspective Array)
                        float4x4 lightMat = float4x4(light.points[0], light.points[1], light.points[2], light.points[3]);
        float4 sPos = mul(lightMat, float4(worldPos + N * 0.05f, 1.0f));
                        sPos.xy     = sPos.xy * 0.5f + 0.5f * sPos.w;

                        float depthRef   = (sPos.z / sPos.w) - 0.005f;
                        shadowVisibility = punctualShadow2D.SampleCmpLevelZero(shadowSampler, float3(sPos.xy / sPos.w, (float) light.shadowLayer), depthRef);
                    }
                }

                directPunctual += (kD_p * albedoRaw.rgb / 3.14159265f + spec_p) * light.color * light.intensity * atten * NdotL * shadowVisibility;
            }
        }
    }

    float3 litColor = ambientDiffuseAndGI * albedoRaw.rgb + directSun + directPunctual;
    return float4(litColor, ao);
}

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All resources use actual Slang syntax (auto binding, uniform push constants, SpecializationConstant) without vk:: extensions.
