// resources/shaders/reflection.slang

import instance_data;
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

float3 RotateVector(float3 V, float3 lightDir) {
    float lenB = length(lightDir);
    if (lenB < 0.001f) {
        return V;
    }
    float3 A     = normalize(float3(0.5f, 1.0f, 0.2f));
    float3 B     = lightDir / lenB;
    float3 q_xyz = cross(B, A);
    float q_w    = 1.0f + dot(A, B);
    float q_len  = sqrt(dot(q_xyz, q_xyz) + q_w * q_w);
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
    float4 camPos;
    int giMode;
    float aoRadius;
    float aoBias;
    float aoPower;
    float giIntensity;
    int giSamples;
    int enableSSR_dynamic;
    int enableRTR_dynamic;
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

// Blue noise tile for the VNDF lobe jitter. Declared before the TLAS so the
// NoRT variant still receives both bindings.
Texture2D<float4> blueNoiseTex;
SamplerState blueNoiseSampler;

// Half-resolution composed RTR result for the roughness band (0.04, 0.40],
// produced by the RtrHalfTrace compute pass: rgb = reflection color,
// a = confidence. Declared before the TLAS so the NoRT variant (which drops
// only the trailing AS slot) keeps the same positional table.
Texture2D<float4> texRtrHalf;

#ifndef DISABLE_RTR
RaytracingAccelerationStructure tlas;

float2 RaytraceRTR(float3 worldPos, float3 N, float3 V, float roughness, out float confidence, out float3 fallbackColor, uint2 pixelPos) {
    confidence    = 0.0f;
    fallbackColor = float3(0.0f, 0.0f, 0.0f);

    float NdotV = dot(N, V);
    if (NdotV <= 1e-4f) {
        return float2(-1.0f, -1.0f);
    }

    // Reflect about a *microfacet* normal drawn from the GGX VNDF instead of
    // the geometric normal. Reflecting about N makes every roughness a mirror:
    // one ray per pixel carries the whole lobe, so brushed metal and polished
    // chrome differ only in how far the ray is allowed to travel. Sampling H
    // from the visible-normal distribution makes the ray follow the actual
    // specular lobe -- wide for rough surfaces, collapsing onto the mirror
    // direction as roughness -> 0 (the unstretch step zeroes H.xy when
    // alpha -> 0, so no separate sharp path is needed).
    //
    // Blue noise seeds the draw so the per-pixel residual is high frequency and
    // the roughness-aware filter downstream can integrate it, rather than the
    // diagonal lattice the old dither produced.
    // Roughness tiering, the standard production concession:
    //   roughness <  0.04 -> mirror ray about N (this branch)
    //   0.04 .. 0.40      -> VNDF lobe ray (else branch)
    //   > 0.40            -> never traced (gated in PSMain), IBL only.
    // At roughness 0.04 the VNDF lobe is alpha = 0.0016, a near-delta
    // distribution: the mirror ray is visually identical, but neighboring
    // pixels on a polished surface then emit parallel rays that traverse
    // the same BVH nodes in lockstep instead of fanning out per pixel and
    // stalling the warp on the slowest divergent traversal. The tier also
    // skips the blue-noise fetch, the tangent frame, and the VNDF math.
    float3 R;
    if (roughness < 0.04f) {
        R = reflect(-V, N);
    } else {
        float3 up        = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
        float3 tangentX  = normalize(cross(up, N));
        float3 tangentY  = cross(N, tangentX);
        float3x3 toLocal = float3x3(tangentX, tangentY, N);
        float3x3 toWorld = transpose(toLocal);

        float3 V_local = mul(toLocal, V);
        // .zw is the second decorrelated pair from the same fetch: the
        // shadow path uses .xy for its sun disk, so the two never share a
        // lattice.
        float2 u       = SampleBlueNoise(blueNoiseTex, blueNoiseSampler, pixelPos, FrameIndexFromCamPosW(frame.camPos.w)).zw;
        float  alpha   = max(roughness * roughness, 1e-5f);
        float3 H_local = SampleGGX_VNDF(V_local, alpha, u);
        float3 H       = normalize(mul(toWorld, H_local));

        R = reflect(-V, H);
        // A microfacet normal tilted away from N can reflect below the
        // surface; such a ray would only ever hit the surface it started on.
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

        // Fast path: most in-view hits (mirrors, floors, metal) land on
        // screen where the lighting pass already shaded the surface. Project
        // first and take that shaded color directly; the BDA vertex fetch,
        // geometric-normal reconstruction, and analytic sun+SH lighting below
        // then only run for hits the reprojection rejects, instead of for
        // every hit. Accepted hits resolve to pure screen lighting, without
        // the analytic max-floor the slow path blends under it.
        float4 fastClip = mul(frame.viewProj, float4(hitWorldPos, 1.0f));
        if (fastClip.w > 0.1f) {
            float2 fastUV = fastClip.xy / fastClip.w * 0.5f + 0.5f;
            if (all(fastUV >= 0.02f) && all(fastUV <= 0.98f)) {
                float sampledDepth = texDepth.SampleLevel(smp, fastUV, 0);
                // Linear view depth of the sampled pixel, without the
                // invViewProj -> world -> viewProj round-trip (M * M^-1):
                // the same value for two 4x4s and a divide less. Jitter
                // lives in the projection's column-2 xy entries, so the z/w
                // linearization against the unjittered invProj is exact.
                float4 linClip      = mul(frame.invProj, float4(0.0f, 0.0f, sampledDepth, 1.0f));
                float distToSampled = abs(linClip.z / linClip.w);
                if (abs(fastClip.w - distToSampled) < max(0.5f, fastClip.w * 0.05f)) {
                    confidence    = 1.0f;
                    fallbackColor = texLighting.SampleLevel(smp, fastUV, 0).rgb;
                    return fastUV;
                }
            }
        }

        // HLSL/Slang CommittedInstanceID is the instanceCustomIndex we write
        // as the draw-queue slot. CommittedInstanceIndex is the compacted
        // TLAS slot and must not be used to index g_instances.
        uint instanceID = q.CommittedInstanceID();
        InstanceData hitInst = g_instances[instanceID];

        // 1. Reconstruct exact geometric hit normal from mesh vertices.
        //
        // The hit position itself is already known analytically (origin +
        // t * dir), and translation cancels inside the edge cross product, so
        // only the linear part of the instance matrix ever matters here:
        // transforming the two edges with the 3x3 and crossing them is
        // mathematically identical to transforming all three points with the
        // full 4x4 -- at two 3x3 multiplies instead of three 4x4 ones.
        float3 hitNormal = float3(0.0f, 1.0f, 0.0f);
        if (hitInst.posAddress != 0) {
            uint primIdx = q.CommittedPrimitiveIndex();
            uint i0      = primIdx * 3 + 0;
            uint i1      = primIdx * 3 + 1;
            uint i2      = primIdx * 3 + 2;

            if (hitInst.iboAddress != 0) {
                uint* indices = (uint*) hitInst.iboAddress;
                i0            = indices[i0];
                i1            = indices[i1];
                i2            = indices[i2];
            }

            float* posValues = (float*) hitInst.posAddress;
            float3 p0        = float3(posValues[i0 * 3 + 0], posValues[i0 * 3 + 1], posValues[i0 * 3 + 2]);
            float3 p1        = float3(posValues[i1 * 3 + 0], posValues[i1 * 3 + 1], posValues[i1 * 3 + 2]);
            float3 p2        = float3(posValues[i2 * 3 + 0], posValues[i2 * 3 + 1], posValues[i2 * 3 + 2]);

            float3x3 world3x3 = (float3x3) hitInst.world;
            float3   crossN   = cross(mul(world3x3, p1 - p0), mul(world3x3, p2 - p0));
            float lenSq   = dot(crossN, crossN);
            if (lenSq > 1e-7f) {
                hitNormal = crossN * rsqrt(lenSq);
            }
        }

        // Ensure normal opposes incoming ray
        if (dot(hitNormal, ray.Direction) > 0.0f) {
            hitNormal = -hitNormal;
        }

        // 2. Full PBR Lighting on the Hit Point (Sun Directional + SH Ambient)
        float3 hitBaseColor = hitInst.baseColorFactor.rgb;
        float3 L_sun        = normalize(frame.lightDir.xyz);
        float NdotL         = saturate(dot(hitNormal, L_sun));
        float3 directSun    = frame.lightDir.w * NdotL * (1.0f / 3.14159265f);
        float3 ambientLight = EvaluateSH(hitNormal, frame.sh) * frame.ambientExposure;

        fallbackColor = hitBaseColor * (directSun + ambientLight) + hitInst.emissiveFactor.rgb;
        confidence    = 1.0f;

        // 3. Project to Screen Space for high-res specular/reflection caching
        float4 hitClip = mul(frame.viewProj, float4(hitWorldPos, 1.0f));
        if (hitClip.w < 0.1f) {
            return float2(-1.0f, -1.0f);
        }

        float2 hitNDC = hitClip.xy / hitClip.w;
        float2 hitUV  = hitNDC * 0.5f + 0.5f;

        float2 edgeFactor = smoothstep(0.0f, 0.05f, hitUV) * smoothstep(1.0f, 0.95f, hitUV);
        float onScreen    = edgeFactor.x * edgeFactor.y;

        if (onScreen > 0.0f) {
            float sampledRawDepth = texDepth.SampleLevel(smp, hitUV, 0);

            float distToHit = hitClip.w;
            // Same round-trip removal as the fast path above: linearize the
            // sampled depth instead of reconstructing and reprojecting it.
            float4 linClip        = mul(frame.invProj, float4(0.0f, 0.0f, sampledRawDepth, 1.0f));
            float distToSampled   = abs(linClip.z / linClip.w);
            float depthDiff       = abs(distToHit - distToSampled);

            // 4. Normal-Orientation Check (Stops front-face bleeding onto back/side faces)
            float4 normRough      = texNormalRoughness.SampleLevel(smp, hitUV, 0);
            float3 sampledNorm    = UnpackNormalOctahedron(normRough.xy * 2.0f - 1.0f);
            float normalAlignment = saturate(dot(hitNormal, sampledNorm));

            float maxDiff   = max(0.5f, distToHit * 0.05f);
            float depthMask = smoothstep(maxDiff * 1.5f, maxDiff * 0.2f, depthDiff) * smoothstep(0.4f, 0.8f, normalAlignment);

            if (depthMask > 0.1f) {
                confidence = lerp(0.8f, 1.0f, onScreen * depthMask);
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
        float3 worldPos = ReconstructWorldPos(input.uv, 1.0f, pc.invViewProj);
        float3 rayDir   = normalize(worldPos - pc.camPos.xyz);

        rayDir = RotateVector(rayDir, frame.lightDir.xyz);

        float dy       = rayDir.y;
        float3 skyGrad = (dy >= 0.0f) ? lerp(frame.skyHorizon.rgb, frame.skyZenith.rgb, pow(saturate(dy), 1.2f))
                                      : lerp(frame.skyHorizon.rgb, frame.skyGround.rgb, pow(saturate(-dy), 0.5f));

        litColor = skyGrad * frame.ambientExposure;
    } else {
        float4 litColorRaw = texLighting.SampleLevel(pointSampler, input.uv, 0);
        if (frame.fullBright != 0) {
            return float4(litColorRaw.rgb, 1.0f);
        }

        float4 normRoughRaw = texNormalRoughness.SampleLevel(smp, input.uv, 0);
        float roughness     = normRoughRaw.z;
        float metallic      = normRoughRaw.w;

        float4 albedoRaw = texInput.SampleLevel(smp, input.uv, 0);
        float3 N         = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);

        float3 worldPos = ReconstructWorldPos(input.uv, depth, pc.invViewProj);
        viewDepth       = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;

        float3 V = normalize(pc.camPos.xyz - worldPos);
        float3 R = reflect(-V, N);

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

        float3 prefilteredColor = texEnvMap.SampleLevel(clampSampler, R_corr, roughness * 5.0f).rgb * frame.ambientExposure;
        float3 specularIBL      = prefilteredColor * FssEss;

        float ao = litColorRaw.a;
        litColor = litColorRaw.rgb;

        const bool wantRTR = (ENABLE_RTR != 0) && (pc.enableRTR_dynamic != 0);
        const bool wantSSR = (ENABLE_SSR != 0) && (pc.enableSSR_dynamic != 0);

        if (roughness <= 0.4f && (wantSSR || wantRTR)) {
            float confidence       = 0.0f;
            float3 reflectionColor = float3(0.0f, 0.0f, 0.0f);

#ifndef DISABLE_RTR
            if (wantRTR && roughness < 0.04f) {
                // Mirror tier: warp-coherent rays, traced in-shader.
                float3 rtrFallback = float3(0.0f, 0.0f, 0.0f);
                float2 hitUV       = RaytraceRTR(worldPos, N, V, roughness, confidence, rtrFallback, uint2(input.pos.xy));

                if (confidence > 0.0f) {
                    reflectionColor = rtrFallback;
                    if (hitUV.x >= 0.0f) {
                        float4 hitNormRough = texNormalRoughness.SampleLevel(smp, hitUV, 0);
                        if (hitNormRough.z != 0.0f) {
                            float3 screenColor = texLighting.SampleLevel(smp, hitUV, 0).rgb;
                            // The lighting target now carries emission as its
                            // own additive term (lighting.slang), so screen
                            // colour is trustworthy for emitters. The max()
                            // stays for pixels the analytic fallback resolves
                            // better than a low-confidence screen hit.
                            reflectionColor = max(rtrFallback, lerp(rtrFallback, screenColor, confidence));
                        }
                    }
                }
            } else if (wantRTR) {
                // Roughness band (0.04, 0.40]: the lobe rays diverge per
                // pixel, so they are traced at half resolution by the
                // RtrHalfTrace compute pass (75% fewer rays, traversal out
                // of the fragment quads). Bilinear-upsample the composed
                // result; a = 0 (sky, miss, outside band) leaves
                // reflectionColor at zero and confidence at zero, so the
                // lerp below falls back to specularIBL.
                float4 halfRtr = texRtrHalf.SampleLevel(smp, input.uv, 0);
                confidence    = halfRtr.a;
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

            float3 specReflect = lerp(specularIBL, reflectionColor * FssEss, confidence);

            float roughnessFade    = saturate(1.0f - roughness * 2.0f);
            float horizonOcclusion = saturate(1.0f + dot(R, N));
            horizonOcclusion *= horizonOcclusion;

            litColor += specReflect * roughnessFade * horizonOcclusion;
        } else {
            float3 F_refl       = float3(0.15f, 0.15f, 0.15f);
            float3 F_term       = F_refl + (1.0f - F_refl) * pow(saturate(1.0f - dot(V, N)), 5.0f);
            float roughnessFade = saturate(1.0f - roughness);
            litColor            = lerp(litColor, litColor + specularIBL * F_term * roughnessFade, roughnessFade);
        }
    }

    if (frame.fullBright == 0) {
        float zSlice = log(max(viewDepth, 0.1f) / 0.1f) / log(10000.0f);
        zSlice -= 0.5f / 64.0f;

        float2 noisePos = input.pos.xy;
        noisePos.x += (frame.camPos.w * 60.0f) * 5.588238f;
        noisePos.y += (frame.camPos.w * 60.0f) * 5.588238f;
        float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
        float dither = frac(magic.z * frac(dot(noisePos, magic.xy))) - 0.5f;

        zSlice += (dither * 1.5f) / 64.0f;

        float4 volumetrics = texVoxelIntegrated.SampleLevel(smp, float3(input.uv, zSlice), 0);
        litColor           = litColor * volumetrics.a + volumetrics.rgb;
    }

    return float4(litColor, 1.0f);
}
