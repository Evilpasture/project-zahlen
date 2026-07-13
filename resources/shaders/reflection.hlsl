// resources/shaders/reflection.hlsl
#pragma pack_matrix(column_major)
#include "pbr_helpers.hlsl"
#include "uniforms.hlsl"
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
[[vk::binding(10, 0)]] Texture2D<float4>            texLighting;        // Lit color input from Pass 2
[[vk::binding(11, 0)]] Texture3D<float4>            texVoxelIntegrated; // Volumetrics input

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);

    // Flip-free projection; top-left of the texture maps straight to top-left of clip space (-1, -1)
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

float4 PSMain(VSOutput input): SV_Target0 {
    float  depth = texDepth.SampleLevel(pointSampler, input.uv, 0).r;
    float3 litColor;
    float  viewDepth = 250.0f; // Limit fog integration depth for the skybox

    if (depth >= 1.0f) {
        // Reconstruct the world-space point on the far plane (depth = 1.0)
        float3 worldPos = ReconstructWorldPos(input.uv, 1.0f, pc.invViewProj);
        // Calculate the ray direction pointing away from the camera
        float3 rayDir = normalize(worldPos - pc.camPos.xyz);

        // Rotate the visual skybox to match the dynamic sun
        rayDir = RotateVector(rayDir, frame.lightDir.xyz);

        // FIXED: Swapped 'smp' for 'clampSampler' to enable seamless edge blending
        litColor = texEnvMap.SampleLevel(clampSampler, rayDir, 0.0f).rgb * 25.0f;
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

        // Horizon-clamp: prevent perturbed normals from reflecting rays downward into the floor
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

        // Rotate the specular lookup vector to match the dynamic sun
        R_corr = RotateVector(R_corr, frame.lightDir.xyz);

        // FIXED: Swapped 'smp' for 'clampSampler' to prevent lines appearing in reflections
        float3 prefilteredColor = texEnvMap.SampleLevel(clampSampler, R_corr, roughness * 5.0f).rgb * frame.ambientExposure;
        float3 specularIBL      = prefilteredColor * FssEss;

        // Extract AO from litColorRaw's alpha channel (written by lighting.hlsl)
        float ao = litColorRaw.a;
        litColor = litColorRaw.rgb;

        // Rotate the diffuse SH lookup normal to match the dynamic sun
        float3 N_rot      = RotateVector(N, frame.lightDir.xyz);
        float3 irradiance = EvaluateSH(N_rot, frame.sh) * frame.ambientExposure;

        // --- ACCURATE GLOBAL IRRADIANCE OCCLUSION ---
        // Fade out the bright blue outdoor sky dome inside your indoor probe box
        if (frame.probeMin.w > 0.0f && boxFade > 0.0f) {
            float3 indoorAmbient = float3(0.12f, 0.12f, 0.12f) * frame.ambientExposure;
            irradiance           = lerp(irradiance, indoorAmbient, boxFade);
        }

        // --- ENERGY COMPENSATION TERM GENERATION ---
        float3 Favg       = F0 + (1.0f - F0) / 21.0f;
        float3 FmsEms     = (Favg * (1.0f - (envBRDF.x + envBRDF.y))) / (1.0f - Favg * (1.0f - (envBRDF.x + envBRDF.y)));
        float3 diffuseIBL = (1.0f - FssEss - FmsEms) * (1.0f - metallic) * albedoRaw.rgb * irradiance;

        // Add Diffuse IBL and Multi-Bounce energy compensation to the base lighting
        litColor += (diffuseIBL * ao) + ((FmsEms * irradiance) * ao);

        if (roughness <= 0.4f && (ENABLE_SSR != 0 || ENABLE_RTR != 0)) {
            float  confidence      = 0.0f;
            float  debugValue      = 0.0f;
            float3 reflectionColor = float3(0.0f, 0.0f, 0.0f);
            float2 hitUV           = float2(0.0f, 0.0f);
            float3 biasedStartPos  = worldPos + N * 0.05f;

            if (ENABLE_SSR != 0) {
                hitUV = RaymarchSSR(worldPos, biasedStartPos, R, N, confidence, debugValue, texDepth, pointSampler, pc.viewProj, pc.camPos, pc.invViewProj);
            }

#ifndef DISABLE_RTR
            if (confidence < 0.1f && ENABLE_RTR != 0) {
                hitUV = RaytraceRTR(worldPos, N, R, confidence, tlas, texDepth, pointSampler, pc.viewProj, pc.camPos, pc.invViewProj);
            }
#endif

            if (confidence > 0.0f) {
                float4 hitNormRough = texNormalRoughness.SampleLevel(smp, hitUV, 0);
                if (hitNormRough.z == 0.0f) {
                    confidence = 0.0f;
                } else {
                    confidence *= saturate(dot(R, N) * 10.0f);
                    reflectionColor = texLighting.SampleLevel(smp, hitUV, 0).rgb;
                }
            }

            float3 localReflection = lerp(specularIBL, reflectionColor * FssEss, confidence);

            float3 F_refl           = lerp(float3(0.15f, 0.15f, 0.15f), litColor, metallic);
            float3 F_term           = F_refl + (1.0f - F_refl) * pow(saturate(1.0f - dot(V, N)), 5.0f);
            float  roughnessFade    = saturate(1.0f - roughness);
            float  horizonOcclusion = saturate(1.0f + dot(R, N));
            horizonOcclusion *= horizonOcclusion;

            litColor = lerp(litColor, litColor + localReflection * F_term * roughnessFade * horizonOcclusion, roughnessFade);
        } else {
            float3 F_refl        = float3(0.15f, 0.15f, 0.15f);
            float3 F_term        = F_refl + (1.0f - F_refl) * pow(saturate(1.0f - dot(V, N)), 5.0f);
            float  roughnessFade = saturate(1.0f - roughness);
            litColor             = lerp(litColor, litColor + specularIBL * F_term * roughnessFade, roughnessFade);
        }
    }

    // Apply Volumetrics to BOTH geometry and the skybox
    if (frame.fullBright == 0) {
        float zSlice = log(max(viewDepth, 0.1f) / 0.1f) / log(10000.0f);

        // 1. Correct Voxel Center Alignment Offset
        zSlice -= 0.5f / 64.0f;

        // 2. Interleaved Gradient Noise (IGN) for perfect TAA dither reconstruction
        float2 noisePos = input.pos.xy;
        noisePos.x += (frame.camPos.w * 60.0f) * 5.588238f;
        noisePos.y += (frame.camPos.w * 60.0f) * 5.588238f;
        float3 magic  = float3(0.06711056f, 0.00583715f, 52.9829189f);
        float  dither = frac(magic.z * frac(dot(noisePos, magic.xy))) - 0.5f;

        // Shift the read coordinate by up to 1 voxel depth to shatter the trilinear interpolation bands
        zSlice += (dither * 1.5f) / 64.0f;

        float4 volumetrics = texVoxelIntegrated.SampleLevel(smp, float3(input.uv, zSlice), 0);
        litColor           = litColor * volumetrics.a + volumetrics.rgb;
    }

    return float4(litColor, 1.0f);
}
