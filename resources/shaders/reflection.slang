
import gpu_buffer;
import instance_data;
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
TextureCube<float4> texEnvMap;
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
        float2 u       = SampleBlueNoise(blueNoiseTex, blueNoiseSampler, pixelPos, FrameIndexFromCamPosW(frame.camPos.w)).zw;
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
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
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

        float3 hitBaseColor = hitInst.baseColorFactor.rgb;
        float3 L_sun        = SafeNormalize(frame.lightDir.xyz, float3(0.0f, 1.0f, 0.0f));
        float3 ambientLight = EvaluateSH(hitNormal, frame.sh) * frame.ambientExposure;

        fallbackColor = hitBaseColor * ambientLight + hitInst.emissiveFactor.rgb;
        confidence = 0.0f;

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

        if (frame.environmentMode == 1) {
            litColor = texEnvMap.SampleLevel(clampSampler, rayDir, 0).rgb * frame.ambientExposure;
        } else {
            rayDir = RotateVector(rayDir, frame.lightDir.xyz);

            float dy       = rayDir.y;
            float3 skyGrad = (dy >= 0.0f) ? lerp(frame.skyHorizon.rgb, frame.skyZenith.rgb, pow(saturate(dy), 1.2f))
                                          : lerp(frame.skyHorizon.rgb, frame.skyGround.rgb, pow(saturate(-dy), 0.5f));

            litColor = skyGrad * frame.ambientExposure;
        }
    } else {
        float4 litColorRaw = texLighting.SampleLevel(pointSampler, input.uv, 0);
        if (frame.fullBright != 0) {
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
        float4 anisoRaw = texAnisotropy.SampleLevel(pointSampler, input.uv, 0);
        float strength = anisoRaw.z;
        float3 T = strength > 0.0f ? UnpackAnisotropy(anisoRaw.xy, N) : float3(0.0f);
        float3 B = cross(N, T);
        float3 R = AnisotropicReflection(N, V, B, roughness, strength);

        float3 F0      = lerp(float3(0.04f, 0.04f, 0.04f), albedoRaw.rgb, metallic);
        float NdotV    = saturate(dot(N, V));
        float2 envBRDF = brdfLUT.SampleLevel(clampSampler, float2(NdotV, roughness), 0.0f).rg;
        float3 F_rough = FresnelSchlickRoughness(NdotV, F0, roughness);
        float3 FssEss  = F_rough * envBRDF.x + float3(envBRDF.y, envBRDF.y, envBRDF.y);

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
        float4 coatRaw = texClearcoat.SampleLevel(pointSampler, input.uv, 0);
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
                confidence     = halfRtr.a;
                if (confidence > 0.0f) {
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

        float ignFrame  = float(FrameIndexFromCamPosW(frame.camPos.w));
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
