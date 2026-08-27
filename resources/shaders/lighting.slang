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
SamplerState pointSampler;

// --- Punctual Shadow Maps ---
TextureCubeArray<float> punctualShadowCube; //  Omni-directional shadows
Texture2DArray<float> punctualShadow2D;     // Spotlight shadows

// Blue noise tile for the RT shadow dither. Declared before the TLAS so the
// NoRT variant still gets both bindings -- only the trailing hole moves.
Texture2D<float4> blueNoiseTex;
SamplerState blueNoiseSampler;

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

// ---------------------------------------------------------------------------
// Ambient / GI term, inlined from the former standalone ambient pass.
//
// The old deferred chain spent one full-screen pass + one full-screen HDR
// round-trip per frame purely to evaluate AO/GI into an intermediate target
// that this shader immediately re-sampled. Computing it on the fly removes
// the pass boundary, the extra render target and its bandwidth.
// Returns float4(GI + SH sky ambient, AO).
// ---------------------------------------------------------------------------
float4 ComputeAmbientAndAO(float2 uv, uint2 pixelPos, float3 N, float3 worldPos, float viewDepth) {
    float  ao            = 1.0f;
    float3 indirectLight = float3(0.0f, 0.0f, 0.0f);

    float2 edgeFactor = smoothstep(0.0f, 0.08f, uv) * smoothstep(1.0f, 0.92f, uv);
    float  screenFade = edgeFactor.x * edgeFactor.y;

    // --- DEFINE SMOOTH FADE BOUNDARY FROM 40m TO 50m ---
    float fade = saturate((50.0f - viewDepth) / 10.0f);

    if (pc.giMode > 0 && fade > 0.0f) {
        float occlusion = 0.0f;
        uint dw, dh;
        texDepth.GetDimensions(dw, dh);

        if (pc.giMode == 3 || pc.giMode == 4) {
            float angle        = GetStableWeylNoise(pixelPos, pc.camPos.w) * 2.0f * 3.14159265f;
            int steps          = max(pc.giSamples / 6, 1);
            float jitterOffset = GetRotationAngle(uv, pc.camPos.w);
            float aspect       = float(dw) / max(float(dh), 1.0f);
            float focalLength  = abs(pc.viewProj[1][1]);

            // FIX: UV radius projection depends strictly on view-space depth (z-depth)
            float uvRadius = min((pc.aoRadius * focalLength) / max(viewDepth, 0.1f), 0.2f);

            for (int d = 0; d < 4; ++d) {
                float sliceAngle  = (float(d) / 4.0f) * 3.14159265f + angle;
                float2 rotatedDir = float2(cos(sliceAngle), sin(sliceAngle));
                float2 uvStep     = rotatedDir * uvRadius;
                uvStep.x /= aspect;

                float max_sin_right = 0.0f;
                float max_sin_left  = 0.0f;

                for (int i = 1; i <= steps; ++i) {
                    float linearT   = (float(i) - 0.5f + (jitterOffset - 0.5f)) / float(steps);
                    float t         = linearT * linearT;
                    float2 uv_right = uv + uvStep * t;
                    float2 uv_left  = uv - uvStep * t;

                    if (all(uv_right >= 0.0f) && all(uv_right <= 1.0f)) {
                        float d_right = texDepth.SampleLevel(pointSampler, uv_right, 0).r;
                        if (d_right < 1.0f) {
                            float3 pos_right = ReconstructWorldPos(uv_right, d_right, pc.invViewProj);
                            float3 H         = pos_right - worldPos;
                            float len        = length(H);
                            if (len < pc.aoRadius)
                                max_sin_right = max(max_sin_right, dot(H, N) / max(len, 0.001f));
                        }
                    }
                    if (all(uv_left >= 0.0f) && all(uv_left <= 1.0f)) {
                        float d_left = texDepth.SampleLevel(pointSampler, uv_left, 0).r;
                        if (d_left < 1.0f) {
                            float3 pos_left = ReconstructWorldPos(uv_left, d_left, pc.invViewProj);
                            float3 H        = pos_left - worldPos;
                            float len       = length(H);
                            if (len < pc.aoRadius)
                                max_sin_left = max(max_sin_left, dot(H, N) / max(len, 0.001f));
                        }
                    }
                }
                occlusion += saturate((max_sin_right + max_sin_left) * 0.5f - pc.aoBias);
            }

            float rawAo = 1.0f - SoftClamp(saturate((occlusion / 4.0f) * pc.aoPower * screenFade), 0.85f);
            ao          = lerp(1.0f, rawAo, fade);

        } else {
            float angle    = GetRotationAngle(uv, pc.camPos.w);
            float cosTheta = cos(angle * 2.0f * 3.14159265f);
            float sinTheta = sin(angle * 2.0f * 3.14159265f);

            int effectiveSamples = min(pc.giSamples, 16);
            for (int s = 0; s < effectiveSamples; ++s) {
                float3 sampleOffset = HemisphereSamples[s % 8];
                float3 rotatedSample =
                    float3(sampleOffset.x * cosTheta - sampleOffset.y * sinTheta, sampleOffset.x * sinTheta + sampleOffset.y * cosTheta, sampleOffset.z);
                float3 up        = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
                float3 tangent   = normalize(cross(up, N));
                float3 bitangent = cross(N, tangent);
                float3 dir       = rotatedSample.x * tangent + rotatedSample.y * bitangent + rotatedSample.z * N;
                float3 testPos   = worldPos + dir * pc.aoRadius;

                float4 clipPos = mul(pc.viewProj, float4(testPos, 1.0f));
                // Native Vulkan positive coordinate mapping mapping [-1, 1] to [0, 1]
                float2 uv_sample = (clipPos.xy / clipPos.w) * 0.5f + 0.5f;

                if (any(uv_sample < 0.0f) || any(uv_sample > 1.0f))
                    continue;

                float sampleRawDepth = texDepth.SampleLevel(pointSampler, uv_sample, 0).r;
                if (sampleRawDepth >= 1.0f)
                    continue;

                float3 sampleWorldPos = ReconstructWorldPos(uv_sample, sampleRawDepth, pc.invViewProj);
                float diff            = length(testPos - pc.camPos.xyz) - length(sampleWorldPos - pc.camPos.xyz);

                if (diff > 0.0f) {
                    float finalWeight = smoothstep(0.0f, pc.aoBias * 2.0f, diff - pc.aoBias) * smoothstep(0.0f, 1.0f, pc.aoRadius / abs(diff));
                    if (pc.giMode == 1)
                        occlusion += finalWeight;
                    else if (pc.giMode == 2)
                        indirectLight += texInput.SampleLevel(smp, uv_sample, 0).rgb * max(dot(N, dir), 0.0f) * finalWeight;
                }
            }

            if (pc.giMode == 1) {
                float rawAo = 1.0f - SoftClamp(saturate((occlusion / float(effectiveSamples)) * pc.aoPower * screenFade), 0.85f);
                ao          = lerp(1.0f, rawAo, fade);
            } else if (pc.giMode == 2) {
                float3 rawGI  = (indirectLight / float(effectiveSamples)) * pc.giIntensity * screenFade;
                indirectLight = lerp(float3(0.0f, 0.0f, 0.0f), rawGI, fade);
            }
        }
    }

    // Add baseline spherical harmonics ambient sky light so unlit faces receive soft fill light
    float3 baselineAmbient = EvaluateSH(N, frame.sh) * frame.ambientExposure;
    indirectLight += baselineAmbient;

    return float4(indirectLight, ao);
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

    float4 ambientData         = ComputeAmbientAndAO(input.uv, uint2(input.pos.xy), N, worldPos, viewDepth);
    float3 ambientDiffuseAndGI = ambientData.rgb;
    float ao                   = ambientData.a;

    // --- 1. Direct Sun Light ---
    float3 L_sun    = normalize(frame.lightDir.xyz);
    float3 H_sun    = normalize(V + L_sun + 1e-5f);
    float NdotL_sun = saturate(dot(N, L_sun));

    float shadow = 1.0f;

    // Cascade shadow maps: analytic PCSS, deterministic per pixel. Computed
    // whenever they will contribute (always, except for RT-shadow pixels
    // nearer than the fade start, which never sample them).
    float cascadeShadow = 1.0f;
    bool  needCascade   = true;
#ifndef DISABLE_RTR
    needCascade = !(ENABLE_RTR != 0 && pc.enableRTR != 0 && viewDepth < 80.0f);
#endif
    if (needCascade) {
        uint cascadeIndex = 0;
        if (viewDepth > frame.cascadeSplits.x)
            cascadeIndex = 1;
        if (viewDepth > frame.cascadeSplits.y)
            cascadeIndex = 2;
        if (viewDepth > frame.cascadeSplits.z)
            cascadeIndex = 3;

        // True per-cascade ortho texel size, derived from the cascade matrix
        // itself: for an ortho projection |M * dir| for a unit world direction
        // is 1/radius (orthonormal light rotation scaled by 1/radius), so one
        // shadow texel spans 2*radius/resolution world units. The old
        // shadowWidth*(cascadeIndex+1)/resolution guess was 3-6x too small on
        // the outer cascades, starving the normal offset and letting far
        // surfaces acne-shimmer as they slide across the per-pixel dither.
        // Equally, |M.z2| gives NDC depth per world metre for the cascade's
        // own [NearClip, farClip] range, replacing PCSS's hardcoded 400.
        float4 lightBasisX     = mul(frame.lightSpaceMatrices[cascadeIndex], float4(1.0f, 0.0f, 0.0f, 0.0f));
        float4 lightBasisZ     = mul(frame.lightSpaceMatrices[cascadeIndex], float4(0.0f, 0.0f, 1.0f, 0.0f));
        float  ndcPerWorldX    = max(length(lightBasisX.xyz), 1e-6f);
        float  texelSizeWorld  = 2.0f / (ndcPerWorldX * float(frame.shadowResolution));
        float  worldDepthPerNdc = 1.0f / max(abs(lightBasisZ.z), 1e-8f);
        float normalBias      = saturate(1.0 - dot(N, L_sun)) * 1.5 + 0.2;
        float3 biasedWorldPos = worldPos + N * (normalBias * texelSizeWorld);

        float4 shadowPos = mul(frame.lightSpaceMatrices[cascadeIndex], float4(biasedWorldPos, 1.0f));

        shadowPos.xy = shadowPos.xy * 0.5f + 0.5f * shadowPos.w;

        cascadeShadow = CalculateShadowPCSS(shadowPos, N, L_sun, input.pos.xy, shadowMap, shadowSampler, pointSampler, frame.shadowResolution, cascadeIndex, worldDepthPerNdc);
    }
    shadow = cascadeShadow;

#ifndef DISABLE_RTR
    // RT shadow -> cascade blend. The RT shadow is a 1-bit estimate (one ray,
    // binary hit/miss) with a fixed screen-space dither, and its penumbra
    // width grows linearly with distance (spread = sunSize radians): a few
    // pixels near the camera, but ~sunSize*distance wide at 200-300 m. Every
    // pixel in that wide band is a coin flip that flips again as geometry
    // slides across the per-pixel dither - the far shimmer reported in the
    // preset scene. The cascade maps are deterministic and PCSS gives
    // analytic soft shadows, so hand the sun shadow to them with distance:
    // RT-only below 80 m, smoothstep blend across 80-160 m, cascade-only
    // beyond (the RT ray is skipped there - it was pure noise). Dynamic
    // casters are unaffected: the cascade pass re-renders them every frame.
    if (ENABLE_RTR != 0 && pc.enableRTR != 0 && viewDepth < 160.0f) {
        if (viewDepth < 80.0f) {
            shadow = CalculateShadowRayTraced(worldPos, N, L_sun, input.pos.xy, pc.camPos.w, tlas, blueNoiseTex, blueNoiseSampler, frame.sunSize);
        } else {
            float rtrShadow = CalculateShadowRayTraced(worldPos, N, L_sun, input.pos.xy, pc.camPos.w, tlas, blueNoiseTex, blueNoiseSampler, frame.sunSize);
            float t         = saturate((viewDepth - 80.0f) / 80.0f);
            t               = t * t * (3.0f - 2.0f * t);
            shadow          = lerp(rtrShadow, cascadeShadow, t);
        }
    }
#endif

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
