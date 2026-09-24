// resources/shaders/lighting.hlsl

import cluster_grid;
import cluster_math;
import descriptor_heap_layout;
import pbr_helpers;
import uniforms;

[vk::push_constant]
ScenePassPushConstants pc;

// --- G-Buffer & Lighting Inputs
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

// --- Punctual Shadow Maps
TextureCubeArray<float> punctualShadowCube; //  Omni-directional shadows
Texture2DArray<float> punctualShadow2D;     // Spotlight shadows

// Blue noise tile for the RT shadow dither. Declared before the TLAS so the
// NoRT variant still gets both bindings -- only the trailing hole moves.
Texture2D<float4> blueNoiseTex;
SamplerState blueNoiseSampler;

// Emissive G-Buffer channel. It is added to the composed result instead of
// being multiplied by incident light: emission is what a surface radiates on
// its own, so it has to survive shadow, night and back-facing geometry.
// Declared after the blue noise pair and before the TLAS for the same reason
// they are -- the optional TLAS slot must stay at the tail.
Texture2D<float4> texEmissive;

// Half-resolution GTAO occlusion from the dedicated GtaoAo pass
// (ao_gtao.slang). Sampled only in giMode 3/4; bound in both variants --
// like texEmissive it sits before the optional TLAS so the tail stays
// aligned when the NoRT variant drops the TLAS declaration.
Texture2D<float> texAo;

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

// Ambient / GI term -- returns float4(GI + SH sky ambient, AO). The AO-only GTAO modes
// (giMode 3/4) come from the dedicated half-resolution GtaoAo pass (ao_gtao.slang) and are
// upsampled here; the cheaper modes (sample AO / SSGI gather) stay inline.
float4 ComputeAmbientAndAO(float2 uv, uint2 pixelPos, float3 N, float3 worldPos, float viewDepth) {
    float  ao            = 1.0f;
    float3 indirectLight = float3(0.0f, 0.0f, 0.0f);

    float2 edgeFactor = smoothstep(0.0f, 0.08f, uv) * smoothstep(1.0f, 0.92f, uv);
    float  screenFade = edgeFactor.x * edgeFactor.y;

    // --- DEFINE SMOOTH FADE BOUNDARY FROM 40m TO 50m
    float fade = saturate((50.0f - viewDepth) / 10.0f);

    if (pc.giMode > 0 && fade > 0.0f) {
        float occlusion = 0.0f;
        uint dw, dh;
        texDepth.GetDimensions(dw, dh);

        if (pc.giMode == 3 || pc.giMode == 4) {
            // GTAO runs in the dedicated half-resolution GtaoAo pass
            // (ao_gtao.slang); here we depth-weighted-upsample its R8
            // target. The four taps are the nearest half-res texel centers;
            // each weight combines the bilinear fraction with an edge-stop
            // on view depth so silhouettes keep a crisp terminator instead
            // of bleeding the occlusion of the foreground over the sky (or
            // vice versa).
            uint aoW, aoH;
            texAo.GetDimensions(aoW, aoH);
            const float2 halfSize = float2(float(aoW), float(aoH));
            const float2 pos      = uv * halfSize - 0.5f;
            const float2 corner   = floor(pos);
            const float2 blend    = pos - corner;

            float weightSum = 0.0f;
            float aoSum     = 0.0f;
            for (int tapY = 0; tapY <= 1; ++tapY) {
                for (int tapX = 0; tapX <= 1; ++tapX) {
                    const float2 tapOff = float2(float(tapX), float(tapY));
                    const float  bilinear =
                        lerp(1.0f - blend.x, blend.x, tapOff.x) * lerp(1.0f - blend.y, blend.y, tapOff.y);
                    const float2 tuv    = (corner + tapOff + 0.5f) / halfSize;
                    const float  tapAo  = texAo.SampleLevel(pointSampler, tuv, 0).r;
                    const float  tapRaw = texDepth.SampleLevel(pointSampler, tuv, 0).r;
                    const float3 tapPos = ReconstructWorldPos(tuv, tapRaw, pc.invViewProj);
                    const float  tapViewDepth = mul(frame.unjitteredViewProj, float4(tapPos, 1.0f)).w;
                    const float  edgeStop = exp(-abs(tapViewDepth - viewDepth) / max(viewDepth * 0.02f, 1e-3f));
                    const float  w        = bilinear * edgeStop;
                    weightSum += w;
                    aoSum += w * tapAo;
                }
            }
            ao = (weightSum > 0.0f) ? (aoSum / weightSum) : 1.0f;

        } else {
            float angle    = GetRotationAngle(uv, pc.camPos.w);
            float cosTheta = cos(angle * 2.0f * 3.14159265f);
            float sinTheta = sin(angle * 2.0f * 3.14159265f);

            // Loop invariants, hoisted: the tangent frame depends only on N,
            // the projection of the shading point and its camera-relative
            // vector only on worldPos. Rebuilding these per sample was 8-16x
            // redundant work; the per-sample projection of the offset point
            // exploits linearity: viewProj*(P + r*d) = viewProj*P + r*(viewProj*d).
            float3 up        = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
            float3 tangent   = normalize(cross(up, N));
            float3 bitangent = cross(N, tangent);
            float4 centerClip = mul(pc.viewProj, float4(worldPos, 1.0f));

            int effectiveSamples = min(pc.giSamples, 16);
            for (int s = 0; s < effectiveSamples; ++s) {
                float3 sampleOffset = HemisphereSamples[s % 8];
                float3 rotatedSample =
                    float3(sampleOffset.x * cosTheta - sampleOffset.y * sinTheta, sampleOffset.x * sinTheta + sampleOffset.y * cosTheta, sampleOffset.z);
                float3 dir       = rotatedSample.x * tangent + rotatedSample.y * bitangent + rotatedSample.z * N;
                float3 offsetPos = dir * pc.aoRadius;

                float4 clipPos = centerClip + mul(pc.viewProj, float4(offsetPos, 0.0f));
                // Native Vulkan positive coordinate mapping mapping [-1, 1] to [0, 1]
                float2 uv_sample = (clipPos.xy / clipPos.w) * 0.5f + 0.5f;

                if (any(uv_sample < 0.0f) || any(uv_sample > 1.0f))
                    continue;

                float sampleRawDepth = texDepth.SampleLevel(pointSampler, uv_sample, 0).r;
                if (sampleRawDepth >= 1.0f)
                    continue;

                // Compare view-axis depths instead of Euclidean camera
                // distances: the test point's linear view depth is the
                // clip-space w already computed for its projection, and the
                // sample's comes from the same invProj linearization the
                // A-Trous denoiser uses. This drops the 4x4 world
                // reconstruction and both square roots per sample. Near the
                // screen edges the view axis diverges from the line of
                // sight, so the occlusion threshold shifts slightly there --
                // the standard depth-buffer AO approximation.
                float4 linClip = mul(frame.invProj, float4(0.0f, 0.0f, sampleRawDepth, 1.0f));
                float diff     = clipPos.w - abs(linClip.z / linClip.w);

                if (diff > 0.0f) {
                    float finalWeight = smoothstep(0.0f, pc.aoBias * 2.0f, diff - pc.aoBias) * smoothstep(0.0f, 1.0f, pc.aoRadius / max(abs(diff), 0.001f));
                    if (pc.giMode == 1) {
                        occlusion += finalWeight;
                    } else if (pc.giMode == 2) {
                        // Emitters bounce: the gather reads lit scene colour
                        // plus the emissive channel, which the lighting pass
                        // adds separately and so is absent from texInput.
                        float3 sampleRadiance = texInput.SampleLevel(smp, uv_sample, 0).rgb + texEmissive.SampleLevel(smp, uv_sample, 0).rgb;
                        indirectLight += sampleRadiance * max(dot(N, dir), 0.0f) * finalWeight;
                    }
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

    float4 albedoRaw = texInput.SampleLevel(pointSampler, input.uv, 0);
    float3 emissive  = texEmissive.SampleLevel(pointSampler, input.uv, 0).rgb;
    if (frame.fullBright != 0) {
        return float4(albedoRaw.rgb + emissive, 1.0f);
    }
    float4 normRoughRaw = texNormalRoughness.SampleLevel(pointSampler, input.uv, 0);
    float3 N            = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);
    float roughness     = normRoughRaw.z;
    float metallic      = normRoughRaw.w;

    float3 worldPos = ReconstructWorldPos(input.uv, depth, pc.invViewProj);

    float viewDepth = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;

    // Safe normalize: when camera is extremely close to surface (worldPos ≈ camPos),
    // length is ~0 and normalize produces NaN that propagates to NaN lighting → black square.
    float3 V    = SafeNormalize(pc.camPos.xyz - worldPos, N);
    float3 F0   = lerp(float3(0.04f, 0.04f, 0.04f), albedoRaw.rgb, metallic);
    float NdotV = saturate(dot(N, V));

    float4 ambientData         = ComputeAmbientAndAO(input.uv, uint2(input.pos.xy), N, worldPos, viewDepth);
    float3 ambientDiffuseAndGI = ambientData.rgb;
    float ao                   = ambientData.a;

    // --- 1. Direct Sun Light
    float3 L_sun    = SafeNormalize(frame.lightDir.xyz, float3(0.0f, 1.0f, 0.0f));
    float3 H_sun    = SafeNormalize(V + L_sun, N);
    float NdotL_sun = saturate(dot(N, L_sun));

    float shadow = 1.0f;

    // Cascade shadow maps: analytic PCSS, deterministic per pixel. Computed
    // whenever they will contribute (always, except for RT-shadow pixels
    // nearer than the fade start, which never sample them).
    float cascadeShadow = 1.0f;
    bool  needCascade   = true;
    bool  rtrBlendBand  = false;
#ifndef DISABLE_RTR
    needCascade  = !(ENABLE_RTR != 0 && pc.enableRTR != 0 && viewDepth < 80.0f);
    rtrBlendBand = (ENABLE_RTR != 0 && pc.enableRTR != 0 && viewDepth >= 80.0f && viewDepth < 160.0f);
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

        // In the 80-160 m RT-blend band the cascade result is lerped against
        // the ray-traced shadow, so half the PCSS taps are invisible under
        // the blend; >= 160 m keeps the full kernel. The reduced variant
        // strides the same fixed Poisson disks -- deterministic, no new
        // temporal noise.
        cascadeShadow = CalculateShadowPCSS(
            shadowPos, N, L_sun, input.pos.xy, shadowMap, shadowSampler, pointSampler, frame.shadowResolution, cascadeIndex, worldDepthPerNdc, rtrBlendBand
        );
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

    // --- 2. Punctual and Area Lights (Clustered)
    float3 directPunctual = float3(0.0f, 0.0f, 0.0f);

    uint cIdx = ClusterIndex(uint(input.uv.x * float(Cluster.Width)), uint(input.uv.y * float(Cluster.Height)), ViewDepthToClusterZWith(viewDepth, frame.nearZ, frame.farZ));

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
            float3 L_unnorm = light.position - worldPos;
            float range     = max(light.range, 0.001f);
            float distSq    = dot(L_unnorm, L_unnorm);
            // Outside the light's sphere the squared window term is exactly
            // zero, so reject before paying for the sqrt, the normalize, and
            // the GGX evaluation gated on NdotL below.
            if (distSq >= range * range)
                continue;
            float distToCenter = sqrt(distSq);
            float3 L_p         = L_unnorm / (distToCenter + 1e-5f);
            float NdotL        = max(dot(N, L_p), 0.0f);

            float distRatio = distToCenter / range;
            float window    = saturate(1.0f - distRatio * distRatio);
            window *= window;
            float atten = window / (distSq + 1.0f);

            if (NdotL > 0.0f) {
                float3 H   = SafeNormalize(V + L_p, N);
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

    // AO occludes only the ambient + gathered-GI term: direct sun/punctual
    // light already carries real shadow visibility, so multiplying ao into
    // those would double-darken sunlit contact edges. The factor still rides
    // in alpha for the reflection pass, which applies it to the specular IBL.
    float3 litColor = ambientDiffuseAndGI * albedoRaw.rgb * ao + directSun + directPunctual + emissive;
    return float4(litColor, ao);
}
