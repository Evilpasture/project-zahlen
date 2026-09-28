
import instance_data;
import pbr_helpers;
import uniforms;

float3 SafeNormalizeLocal(float3 v, float3 fallback) {
    float len = length(v);
    if (len < 1e-5f || len != len || len > 1e10f)
        return fallback;
    float3 r = v / len;
    if (r.x != r.x || r.y != r.y || r.z != r.z)
        return fallback;
    return r;
}
float3 SafeNormalizeLocal(float3 v) {
    float len = length(v);
    if (len < 1e-5f || len != len || len > 1e10f)
        return float3(0.0f, 0.0f, 1.0f);
    float3 r = v / len;
    if (r.x != r.x || r.y != r.y || r.z != r.z)
        return float3(0.0f, 0.0f, 1.0f);
    return r;
}
#define SafeNormalize SafeNormalizeLocal

struct RtrHalfPushConstants {
    uint2 halfRes;
    uint2 _pad;
};
[vk::push_constant]
RtrHalfPushConstants pc;

Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
Texture2D<float4> texAnisotropy;
Texture2D<float4> texLighting;
SamplerState smp;
SamplerState pointSampler;
ConstantBuffer<FrameUniforms> frame;
StructuredBuffer<InstanceData> g_instances;

Texture2D<float4> blueNoiseTex;
SamplerState blueNoiseSampler;

RWTexture2D<float4> outImage;
RaytracingAccelerationStructure tlas;

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (any(tid.xy >= pc.halfRes)) {
        return;
    }
    outImage[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);

    uint2 fullPos = tid.xy * 2u;
    float depth   = texDepth.Load(int3(fullPos, 0)).r;
    if (depth >= 1.0f) {
        return;
    }

    float4 normRough = texNormalRoughness.Load(int3(fullPos, 0));
    float  roughness = normRough.z;
    if (roughness < 0.04f || roughness > 0.4f) {
        return;
    }

    float3 N        = UnpackNormalOctahedron(normRough.xy * 2.0f - 1.0f);
    float2 uv       = (float2(fullPos) + 0.5f) / float2(frame.screenResolution);
    float3 worldPos = ReconstructWorldPos(uv, depth, frame.invViewProj);
    float3 V        = SafeNormalize(frame.camPos.xyz - worldPos, N);
    float  NdotV    = dot(N, V);
    if (NdotV <= 1e-4f) {
        return;
    }

    float4 anisoRaw = texAnisotropy.Load(int3(fullPos, 0));
    float strength = anisoRaw.z;
    float3 up        = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
    float3 tangentX  = strength > 1.0f / 255.0f ? UnpackAnisotropy(anisoRaw.xy, N) : normalize(cross(up, N));
    float3 tangentY  = cross(N, tangentX);
    float3x3 toLocal = float3x3(tangentX, tangentY, N);
    float3x3 toWorld = transpose(toLocal);

    float3 V_local = mul(toLocal, V);
    float2 u       = SampleBlueNoise(blueNoiseTex, blueNoiseSampler, fullPos, FrameIndexFromCamPosW(frame.camPos.w)).zw;
    float2 alpha   = strength > 1.0f / 255.0f ? AnisotropicAlphas(roughness, strength) : float2(max(roughness * roughness, 1e-5f));
    float3 H_local = SampleGGX_VNDF(V_local, alpha, u);
    float3 H       = normalize(mul(toWorld, H_local));

    float3 R = reflect(-V, H);
    if (dot(R, N) <= 0.0f) {
        R = reflect(-V, N);
    }

    RayDesc ray;
    ray.Origin    = worldPos + N * 0.02f;
    ray.Direction = R;
    ray.TMin      = 0.01f;
    ray.TMax      = lerp(50.0f, 16.0f, roughness);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        return;
    }

    float  rayT         = q.CommittedRayT();
    float3 hitWorldPos  = ray.Origin + ray.Direction * rayT;
    float  confidence   = 0.0f;
    float3 fallbackColor = float3(0.0f, 0.0f, 0.0f);
    float2 hitUV        = float2(-1.0f, -1.0f);

    bool fastHit = false;
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
                hitUV         = fastUV;
                fastHit       = true;
            }
        }
    }

    if (!fastHit) {

        uint instanceID = q.CommittedInstanceID();
        InstanceData hitInst = g_instances[instanceID];

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

        if (dot(hitNormal, ray.Direction) > 0.0f) {
            hitNormal = -hitNormal;
        }

        float3 hitBaseColor = hitInst.baseColorFactor.rgb;
        float3 ambientLight = EvaluateSH(hitNormal, frame.sh) * frame.ambientExposure;

        fallbackColor = hitBaseColor * ambientLight + hitInst.emissiveFactor.rgb;
        confidence    = 0.0f;

        float4 hitClip = mul(frame.viewProj, float4(hitWorldPos, 1.0f));
        if (hitClip.w >= 0.1f) {
            float2 hitNDC = hitClip.xy / hitClip.w;
            hitUV         = hitNDC * 0.5f + 0.5f;

            float2 edgeFactor = smoothstep(0.0f, 0.05f, hitUV) * smoothstep(1.0f, 0.95f, hitUV);
            float onScreen    = edgeFactor.x * edgeFactor.y;

            bool gated = false;
            if (onScreen > 0.0f) {
                float sampledRawDepth = texDepth.SampleLevel(pointSampler, hitUV, 0);

                float distToHit = hitClip.w;
                float4 linClip        = mul(frame.invProj, float4(0.0f, 0.0f, sampledRawDepth, 1.0f));
                float distToSampled   = abs(linClip.z / linClip.w);
                float depthDiff       = abs(distToHit - distToSampled);

                float4 hitNormRough   = texNormalRoughness.SampleLevel(pointSampler, hitUV, 0);
                float3 sampledNorm    = UnpackNormalOctahedron(hitNormRough.xy * 2.0f - 1.0f);
                float normalAlignment = saturate(dot(hitNormal, sampledNorm));

                float maxDiff   = max(0.5f, distToHit * 0.05f);
                float depthMask = smoothstep(maxDiff * 1.5f, maxDiff * 0.2f, depthDiff) * smoothstep(0.4f, 0.8f, normalAlignment);

                if (depthMask > 0.1f) {
                    confidence = saturate(onScreen * depthMask);
                    gated      = true;
                }
            }
            if (!gated) {
                hitUV = float2(-1.0f, -1.0f);
            }
        }
    }

    float3 reflectionColor = float3(0.0f, 0.0f, 0.0f);
    if (hitUV.x >= 0.0f && confidence > 0.0f) {
        float4 hitNormRough = texNormalRoughness.SampleLevel(pointSampler, hitUV, 0);
        if (hitNormRough.z != 0.0f) {
            float3 screenColor = texLighting.SampleLevel(pointSampler, hitUV, 0).rgb;
            reflectionColor = lerp(screenColor * 0.5f, screenColor, confidence);
        }
    }

    outImage[tid.xy] = float4(reflectionColor, confidence);
}
