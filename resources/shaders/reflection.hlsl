// resources/shaders/reflection.hlsl
#pragma pack_matrix(column_major) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
#include "pbr_helpers.hlsl"
#include "uniforms.hlsl"

struct InstanceData {
    float4x4 world;
    float4x4 prevWorld;
    uint64_t posAddress;
    uint64_t attrAddress;
    uint64_t skinAddress;
    uint64_t iboAddress;

    uint vertexCount;
    uint indexCount;
    uint texIndices0;
    uint texIndices1;

    float cullRadius;
    float metallicFactor;
    float roughnessFactor;
    float alphaCutoff;

    uint flags;
    uint jointOffset;
    uint morphOffset;
    uint activeMorphCount;

    float3 localCenter;
    uint   _paddingCenter;

    float4 morphWeights;
    float4 baseColorFactor;
    float4 emissiveFactor;
};

[[vk::binding(12, 0)]] StructuredBuffer<InstanceData> g_instances;

[[vk::constant_id(0)]] const int ENABLE_SSR = 1;
#ifndef DISABLE_RTR
[[vk::constant_id(1)]] const int ENABLE_RTR = 1;
#else
static const int ENABLE_RTR = 0;
#endif

float3 RotateVector(float3 V, float3 lightDir) {
    float lenB = length(lightDir);
    if (lenB < 0.001f) {
        return V;
    }
    float3 A     = normalize(float3(0.5f, 1.0f, 0.2f));
    float3 B     = lightDir / lenB;
    float3 q_xyz = cross(B, A);
    float  q_w   = 1.0f + dot(A, B);
    float  q_len = sqrt(dot(q_xyz, q_xyz) + q_w * q_w);
    if (q_len > 0.0001f) {
        q_xyz /= q_len;
        q_w /= q_len;
        return V + 2.0f * cross(q_xyz, cross(q_xyz, V) + q_w * V);
    }
    return V;
}

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
    int      enableSSR_dynamic;
    int      enableRTR_dynamic;
    int      _pad;
};
// SLANG WARNING: This push constant block size (~180 bytes) exceeds Vulkan's guaranteed minimum maxPushConstantsSize (128).
// DXC silently allows it, but Slang's SPIR-V legalization validates against VkPhysicalDeviceLimits::maxPushConstantsSize.
// Desktop GPUs typically support 256, but mobile/portable targets may fail. Consider splitting into ConstantBuffer for portability or verify device limit.
// Modern Slang idiom: use ParameterBlock<FrameUniforms> or ConstantBuffer for large per-frame data, keep push constants <128 bytes.
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] Texture2D<float4>   texInput;
[[vk::binding(1, 0)]] SamplerState        smp;
[[vk::binding(2, 0)]] Texture2D<float>    texDepth;
[[vk::binding(3, 0)]] Texture2D<float4>   texNormalRoughness;
[[vk::binding(4, 0)]] SamplerState        pointSampler;
[[vk::binding(5, 0)]] TextureCube<float4> texEnvMap;
#ifndef DISABLE_RTR
[[vk::binding(6, 0)]] RaytracingAccelerationStructure tlas;
#endif
[[vk::binding(7, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(8, 0)]] Texture2D                     brdfLUT;
[[vk::binding(9, 0)]] SamplerState                  clampSampler;
[[vk::binding(10, 0)]] Texture2D<float4>            texLighting;
[[vk::binding(11, 0)]] Texture3D<float4>            texVoxelIntegrated;

#ifndef DISABLE_RTR
float2 RaytraceRTR(
    float3                          worldPos,
    float3                          N,
    float3                          R,
    out float                       confidence,
    out float3                      fallbackColor,
    RaytracingAccelerationStructure tlas,
    Texture2D<float>                texDepth,
    SamplerState                    pointSampler,
    float4x4                        viewProj,
    float4                          camPos,
    float4x4                        invViewProj
) {
    confidence    = 0.0f;
    fallbackColor = float3(0.0f, 0.0f, 0.0f);

    RayDesc ray;
    ray.Origin    = worldPos + N * 0.05f;
    ray.Direction = R;
    ray.TMin      = 0.01f;
    ray.TMax      = 50.0f;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
    }

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float  rayT        = q.CommittedRayT();
        float3 hitWorldPos = ray.Origin + ray.Direction * rayT;
        float4 hitClip     = mul(viewProj, float4(hitWorldPos, 1.0f));

        if (hitClip.w < 0.1f)
            return float2(-1.0f, -1.0f);

        float2 hitNDC = hitClip.xy / hitClip.w;
        float2 hitUV  = hitNDC * 0.5f + 0.5f;

        float2 edgeFactor = smoothstep(0.0f, 0.03f, hitUV) * smoothstep(1.0f, 0.97f, hitUV);
        float  onScreen   = edgeFactor.x * edgeFactor.y;

        if (onScreen > 0.0f) {
            float  sampledRawDepth = texDepth.SampleLevel(pointSampler, hitUV, 0);
            float3 sampledWorldPos = ReconstructWorldPos(hitUV, sampledRawDepth, invViewProj);

            float distToHit     = hitClip.w;
            float distToSampled = mul(viewProj, float4(sampledWorldPos, 1.0f)).w;
            float depthDiff     = abs(distToHit - distToSampled);

            float maxDiff   = 0.50f + rayT * 0.08f;
            float depthMask = smoothstep(maxDiff, 0.0f, depthDiff);

            if (depthMask > 0.0f) {
                // Front-facing & on-screen: sample high-res G-buffer lighting
                confidence = onScreen * depthMask;
                return hitUV;
            }
        }

        // --- BACKFACING / OFF-SCREEN FALLBACK ---
        // Fetch instance base color directly from SSBO!
        uint         instanceID = q.CommittedInstanceID();
        InstanceData hitInst    = g_instances[instanceID];

        float3 hitBaseColor = hitInst.baseColorFactor.rgb;
        float3 ambientLight = EvaluateSH(float3(0.0f, 1.0f, 0.0f), frame.sh) * frame.ambientExposure;

        fallbackColor = hitBaseColor * ambientLight;
        confidence    = 1.0f; // High-confidence solid reflection

        return float2(-1.0f, -1.0f);
    }

    return float2(-1.0f, -1.0f);
}
#endif

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID) {
    VSOutput output;
    output.uv  = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

float4 PSMain(VSOutput input): SV_Target0 {
    float  depth = texDepth.SampleLevel(pointSampler, input.uv, 0).r;
    float3 litColor;
    float  viewDepth = 250.0f;

    if (depth >= 1.0f) {
        float3 worldPos = ReconstructWorldPos(input.uv, 1.0f, pc.invViewProj);
        float3 rayDir   = normalize(worldPos - pc.camPos.xyz);

        rayDir = RotateVector(rayDir, frame.lightDir.xyz);

        float  dy      = rayDir.y;
        float3 skyGrad = (dy >= 0.0f) ? lerp(frame.skyHorizon.rgb, frame.skyZenith.rgb, pow(saturate(dy), 1.2f)) :
                                        lerp(frame.skyHorizon.rgb, frame.skyGround.rgb, pow(saturate(-dy), 0.5f));

        litColor = skyGrad * frame.ambientExposure;
    } else {
        float4 litColorRaw = texLighting.SampleLevel(pointSampler, input.uv, 0);
        if (frame.fullBright != 0) {
            return float4(litColorRaw.rgb, 1.0f);
        }

        float4 normRoughRaw = texNormalRoughness.SampleLevel(smp, input.uv, 0);
        float  roughness    = normRoughRaw.z;
        float  metallic     = normRoughRaw.w;

        float4 albedoRaw = texInput.SampleLevel(smp, input.uv, 0);
        float3 N         = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);

        float3 worldPos = ReconstructWorldPos(input.uv, depth, pc.invViewProj);
        viewDepth       = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;

        float3 V = normalize(pc.camPos.xyz - worldPos);
        float3 R = reflect(-V, N);

        if (R.y < 0.05f) {
            R.y = 0.05f;
            R   = normalize(R);
        }

        float3 F0      = lerp(float3(0.04f, 0.04f, 0.04f), albedoRaw.rgb, metallic);
        float  NdotV   = saturate(dot(N, V));
        float2 envBRDF = brdfLUT.SampleLevel(clampSampler, float2(NdotV, roughness), 0.0f).rg;
        float3 F_rough = FresnelSchlickRoughness(NdotV, F0, roughness);
        float3 FssEss  = F_rough * envBRDF.x + float3(envBRDF.y, envBRDF.y, envBRDF.y);

        float3 R_corr  = R;
        float  boxFade = 0.0f;
        if (frame.probeMin.w > 0.0f) {
            float3 boxCenter      = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
            float3 boxExtent      = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
            float3 distFromCenter = abs(worldPos - boxCenter);
            float  normDist =
                max(max(abs(distFromCenter.x) / max(boxExtent.x, 0.0001f), abs(distFromCenter.y) / max(boxExtent.y, 0.0001f)),
                    abs(distFromCenter.z) / max(boxExtent.z, 0.0001f));
            boxFade = smoothstep(1.0f, 0.9f, normDist);
        }

        if (boxFade > 0.0f) {
            float3 boxR = BoxParallaxCorrection(worldPos, R, frame.probeMin.xyz, frame.probeMax.xyz, frame.probePos.xyz);
            R_corr      = lerp(R, boxR, boxFade);
        }

        float3 prefilteredColor = texEnvMap.SampleLevel(clampSampler, R_corr, roughness * 5.0f).rgb * frame.ambientExposure;
        float3 specularIBL      = prefilteredColor * FssEss;

        float ao = litColorRaw.a;
        litColor = litColorRaw.rgb;

        if (roughness <= 0.4f && (ENABLE_SSR != 0 || ENABLE_RTR != 0)) {
            float  confidence      = 0.0f;
            float  debugValue      = 0.0f;
            float3 reflectionColor = float3(0.0f, 0.0f, 0.0f);
            float2 hitUV           = float2(-1.0f, -1.0f);
            float3 biasedStartPos  = worldPos + N * 0.05f;

            if (ENABLE_SSR != 0) {
                hitUV = RaymarchSSR(worldPos, biasedStartPos, R, N, confidence, debugValue, texDepth, pointSampler, pc.viewProj, pc.camPos, pc.invViewProj);
            }

#ifndef DISABLE_RTR
            if (confidence < 0.1f && ENABLE_RTR != 0) {
                float3 rtrFallbackColor = float3(0.0f, 0.0f, 0.0f);
                hitUV = RaytraceRTR(worldPos, N, R, confidence, rtrFallbackColor, tlas, texDepth, pointSampler, pc.viewProj, pc.camPos, pc.invViewProj);

                if (confidence > 0.0f && hitUV.x < 0.0f) {
                    reflectionColor = rtrFallbackColor;
                }
            }
#endif

            if (confidence > 0.0f) {
                if (hitUV.x >= 0.0f) {
                    float4 hitNormRough = texNormalRoughness.SampleLevel(smp, hitUV, 0);
                    if (hitNormRough.z == 0.0f) {
                        confidence = 0.0f;
                    } else {
                        confidence *= saturate(dot(R, N) * 10.0f);
                        reflectionColor = texLighting.SampleLevel(smp, hitUV, 0).rgb;
                    }
                }
            }

            float3 specReflect = lerp(specularIBL, reflectionColor * FssEss, confidence);

            float roughnessFade    = saturate(1.0f - roughness);
            float horizonOcclusion = saturate(1.0f + dot(R, N));
            horizonOcclusion *= horizonOcclusion;

            litColor += specReflect * roughnessFade * horizonOcclusion;
        } else {
            float3 F_refl        = float3(0.15f, 0.15f, 0.15f);
            float3 F_term        = F_refl + (1.0f - F_refl) * pow(saturate(1.0f - dot(V, N)), 5.0f);
            float  roughnessFade = saturate(1.0f - roughness);
            litColor             = lerp(litColor, litColor + specularIBL * F_term * roughnessFade, roughnessFade);
        }
    }

    if (frame.fullBright == 0) {
        float zSlice = log(max(viewDepth, 0.1f) / 0.1f) / log(10000.0f);
        zSlice -= 0.5f / 64.0f;

        float2 noisePos = input.pos.xy;
        noisePos.x += (frame.camPos.w * 60.0f) * 5.588238f;
        noisePos.y += (frame.camPos.w * 60.0f) * 5.588238f;
        float3 magic  = float3(0.06711056f, 0.00583715f, 52.9829189f);
        float  dither = frac(magic.z * frac(dot(noisePos, magic.xy))) - 0.5f;

        zSlice += (dither * 1.5f) / 64.0f;

        float4 volumetrics = texVoxelIntegrated.SampleLevel(smp, float3(input.uv, zSlice), 0);
        litColor           = litColor * volumetrics.a + volumetrics.rgb;
    }

    return float4(litColor, 1.0f);
}
