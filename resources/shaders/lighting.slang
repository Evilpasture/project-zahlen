
import cluster_grid;
import cluster_math;
import ddgi_common;
import descriptor_heap_layout;
import pbr_helpers;
import uniforms;

[vk::push_constant]
ScenePassPushConstants pc;

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

// Punctual shadows live in one 2D depth atlas that every shadow-casting spot
// and point light sub-allocates square tiles out of -- one tile for a spot, six
// for a point's cube faces. `ShadowAtlas` on the CPU owns the packing; this is
// the sampled half of it.
Texture2D<float> shadowAtlas;
StructuredBuffer<ShadowTile> shadowTiles;


Texture2D<float> texAo;

// Baked-light ownership from the G-buffer (Res_BakedMask), read as a bitfield:
//   bit 0 - a VRAD lightmap, or the leaf ambient cube standing in for one,
//           already supplied this pixel's static ambient through the emissive
//           overlay, so the analytic SH term below would double it,
//   bit 1 - static *direct* light is baked here as well, which only a lightmap
//           is: a cube carries indirect only, so a prop riding one keeps its
//           live sun.
Texture2D<uint> texBakedMask;


#ifndef DISABLE_RTR
Texture2D<float> texRtShadowMask;
Texture2D<float4> texDdgiIrradiance;
Texture2D<float2> texDdgiDistance;
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

    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

float4 ComputeAmbientAndAO(float2 uv, uint2 pixelPos, float3 N, float3 worldPos, float viewDepth, float bakedAmbient) {
    float  ao            = 1.0f;
    float3 indirectLight = float3(0.0f, 0.0f, 0.0f);

    float2 edgeFactor = smoothstep(0.0f, 0.08f, uv) * smoothstep(1.0f, 0.92f, uv);
    float  screenFade = edgeFactor.x * edgeFactor.y;

    float fade = saturate((50.0f - viewDepth) / 10.0f);

    if (pc.giMode > 0 && fade > 0.0f) {
        float occlusion = 0.0f;
        uint dw, dh;
        texDepth.GetDimensions(dw, dh);

        if (pc.giMode == 3 || pc.giMode == 4) {
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
                    const float3 tapN   = UnpackNormalOctahedron(texNormalRoughness.SampleLevel(pointSampler, tuv, 0).xy * 2.0f - 1.0f);
                    const float  planeDistance = abs(dot(tapPos - worldPos, N));
                    const float  edgeStop = exp(-planeDistance / max(viewDepth * 0.02f, 1e-3f)) * pow(saturate(dot(N, tapN)), 16.0f);
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
                float2 uv_sample = (clipPos.xy / clipPos.w) * 0.5f + 0.5f;

                if (any(uv_sample < 0.0f) || any(uv_sample > 1.0f))
                    continue;

                float sampleRawDepth = texDepth.SampleLevel(pointSampler, uv_sample, 0).r;
                if (sampleRawDepth >= 1.0f)
                    continue;

                float4 linClip = mul(frame.invProj, float4(0.0f, 0.0f, sampleRawDepth, 1.0f));
                float diff     = clipPos.w - abs(linClip.z / linClip.w);

                if (diff > 0.0f) {
                    float finalWeight = smoothstep(0.0f, pc.aoBias * 2.0f, diff - pc.aoBias) * smoothstep(0.0f, 1.0f, pc.aoRadius / max(abs(diff), 0.001f));
                    if (pc.giMode == 1) {
                        occlusion += finalWeight;
                    } else if (pc.giMode == 2) {
                        float3 sampleRadiance = texInput.SampleLevel(smp, uv_sample, 0).rgb;
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

    // Ambient is the only term this function owns, and the only one that is
    // ownership-dependent: a baked pixel already carries the map's static
    // ambient -- sky term and bounced light alike -- in its lightmap or leaf
    // ambient cube, which PSEmissive adds back verbatim, so the analytic SH
    // floor is withheld there. What survives on a baked pixel is the DDGI
    // term's live half, added below.
    const float3 shAmbient = EvaluateSH(N, frame.sh) * frame.ambientExposure;
    float3       baselineAmbient = shAmbient * (1.0f - bakedAmbient);
#ifndef DISABLE_RTR
    if (ENABLE_RTR != 0 && frame.ddgiParams.w > 0.0f) {
        float3 V_amb = SafeNormalize(pc.camPos.xyz - worldPos, N);
        // SampleDDGIOrSH resolves to max(probe field, SH floor). Only the part
        // above that floor is live GI -- the flashlight's bounce, a door opening
        // onto a baked wall -- and it belongs to baked and unbaked surfaces
        // alike, because no bake ever saw it. Unbaked pixels therefore still
        // resolve to exactly max(probes, shAmbient), i.e. the previous result.
        const float3 probes = SampleDDGIOrSH(worldPos, N, V_amb, frame, texDdgiIrradiance, texDdgiDistance, clampSampler);
        baselineAmbient += max(probes - shAmbient, float3(0.0f));
    }
#endif
    indirectLight += baselineAmbient;

    return float4(indirectLight, ao);
}

struct LightingOutput {
    float4 color : SV_Target0;
    float ao : SV_Target1;
};

LightingOutput PSMain(VSOutput input) {
    float depth = texDepth.SampleLevel(pointSampler, input.uv, 0);
    // One world, one depth range (the band is retired): 1.0 means empty.
    if (depth >= 1.0f)
        discard;

    float4 albedoRaw = texInput.SampleLevel(pointSampler, input.uv, 0);
    if (frame.fullBright != 0) {
        LightingOutput outLighting;
        outLighting.color = float4(albedoRaw.rgb, 1.0f);
        outLighting.ao = 1.0f;
        return outLighting;
    }
    float4 normRoughRaw = texNormalRoughness.SampleLevel(pointSampler, input.uv, 0);
    float3 N            = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);
    float roughness     = normRoughRaw.z;
    float metallic      = normRoughRaw.w;

    float3 worldPos = ReconstructWorldPos(input.uv, depth, pc.invViewProj);

    float viewDepth = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;

    float3 V    = SafeNormalize(pc.camPos.xyz - worldPos, N);
    float NdotV = saturate(dot(N, V));

    float3 F0 = lerp(float3(kDielectricF0), albedoRaw.rgb, metallic);
    float  f90 = 1.0f;
    // The compact deferred path is isotropic. Materials that need tangent-space
    // extension lobes are rendered in ForwardPass.
    float materialAo = 1.0f;
    float anisoStrength = 0.0f;
    float3 T = N;
    float3 B = N;

    // One texel fetch of the ownership field, used by both halves of this
    // shader: the ambient gate below and the direct-light gates further down.
    const uint  bakedBits    = texBakedMask.Load(int3(int2(input.pos.xy), 0));
    const float bakedAmbient = float(bakedBits & 1u);
    const float bakedDirect  = float((bakedBits >> 1u) & 1u);

    float4 ambientData         = ComputeAmbientAndAO(input.uv, uint2(input.pos.xy), N, worldPos, viewDepth, bakedAmbient);
    float3 ambientDiffuseAndGI = ambientData.rgb;
    float ao                   = ambientData.a;

    float3 L_sun    = SafeNormalize(frame.lightDir.xyz, float3(0.0f, 1.0f, 0.0f));
    float3 H_sun    = SafeNormalize(V + L_sun, N);
    float NdotL_sun = saturate(dot(N, L_sun));

    float shadow = 1.0f;

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

        float4 lightBasisX     = mul(frame.lightSpaceMatrices[cascadeIndex], float4(1.0f, 0.0f, 0.0f, 0.0f));
        float4 lightBasisZ     = mul(frame.lightSpaceMatrices[cascadeIndex], float4(0.0f, 0.0f, 1.0f, 0.0f));
        float  ndcPerWorldX    = max(length(lightBasisX.xyz), 1e-6f);
        float  texelSizeWorld  = 2.0f / (ndcPerWorldX * float(frame.shadowResolution));
        float  worldDepthPerNdc = 1.0f / max(abs(lightBasisZ.z), 1e-8f);
        float normalBias      = saturate(1.0 - dot(N, L_sun)) * 1.5 + 0.2;
        float3 biasedWorldPos = worldPos + N * (normalBias * texelSizeWorld);

        float4 shadowPos = mul(frame.lightSpaceMatrices[cascadeIndex], float4(biasedWorldPos, 1.0f));

        shadowPos.xy = shadowPos.xy * 0.5f + 0.5f * shadowPos.w;

        cascadeShadow = CalculateShadowPCSS(
            shadowPos, N, L_sun, input.pos.xy, shadowMap, shadowSampler, pointSampler, frame.shadowResolution, cascadeIndex, worldDepthPerNdc, rtrBlendBand
        );
    }
    shadow = cascadeShadow;

#ifndef DISABLE_RTR
    if (ENABLE_RTR != 0 && pc.enableRTR != 0 && viewDepth < 160.0f) {
        // texRtShadowMask is traced and denoised below native resolution, so it
        // has to be filtered up. Filtering by hand keeps this off the heap
        // sampler table -- and the previous point sample was the direct cause of
        // shadow crawl, since one texel of a temporally accumulated mask was
        // being handed to a full-resolution pixel.
        float2 maskDims;
        texRtShadowMask.GetDimensions(maskDims.x, maskDims.y);
        float2 maskCoord = input.uv * maskDims - 0.5f;
        int2   maskBase  = int2(floor(maskCoord));
        float2 maskFrac  = frac(maskCoord);
        float  maskSum    = 0.0f;
        float  maskWeight = 0.0f;
        [unroll]
        for (int maskY = 0; maskY <= 1; ++maskY) {
            [unroll]
            for (int maskX = 0; maskX <= 1; ++maskX) {
                int2  tap  = clamp(maskBase + int2(maskX, maskY), int2(0, 0), int2(maskDims) - 1);
                float wBil = lerp(1.0f - maskFrac.x, maskFrac.x, float(maskX)) *
                             lerp(1.0f - maskFrac.y, maskFrac.y, float(maskY));
                maskSum    += texRtShadowMask.Load(int3(tap, 0)).r * wBil;
                maskWeight += wBil;
            }
        }
        float rtrShadow = (maskWeight > 1e-5f) ? (maskSum / maskWeight) : 1.0f;
        if (viewDepth < 80.0f) {
            shadow = rtrShadow;
        } else {
            float t         = saturate((viewDepth - 80.0f) / 80.0f);
            t               = t * t * (3.0f - 2.0f * t);
            shadow          = lerp(rtrShadow, cascadeShadow, t);
        }
    }
#endif

    float3 F     = SpecularFresnel(saturate(dot(H_sun, V)), F0, f90);
    float3 spec  = SpecularGGX(N, V, L_sun, H_sun, roughness, F, T, B, anisoStrength);
    float3 kD_sun = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0f - metallic);

    float3 sunRadiance = frame.sunRadiance.xyz;
    // A lightmapped map's sun was compiled into the bake together with its
    // shadows, so its diffuse is withheld wherever the lightmap owns direct
    // light. The specular lobe is not: no lightmap stores one, so the highlight
    // on a lightmapped surface has to come from here or not at all.
    const float sunDiffuseOwnership = 1.0f - bakedDirect;
    float3 directSun = (kD_sun * albedoRaw.rgb / 3.14159265f * sunDiffuseOwnership + spec) * sunRadiance * NdotL_sun * shadow;

    float3 directPunctual = float3(0.0f, 0.0f, 0.0f);

    uint cIdx = ClusterIndex(uint(input.uv.x * float(Cluster.Width)), uint(input.uv.y * float(Cluster.Height)), ViewDepthToClusterZWith(viewDepth, frame.nearZ, frame.farZ));

    ClusterVolume cluster = clusterGrid[cIdx];
    uint lightListCount   = min(cluster.count, 64u);

    for (uint i = 0; i < lightListCount; ++i) {
        uint l = clusterIndexList[cluster.offset + i];
        if (l >= frame.lightCount)
            continue;

        Light light = lights[l];
        // A light the map compiler baked owns its diffuse only where the bake
        // owns it; unbaked pixels -- props, entities, skybox scenery -- still
        // need the live light.
        //
        // Specular follows the same rule for an *authored* light, for the reason
        // the sun's does: no lightmap stores a lobe, so the highlight on a
        // lightmapped surface has to come from here or not at all. It does not
        // follow it for an emissive-surface proxy (baked == 2). That proxy is
        // the importer's reconstruction of a lamp panel or screen the same
        // compile already integrated into the same lightmap, it sits
        // centimetres off its own mounting surface, and Source's lamp panels
        // have no specular lobe at all -- so a live one paints a clipped GGX
        // highlight across the floor beside every fixture in the map.
        const float lightDiffuseOwnership  = (light.baked != 0u) ? (1.0f - bakedDirect) : 1.0f;
        const float lightSpecularOwnership = (light.baked > 1u) ? (1.0f - bakedDirect) : 1.0f;
        if (light.type == LightType::Area) {
            float2 uv      = float2(roughness, sqrt(1.0f - NdotV));
            float4 t1      = ltc_mat.SampleLevel(clampSampler, uv, 0.0f);
            float3x3 Minv  = float3x3(float3(t1.x, 0.0f, t1.y), float3(0.0f, 1.0f, 0.0f), float3(t1.z, 0.0f, t1.w));
            float4 t2      = ltc_amp.SampleLevel(clampSampler, uv, 0.0f);
            float2 schlick = float2(t2.x, t2.y);

            float3 specLTC = LTC_Evaluate(N, V, worldPos, Minv, light.points, light.twoSided == 1);
            // The LTC's second coefficient integrates the grazing lobe, so it
            // scales with f90 rather than with one-minus-F0.
            specLTC *= F0 * schlick.x + float3(f90) * schlick.y;

            float3 diffLTC = LTC_Evaluate(N, V, worldPos, float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1), light.points, light.twoSided == 1);

            const float shadowVisibility = 1.0f;

            directPunctual += ((1.0f - metallic) * albedoRaw.rgb * diffLTC * lightDiffuseOwnership + specLTC * lightSpecularOwnership) *
                              light.color * light.intensity * shadowVisibility;
        } else {
            if (light.type == LightType::Directional || light.type == LightType::Sun)
                continue;
            float3 L_unnorm = light.position - worldPos;
            float range     = max(light.range, 0.001f);
            float distSq    = dot(L_unnorm, L_unnorm);
            if (distSq >= range * range)
                continue;
            float distToCenter = sqrt(distSq);
            float3 L_p         = L_unnorm / (distToCenter + 1e-5f);
            float NdotL        = max(dot(N, L_p), 0.0f);

            float distRatio = distToCenter / range;
            float window    = saturate(1.0f - distRatio * distRatio);
            window *= window;
            float atten = window / (distSq + 1.0f);

            // Spotlight cone falloff: lerp between the outer and inner cone
            // cosines. Lights without an authored cone pack -1/-1 (see
            // PackLight), which skips this branch and keeps the full sphere.
            if (light.type == LightType::Spot && light.outerConeCos > -0.5f) {
                float3 spotDir   = normalize(light.direction);
                float  cosAngle  = dot(-L_p, spotDir);
                float  spotAtten = saturate((cosAngle - light.outerConeCos) / max(light.innerConeCos - light.outerConeCos, 1e-4f));
                atten *= spotAtten * spotAtten;
            }

            if (NdotL > 0.0f) {
                float3 H   = SafeNormalize(V + L_p, N);
                float3 F_p = SpecularFresnel(max(dot(H, V), 0.0f), F0, f90);
                float3 spec_p = SpecularGGX(N, V, L_p, H, roughness, F_p, T, B, anisoStrength);
                float3 kD_p   = (1.0f - F_p) * (1.0f - metallic);

                float shadowVisibility = 1.0f;
                if (light.shadowTileBase >= 0) {
                    // A point light is six atlas tiles, one per cube face in
                    // Vulkan layer order (+X,-X,+Y,-Y,+Z,-Z); a spot is one.
                    uint face = 0u;
                    if (light.type == LightType::Point || light.outerConeCos <= -0.5f) {
                        float3 r  = worldPos - light.position;
                        float3 ar = abs(r);
                        if (ar.x >= ar.y && ar.x >= ar.z) {
                            face = (r.x > 0.0f) ? 0u : 1u;
                        } else if (ar.y >= ar.z) {
                            face = (r.y > 0.0f) ? 2u : 3u;
                        } else {
                            face = (r.z > 0.0f) ? 4u : 5u;
                        }
                    }

                    // Both light kinds then take the same path: the tile's own
                    // matrix is the one the rasterizer drew with, so the depth
                    // written and the depth compared are the same quantity.
                    ShadowTile tile = shadowTiles[light.shadowTileBase + face];
                    float4 sPos     = mul(tile.viewProj, float4(worldPos + N * 0.05f, 1.0f));
                    if (sPos.w > 0.0f) {
                        float2 tileUv  = sPos.xy / sPos.w * 0.5f + 0.5f;
                        float2 atlasUv = tile.uvOrigin + tileUv * tile.uvScale;
                        float ndl        = max(NdotL, 1e-3f);
                        float slope      = sqrt(max(0.0f, 1.0f - ndl * ndl)) / ndl;
                        float depthBias  = 0.0005f + 0.0025f * min(slope, 8.0f);
                        shadowVisibility = shadowAtlas.SampleCmpLevelZero(shadowSampler, atlasUv, sPos.z / sPos.w - depthBias);
                    }
                }

                directPunctual += (kD_p * albedoRaw.rgb / 3.14159265f * lightDiffuseOwnership + spec_p * lightSpecularOwnership) *
                                  light.color * light.intensity * atten * NdotL * shadowVisibility;
            }
        }
    }

    // Ambient SH/GI is diffuse irradiance. Metals have no diffuse lobe; adding
    // it unconditionally to the specular IBL in reflection.slang made rough
    // metal look white even with physically correct exposure and no clearcoat.
    float3 kD_ambient = (1.0f - SpecularFresnelRoughness(NdotV, F0, f90, roughness)) * (1.0f - metallic);
    // Indirect light softens occlusion: the SH/GI term is a measured or
    // gathered bounce field that already contains interreflection, so
    // multiplying single-bounce AO in at full strength double-darkens
    // enclosed rooms (small rooms crushed to a fraction of their baked
    // level while open spaces matched). Half-strength AO keeps contact
    // darkening without erasing the GI it is supposed to modulate.
    float aoAmbient = lerp(1.0f, ao, 0.5f);
    float3 litColor = ambientDiffuseAndGI * albedoRaw.rgb * kD_ambient * aoAmbient * materialAo + directSun + directPunctual;
    LightingOutput outLighting;
    outLighting.color = float4(litColor, 1.0f);
    outLighting.ao = saturate(ao);
    return outLighting;
}
