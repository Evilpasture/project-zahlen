// resources/shaders/lighting.hlsl

import cluster_math;
import descriptor_heap_layout;
import pbr_helpers;
import uniforms;

[vk::push_constant]
ScenePassPushConstants pc;

// --- G-Buffer & Lighting Inputs ---
Texture2D<float4> texInput;
SamplerState smp;
Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
StructuredBuffer<Light> lights;
ConstantBuffer<FrameUniforms> frame;
Texture2DArray<float> shadowMap;
SamplerComparisonState shadowSampler;
Texture2D ltc_mat;
Texture2D ltc_amp;
SamplerState clampSampler;
StructuredBuffer<ClusterVolume> clusterGrid;
StructuredBuffer<uint> clusterIndexList;
Texture2D<float4> texAmbient;
SamplerState pointSampler;

// --- Punctual Shadow Maps ---
TextureCubeArray<float> punctualShadowCube; //  Omni-directional shadows
Texture2DArray<float> punctualShadow2D;     // Spotlight shadows

// The TLAS binding is declared LAST on purpose: in the NoRT variant this
// declaration disappears entirely, so the (never present) hole stays at the
// tail of the binding table and positional descriptor writes stay aligned.
#ifndef DISABLE_RTR
RaytracingAccelerationStructure tlas;
#endif

[[vk::constant_id(0)]]
const int ENABLE_RTR = 1;

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID: SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);

    // Flip-free projection; top-left of the texture maps straight to top-left of clip space (-1, -1)
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
    float depth = texDepth.SampleLevel(pointSampler, input.uv, 0);
    if (depth >= 1.0f)
        discard;

    float4 albedoRaw = texInput.SampleLevel(smp, input.uv, 0);
    if (frame.fullBright != 0) {
        return float4(albedoRaw.rgb, 1.0f);
    }
    float4 normRoughRaw = texNormalRoughness.SampleLevel(smp, input.uv, 0);
    float3 N            = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);
    float roughness     = normRoughRaw.z;
    float metallic      = normRoughRaw.w;

    float3 worldPos = ReconstructWorldPos(input.uv, depth, pc.invViewProj);

    float viewDepth = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;

    float3 V    = normalize(pc.camPos.xyz - worldPos);
    float3 F0   = lerp(float3(0.04f, 0.04f, 0.04f), albedoRaw.rgb, metallic);
    float NdotV = saturate(dot(N, V));

    float4 ambientData         = texAmbient.SampleLevel(smp, input.uv, 0);
    float3 ambientDiffuseAndGI = ambientData.rgb;
    float ao                   = ambientData.a;

    // --- 1. Direct Sun Light ---
    float3 L_sun    = normalize(frame.lightDir.xyz);
    float3 H_sun    = normalize(V + L_sun + 1e-5f);
    float NdotL_sun = saturate(dot(N, L_sun));

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

        float texelSizeWorld  = (frame.shadowWidth * (cascadeIndex + 1)) / frame.shadowResolution;
        float normalBias      = saturate(1.0 - dot(N, L_sun)) * 1.5 + 0.2;
        float3 biasedWorldPos = worldPos + N * (normalBias * texelSizeWorld);

        float4 shadowPos = mul(frame.lightSpaceMatrices[cascadeIndex], float4(biasedWorldPos, 1.0f));

        shadowPos.xy = shadowPos.xy * 0.5f + 0.5f * shadowPos.w;

        shadow = CalculateShadowPCSS(shadowPos, N, L_sun, input.pos.xy, shadowMap, shadowSampler, pointSampler, frame.shadowResolution, cascadeIndex);
    }

    float D      = DistributionGGX(N, H_sun, roughness);
    float G_term = GeometrySmith(N, V, L_sun, roughness);
    float3 F     = FresnelSchlick(saturate(dot(H_sun, V)), F0);

    float3 spec   = (D * G_term * F) / max(4.0f * NdotV * NdotL_sun, 0.001f);
    float3 kD_sun = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0f - metallic);

    float sunIntensity = frame.lightDir.w;
    float3 directSun   = (kD_sun * albedoRaw.rgb / 3.14159265f + spec) * sunIntensity * NdotL_sun * shadow;

    // --- 2. Punctual and Area Lights (Clustered) ---
    float3 directPunctual = float3(0.0f, 0.0f, 0.0f);

    uint cIdx = ClusterIndexFromUV(input.uv, viewDepth);

    ClusterVolume cluster = clusterGrid[cIdx];
    uint lightListCount   = min(cluster.count, 64u); // Safety clamp against out-of-bounds reads

    for (uint i = 0; i < lightListCount; ++i) {
        uint l = clusterIndexList[cluster.offset + i];
        if (l >= frame.lightCount)
            continue; // Out-of-bounds guard

        Light light = lights[l];
        if (light.type == 3) { // AREA LIGHT
            float2 uv      = float2(roughness, sqrt(1.0f - NdotV));
            float4 t1      = ltc_mat.SampleLevel(clampSampler, uv, 0.0f);
            float3x3 Minv  = float3x3(float3(t1.x, 0.0f, t1.y), float3(0.0f, 1.0f, 0.0f), float3(t1.z, 0.0f, t1.w));
            float4 t2      = ltc_amp.SampleLevel(clampSampler, uv, 0.0f);
            float2 schlick = float2(t2.x, t2.y);

            float3 specLTC = LTC_Evaluate(N, V, worldPos, Minv, light.points, light.twoSided == 1);
            specLTC *= F0 * schlick.x + (1.0f - F0) * schlick.y;

            float3 diffLTC = LTC_Evaluate(N, V, worldPos, float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1), light.points, light.twoSided == 1);

            // Keep ray-traced inline queries reserved for the primary sun.
            // Per-cluster area-light rays are the worst warp-divergence path.
            const float shadowVisibility = 1.0f;

            directPunctual += ((1.0f - metallic) * albedoRaw.rgb * diffLTC + specLTC) * light.color * light.intensity * shadowVisibility;
        } else { // POINT/SPOT LIGHT
            if (light.type == 0 || light.type == 4)
                continue;
            float3 L_unnorm    = light.position - worldPos;
            float distToCenter = length(L_unnorm);
            float3 L_p         = L_unnorm / (distToCenter + 1e-5f);
            float NdotL        = max(dot(N, L_p), 0.0f);

            float distRatio = distToCenter / max(light.range, 0.001f);
            float window    = saturate(1.0f - distRatio * distRatio);
            window *= window;
            float atten = window / (distToCenter * distToCenter + 1.0f);

            if (NdotL > 0.0f) {
                float3 H   = normalize(V + L_p);
                float D_p  = DistributionGGX(N, H, roughness);
                float G_p  = GeometrySmith(N, V, L_p, roughness);
                float3 F_p = FresnelSchlick(max(dot(H, V), 0.0f), F0);

                float3 spec_p = (D_p * G_p * F_p) / (4.0f * max(dot(N, V), 0.0f) * NdotL + 0.0001f);
                float3 kD_p   = (1.0f - F_p) * (1.0f - metallic);

                float shadowVisibility = 1.0f;
                if (light.shadowLayer >= 0) {
                    if (light.type == 1) { // POINT LIGHT (Omni-directional Cubemap)
                        float3 r = -L_unnorm;

                        float currentDepth = (distToCenter / max(light.range, 0.001f)) - 0.005f;

                        shadowVisibility = punctualShadowCube.SampleCmpLevelZero(shadowSampler, float4(r, light.shadowLayer), currentDepth);
                    } else if (light.type == 2) { // SPOT LIGHT (2D Perspective Array)
                        // Slang has no direct array->matrix cast; columns of a
                        // column-major float4x4 map 1:1 to the float4 array.
                        float4x4 lightViewProj;
                        lightViewProj[0] = light.points[0];
                        lightViewProj[1] = light.points[1];
                        lightViewProj[2] = light.points[2];
                        lightViewProj[3] = light.points[3];
                        float4 sPos      = mul(lightViewProj, float4(worldPos + N * 0.05f, 1.0f));
                        sPos.xy          = sPos.xy * 0.5f + 0.5f * sPos.w;

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
