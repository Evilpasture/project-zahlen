
import common;
import ddgi_common;
import gpu_buffer;
import instance_data;
import material_model;
import math.transform;
import pbr_helpers;
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
    float4 camPos;
    int giMode;
    float aoRadius;
    float aoBias;
    float aoPower;
    float giIntensity;
    int giSamples;
    int enableSSR;
    int enableRTR;
    int _pad;
};
[vk::push_constant]
PushConstants pc;

Texture2D<float4> texInput;
SamplerState smp;
Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
SamplerState pointSampler;
TextureCube<float4> texEnvMap; // conditioned IBL, never used as the cooked visible sky
Texture2D<float4> texSkyEquirect; // original HDR only when environmentMode == 3
SamplerState skySampler;
ConstantBuffer<FrameUniforms> frame;
Texture2D brdfLUT;
SamplerState clampSampler;
Texture2D<float4> texLighting;
Texture3D<float4> texVoxelIntegrated;
StructuredBuffer<InstanceData> g_instances;

Texture2D<float4> blueNoiseTex;
SamplerState blueNoiseSampler;

Texture2D<float4> texRtrHalf;

Texture2D<float4> texClearcoat;
Texture2D<float4> texSheen;
Texture2D<float4> texAnisotropy;

#ifndef DISABLE_RTR
RaytracingAccelerationStructure tlas;
Texture2D<float4> texDdgiIrradiance;
Texture2D<float2> texDdgiDistance;

float2 RaytraceRTR(float3 worldPos, float3 N, float3 V, float roughness, float3 anisT, float3 anisB, float strength,
                   out float confidence, out float3 fallbackColor, uint2 pixelPos) {
    confidence    = 0.0f;
    fallbackColor = float3(0.0f, 0.0f, 0.0f);

    float NdotV = dot(N, V);
    if (NdotV <= 1e-4f) {
        return float2(-1.0f, -1.0f);
    }

    float3 R;
    if (roughness < 0.04f && strength <= 1.0f / 255.0f) {
        R = reflect(-V, N);
    } else {
        float3 up        = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
        float3 tangentX  = strength > 1.0f / 255.0f ? anisT : normalize(cross(up, N));
        float3 tangentY  = strength > 1.0f / 255.0f ? anisB : cross(N, tangentX);
        float3x3 toLocal = float3x3(tangentX, tangentY, N);
        float3x3 toWorld = transpose(toLocal);

        float3 V_local = mul(toLocal, V);
        float2 u       = SampleBlueNoise(blueNoiseTex, blueNoiseSampler, pixelPos, WrapFrameIndex(frame.frameIndex)).zw;
        float2 alpha   = strength > 1.0f / 255.0f ? AnisotropicAlphas(roughness, strength) : float2(max(roughness * roughness, 1e-5f));
        float3 H_local = SampleGGX_VNDF(V_local, alpha, u);
        float3 H       = normalize(mul(toWorld, H_local));

        R = reflect(-V, H);
        if (dot(R, N) <= 0.0f) {
            R = reflect(-V, N);
        }
    }
    float maxDist = lerp(50.0f, 16.0f, roughness);

    RayDesc ray;
    ray.Origin    = worldPos + N * 0.02f;
    ray.Direction = R;
    ray.TMin      = 0.01f;
    ray.TMax      = maxDist;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
    // World first, background layer on miss: the map wins every reflection
    // pixel it covers even where skybox scenery sits nearer (Source's two
    // worlds never race in one depth buffer).
    q.TraceRayInline(tlas, RAY_FLAG_NONE, kRayMaskWorld, ray);
    while (q.Proceed()) {
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        q.TraceRayInline(tlas, RAY_FLAG_NONE, kRayMaskSkyboxLayer, ray);
        while (q.Proceed()) {
        }
    }

    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float rayT         = q.CommittedRayT();
        float3 hitWorldPos = ray.Origin + ray.Direction * rayT;

        float4 fastClip = mul(frame.viewProj, float4(hitWorldPos, 1.0f));
        if (fastClip.w > 0.1f) {
            float2 fastUV = fastClip.xy / fastClip.w * 0.5f + 0.5f;
            if (all(fastUV >= 0.02f) && all(fastUV <= 0.98f)) {
                float sampledDepth = texDepth.SampleLevel(pointSampler, fastUV, 0);
                float4 linClip      = mul(frame.invProj, float4(0.0f, 0.0f, sampledDepth, 1.0f));
                float distToSampled = abs(linClip.z / linClip.w);
                if (abs(fastClip.w - distToSampled) < max(0.5f, fastClip.w * 0.05f)) {
                    confidence    = 1.0f;
                    fallbackColor = texLighting.SampleLevel(pointSampler, fastUV, 0).rgb;
                    return fastUV;
                }
            }
        }

        uint instanceID      = q.CommittedInstanceID();
        InstanceData hitInst = g_instances[instanceID];

        float3 hitNormal = float3(0.0f, 1.0f, 0.0f);
        if (hitInst.posAddress != 0) {
            uint primIdx = q.CommittedPrimitiveIndex();
            uint i0      = primIdx * 3 + 0;
            uint i1      = primIdx * 3 + 1;
            uint i2      = primIdx * 3 + 2;

            if (hitInst.iboAddress != 0) {
                GPUBuffer<uint> indices = GPUBuffer<uint>(hitInst.iboAddress);
                i0 = indices.load(i0);
                i1 = indices.load(i1);
                i2 = indices.load(i2);
            }

            GPUBuffer<float> posValues = GPUBuffer<float>(hitInst.posAddress);
            float3 p0 = float3(posValues.load(i0 * 3 + 0), posValues.load(i0 * 3 + 1), posValues.load(i0 * 3 + 2));
            float3 p1 = float3(posValues.load(i1 * 3 + 0), posValues.load(i1 * 3 + 1), posValues.load(i1 * 3 + 2));
            float3 p2 = float3(posValues.load(i2 * 3 + 0), posValues.load(i2 * 3 + 1), posValues.load(i2 * 3 + 2));

            float3x3 world3x3 = (float3x3) hitInst.world;
            float3 crossN     = cross(mul(world3x3, p1 - p0), mul(world3x3, p2 - p0));
            float lenSq       = dot(crossN, crossN);
            if (lenSq > 1e-7f) {
                hitNormal = crossN * rsqrt(lenSq);
            }
        }

        if (dot(hitNormal, ray.Direction) > 0.0f) {
            hitNormal = -hitNormal;
        }

        float3 hitBaseColor = max(hitInst.baseColorFactor.rgb, float3(0.0f));
        float3 emissive     = max(hitInst.emissiveFactor.rgb, float3(0.0f));
        if (hitInst.isUnlit) {
            fallbackColor = hitBaseColor + emissive;
        } else {
            float3 ambientLight = SampleDDGIOrSH(hitWorldPos, hitNormal, -ray.Direction, frame, texDdgiIrradiance, texDdgiDistance, clampSampler);
            fallbackColor       = hitBaseColor * (1.0f - saturate(hitInst.metallicFactor)) * ambientLight + emissive;
        }
        float offscreenConfidence = saturate(1.0f - rayT / max(maxDist, 1e-3f));
        confidence                = offscreenConfidence;

        float4 hitClip = mul(frame.viewProj, float4(hitWorldPos, 1.0f));
        if (hitClip.w < 0.1f) {
            return float2(-1.0f, -1.0f);
        }

        float2 hitNDC = hitClip.xy / hitClip.w;
        float2 hitUV  = hitNDC * 0.5f + 0.5f;

        float2 edgeFactor = smoothstep(0.0f, 0.05f, hitUV) * smoothstep(1.0f, 0.95f, hitUV);
        float onScreen    = edgeFactor.x * edgeFactor.y;

        if (onScreen > 0.0f) {
            float sampledRawDepth = texDepth.SampleLevel(pointSampler, hitUV, 0);

            float distToHit = hitClip.w;
            float4 linClip      = mul(frame.invProj, float4(0.0f, 0.0f, sampledRawDepth, 1.0f));
            float distToSampled = abs(linClip.z / linClip.w);
            float depthDiff     = abs(distToHit - distToSampled);

            float4 normRough      = texNormalRoughness.SampleLevel(pointSampler, hitUV, 0);
            float3 sampledNorm    = UnpackNormalOctahedron(normRough.xy * 2.0f - 1.0f);
            float normalAlignment = saturate(dot(hitNormal, sampledNorm));

            float maxDiff   = max(0.5f, distToHit * 0.05f);
            float depthMask = smoothstep(maxDiff * 1.5f, maxDiff * 0.2f, depthDiff) * smoothstep(0.4f, 0.8f, normalAlignment);

            if (depthMask > 0.1f) {
                confidence = saturate(onScreen * depthMask);
                return hitUV;
            }
        }

        return float2(-1.0f, -1.0f);
    }

    return float2(-1.0f, -1.0f);
}
// Source draws its 3D skybox as a second world behind the main one: the map
// keeps every pixel it covers and the skybox fills exactly the pixels the map
// leaves empty (the sky brushes). Rasterizing both into one depth buffer
// cannot express that rule -- a buried piece is nearer than the surface it
// must hide behind -- so background-layer instances never enter the G-buffer
// and this trace is their only primary-visibility path. The composition rule
// is structural: no authored scenery is dropped, moved or thinned, and
// nothing can overlap or sink into the world.
bool TryShadeSkyboxLayer(float3 origin, float3 dir, out float3 shaded) {
    shaded = float3(0.0f, 0.0f, 0.0f);

    RayDesc ray;
    ray.Origin    = origin;
    ray.Direction = dir;
    ray.TMin      = 0.01f;
    ray.TMax      = 5000.0f;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, kRayMaskSkyboxLayer, ray);
    while (q.Proceed()) {
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        return false;
    }

    uint instanceID        = q.CommittedInstanceID();
    InstanceData hitInst   = g_instances[instanceID];

    uint primIdx = q.CommittedPrimitiveIndex();
    uint i0      = primIdx * 3 + 0;
    uint i1      = primIdx * 3 + 1;
    uint i2      = primIdx * 3 + 2;
    if (hitInst.iboAddress != 0) {
        GPUBuffer<uint> indices = GPUBuffer<uint>(hitInst.iboAddress);
        i0 = indices.load(i0);
        i1 = indices.load(i1);
        i2 = indices.load(i2);
    }

    float2 baryUV = q.CommittedTriangleBarycentrics();
    float3 bary   = float3(1.0f - baryUV.x - baryUV.y, baryUV.x, baryUV.y);

    float4 w0, w1, w2;
    SceneGeometryVertex v0 = hitInst.EvaluateVertex<false>(i0, w0);
    SceneGeometryVertex v1 = hitInst.EvaluateVertex<false>(i1, w1);
    SceneGeometryVertex v2 = hitInst.EvaluateVertex<false>(i2, w2);

    float2 uv     = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    float2 uv1    = v0.uv1 * bary.x + v1.uv1 * bary.y + v2.uv1 * bary.z;
    float4 color  = v0.color * bary.x + v1.color * bary.y + v2.color * bary.z;
    float3 N      = SafeNormalize(v0.normal * bary.x + v1.normal * bary.y + v2.normal * bary.z, float3(0.0f, 1.0f, 0.0f));
    if (dot(N, dir) > 0.0f) {
        N = -N;
    }

    MaterialSampleInputs material = MakeMaterialInputs(uv, color, v0.materialIndices, v0.samplerCodes, v0.baseColorFactor, v0.emissiveFactor,
                                                       v0.pbrFactors, v0.alphaMode);
    ConfigureSceneMaterial(material, uv1, instanceID);
    float4 albedo  = SampleMaterialAlbedo<ClearCoatMaterial>(material);
    float3 emissive = SampleMaterialEmissive<ClearCoatMaterial>(material);

    if (frame.fullBright != 0) {
        shaded = albedo.rgb + emissive;
        return true;
    }

    float3 hitWorldPos = origin + dir * q.CommittedRayT();

    // Sun + sky irradiance + the authored fixture pools: the same windowed
    // inverse-square the clustered pass uses, over the scene light list the
    // skybox fixtures occupy. The range gate keeps the loop cheap on the
    // large background pixels of an outdoor frame.
    float3 L_sun    = SafeNormalize(frame.lightDir.xyz, float3(0.0f, 1.0f, 0.0f));
    float  NdotL_sun = saturate(dot(N, L_sun));
    float3 direct   = frame.sunRadiance.xyz * NdotL_sun;

    for (uint li = 0; li < frame.lightCount; ++li) {
        Light light = scene.lights[li];
        if (light.type == LightType::Directional || light.type == LightType::Sun || light.type == LightType::Area) {
            continue;
        }
        float3 L_unnorm = light.position - hitWorldPos;
        float  range    = max(light.range, 0.001f);
        float  distSq   = dot(L_unnorm, L_unnorm);
        if (distSq >= range * range) {
            continue;
        }
        float  distToCenter = sqrt(distSq);
        float3 L_p          = L_unnorm / (distToCenter + 1e-5f);
        float  NdotL        = max(dot(N, L_p), 0.0f);
        float  distRatio    = distToCenter / range;
        float  window       = saturate(1.0f - distRatio * distRatio);
        window *= window;
        float atten = window / (distSq + 1.0f);
        if (light.type == LightType::Spot && light.outerConeCos > -0.5f) {
            float3 spotDir  = normalize(light.direction);
            float  cosAngle = dot(-L_p, spotDir);
            float  spotAtten = saturate((cosAngle - light.outerConeCos) / max(light.innerConeCos - light.outerConeCos, 1e-4f));
            atten *= spotAtten * spotAtten;
        }
        direct += light.color * (light.intensity * atten * NdotL);
    }

    float3 ambient = SampleDDGIOrSH(hitWorldPos, N, -dir, frame, texDdgiIrradiance, texDdgiDistance, clampSampler);
    shaded         = albedo.rgb * (1.0f - saturate(hitInst.metallicFactor)) * (direct + ambient) + emissive;
    return true;
}
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
    float depth = texDepth.SampleLevel(pointSampler, input.uv, 0).r;
    float3 litColor;
    float viewDepth = 250.0f;

    if (depth >= 1.0f) {
        if (frame.environmentMode == 2) {
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        }

        float3 worldPos = ReconstructWorldPos(input.uv, 1.0f, pc.invViewProj);
        float3 rayDir   = SafeNormalize(worldPos - pc.camPos.xyz);

        // The 3D-skybox layer fills the pixels the world leaves empty
        // (Source's background composite); anything it misses falls through
        // to the authored sky below.
        bool filled = false;
#ifndef DISABLE_RTR
        filled = TryShadeSkyboxLayer(pc.camPos.xyz, rayDir, litColor);
#endif
        if (!filled) {
            if (frame.environmentMode == 3) {
                // Equirectangular convention matches ibl_bake.slang. The original
                // high-energy disk remains visible, but never enters specular IBL.
                float2 uv = float2(atan2(rayDir.z, rayDir.x) * (0.5f / 3.141592653589793f) + 0.5f,
                                   acos(clamp(rayDir.y, -1.0f, 1.0f)) / 3.141592653589793f);
                litColor = texSkyEquirect.SampleLevel(skySampler, uv, 0).rgb * frame.ambientExposure;
            } else if (frame.environmentMode == 1) {
                litColor = texEnvMap.SampleLevel(clampSampler, rayDir, 0).rgb * frame.ambientExposure;
            } else {
                rayDir = RotateVector(rayDir, frame.lightDir.xyz);

                float dy       = rayDir.y;
                float3 skyGrad = (dy >= 0.0f) ? lerp(frame.skyHorizon.rgb, frame.skyZenith.rgb, pow(saturate(dy), 1.2f))
                                              : lerp(frame.skyHorizon.rgb, frame.skyGround.rgb, pow(saturate(-dy), 0.5f));

                litColor = skyGrad * frame.ambientExposure;
            }
        }
    } else {
        float4 litColorRaw = texLighting.SampleLevel(pointSampler, input.uv, 0);
        if (frame.fullBright != 0) {
            return float4(litColorRaw.rgb, 1.0f);
        }
        float4 anisoRaw = texAnisotropy.SampleLevel(pointSampler, input.uv, 0);
        if (IsGBufferUnlit(anisoRaw.z)) {
            return float4(litColorRaw.rgb, 1.0f);
        }

        float4 normRoughRaw = texNormalRoughness.SampleLevel(pointSampler, input.uv, 0);
        float roughness     = normRoughRaw.z;
        float metallic      = normRoughRaw.w;

        float4 albedoRaw = texInput.SampleLevel(pointSampler, input.uv, 0);
        float3 N         = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);

        float3 worldPos = ReconstructWorldPos(input.uv, depth, pc.invViewProj);
        viewDepth       = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;

        float3 V = SafeNormalize(pc.camPos.xyz - worldPos, N);
        float strength = DecodeGBufferAnisotropy(anisoRaw.z);
        float3 T = strength > 0.0f ? UnpackAnisotropy(anisoRaw.xy, N) : float3(0.0f);
        float3 B = cross(N, T);
        float3 R = AnisotropicReflection(N, V, B, roughness, strength);

        float NdotV = saturate(dot(N, V));
        // KHR_materials_specular's dielectric response rides in the clearcoat
        // lanes for a material that has no coat. Sampling it here also feeds
        // the coat block below, which reads the same word.
        float4           coatRaw  = texClearcoat.SampleLevel(pointSampler, input.uv, 0);
        SpecularResponse specular = DecodeSpecularResponse(coatRaw);
        float3           F0       = lerp(float3(kDielectricF0) * specular.scale, albedoRaw.rgb, metallic);
        float2           envBRDF  = brdfLUT.SampleLevel(clampSampler, float2(NdotV, roughness), 0.0f).rg;
        // The LUT's A/B terms already integrate Schlick Fresnel over the GGX
        // lobe: A multiplies F0 and B the grazing value. Applying the grazing
        // Fresnel again here can wash out dark metal and amplify white glints
        // on masked foliage and thin chains.
        float3 FssEss  = F0 * envBRDF.x + float3(EffectiveF90(specular.f90, metallic)) * envBRDF.y;

        float3 R_corr = R;
        float boxFade = 0.0f;
        if (frame.probeMin.w > 0.0f) {
            float3 boxCenter      = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
            float3 boxExtent      = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
            float3 distFromCenter = abs(worldPos - boxCenter);
            float normDist        = max(max(abs(distFromCenter.x) / max(boxExtent.x, 0.0001f), abs(distFromCenter.y) / max(boxExtent.y, 0.0001f)),
                                        abs(distFromCenter.z) / max(boxExtent.z, 0.0001f));
            boxFade               = smoothstep(1.0f, 0.9f, normDist);
        }

        if (boxFade > 0.0f) {
            float3 boxR = BoxParallaxCorrection(worldPos, R, frame.probeMin.xyz, frame.probeMax.xyz, frame.probePos.xyz);
            R_corr      = lerp(R, boxR, boxFade);
        }

        float ao = litColorRaw.a * anisoRaw.w;

        float3 prefilteredColor = SampleAnisotropicEnvironment(texEnvMap, clampSampler, R_corr, roughness, strength) * frame.ambientExposure;
        float3 baseSpecularIBL = prefilteredColor * FssEss * ao;

        // Independent Charlie layer above metallic-roughness and beneath the
        // dielectric clearcoat. The existing GGX cube is an IBL approximation;
        // direct lighting uses Charlie's distribution and visibility.
        float4 sheenRaw = texSheen.SampleLevel(pointSampler, input.uv, 0);
        float sheenAtt = SheenAlbedoScaling(sheenRaw.rgb, sheenRaw.a, NdotV, NdotV);
        float3 sheenIBL = float3(0.0f);
        if (any(sheenRaw.rgb > 0.0f)) {
            float3 sheenPref = texEnvMap.SampleLevel(clampSampler, reflect(-V, N), max(sheenRaw.a, 0.2f) * 5.0f).rgb;
            sheenIBL = sheenPref * frame.ambientExposure * SheenEnvironmentWeight(sheenRaw.rgb, NdotV) * ao;
        }
        float coatAtt = 1.0f;
        float3 coatIBL = float3(0.0f);
        if (coatRaw.a > 1.0f / 255.0f) {
            float3 Nc    = UnpackNormalOctahedron(coatRaw.xy * 2.0f - 1.0f);
            float NcDotV = saturate(dot(Nc, V));
            float Fc     = ClearcoatFresnel(NcDotV) * coatRaw.a;
            coatAtt      = saturate(1.0f - Fc);
            float3 Rc    = reflect(-V, Nc);
            float3 coatPref = texEnvMap.SampleLevel(clampSampler, Rc, coatRaw.z * 5.0f).rgb * frame.ambientExposure;
            coatIBL = coatPref * Fc * ao;
        }

        litColor = litColorRaw.rgb;

        const bool wantRTR = (ENABLE_RTR != 0) && (pc.enableRTR != 0);
        const bool wantSSR = (ENABLE_SSR != 0) && (pc.enableSSR != 0);

        if (roughness <= 0.4f && (wantSSR || wantRTR)) {
            float confidence       = 0.0f;
            float3 reflectionColor = float3(0.0f, 0.0f, 0.0f);

#ifndef DISABLE_RTR
            if (wantRTR && roughness < 0.04f) {
                float3 rtrFallback = float3(0.0f, 0.0f, 0.0f);
                float2 hitUV       = RaytraceRTR(worldPos, N, V, roughness, T, B, strength, confidence, rtrFallback, uint2(input.pos.xy));

                if (confidence > 0.0f) {
                    reflectionColor = rtrFallback;
                    if (hitUV.x >= 0.0f) {
                        float4 hitNormRough = texNormalRoughness.SampleLevel(pointSampler, hitUV, 0);
                        if (hitNormRough.z != 0.0f) {
                            float3 screenColor = texLighting.SampleLevel(pointSampler, hitUV, 0).rgb;
                            reflectionColor = lerp(rtrFallback, screenColor, confidence);
                        }
                    }
                }
            } else if (wantRTR) {
                float4 halfRtr = texRtrHalf.SampleLevel(smp, input.uv, 0);
                confidence     = saturate(halfRtr.a);
                if (confidence > 1e-4f) {
                    reflectionColor = halfRtr.rgb;
                }
            } else
#endif
                if (wantSSR) {
                float debugValue      = 0.0f;
                float3 biasedStartPos = worldPos + N * 0.05f;
                float2 hitUV =
                    RaymarchSSR(worldPos, biasedStartPos, R, N, confidence, debugValue, texDepth, pointSampler, pc.viewProj, pc.camPos, pc.invViewProj);

                if (confidence > 0.0f) {
                    reflectionColor = texLighting.SampleLevel(smp, hitUV, 0).rgb;
                }
            }

            float3 specReflect = (lerp(baseSpecularIBL, reflectionColor * FssEss, confidence) * sheenAtt + sheenIBL) *
                                 coatAtt + coatIBL;

            float horizonOcclusion = saturate(1.0f + dot(R, N));
            horizonOcclusion *= horizonOcclusion;

            litColor += specReflect * horizonOcclusion;
        } else {
            litColor += (baseSpecularIBL * sheenAtt + sheenIBL) * coatAtt + coatIBL;
        }
    }

    if (frame.fullBright == 0) {
        float zSlice = log(max(viewDepth, frame.nearZ) / frame.nearZ) / log(frame.farZ / frame.nearZ);
        zSlice -= 0.5f / 64.0f;

        float ignFrame  = float(WrapFrameIndex(frame.frameIndex));
        float2 noisePos = input.pos.xy;
        noisePos.x += ignFrame * 5.588238f;
        noisePos.y += ignFrame * 5.588238f;
        float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
        float dither = frac(magic.z * frac(dot(noisePos, magic.xy))) - 0.5f;

        zSlice += (dither * 1.5f) / 64.0f;

        float4 volumetrics = texVoxelIntegrated.SampleLevel(smp, float3(input.uv, zSlice), 0);
        litColor           = litColor * volumetrics.a + volumetrics.rgb;
    }

    return float4(litColor, 1.0f);
}
