import gpu_push_constants;
import gtao_surface;
import pbr_helpers;
import uniforms;
import numbers;

[vk::push_constant]
GtaoPushConstants pc;

Texture2D<float> texDepth;
Texture2D<float> texAoDepth;
Texture2D<float4> texNormalRoughness;
SamplerState pointSampler;
ConstantBuffer<FrameUniforms> frame;
RWTexture2D<float> outAo;

// Integral from 0 to h of cos(theta - n) * sin(theta) dtheta.
float IntegrateArc(float h, float n) {
    return 0.25f * (cos(n) + 2.0f * h * sin(n) - cos(2.0f * h - n));
}

float HorizonCos(float2 uv, uint lod, float3 P, float3 V, float3 T, float openCos) {
    if (any(uv < 0.0f) || any(uv >= 1.0f))
        return openCos;

    float depth = 1.0f;
    if (lod > 0u) {
        depth = texAoDepth.SampleLevel(pointSampler, uv, float(lod));
        // Max reduction avoids expanding foreground silhouettes. Sky in a
        // coarse footprint must not erase an otherwise valid fine sample.
    }
    if (depth >= 1.0f) {
        depth = texDepth.SampleLevel(pointSampler, uv, 0.0f);
        uv = (floor(uv / pc.rcpFullRes) + 0.5f) * pc.rcpFullRes;
    }
    if (depth >= 1.0f)
        return openCos;

    float3 H = ReconstructWorldPos(uv, depth, pc.invViewProj) - P;
    float distanceToTap = length(H);
    float towardEye = dot(H, V);
    float alongSlice = dot(H, T);
    if (distanceToTap < 1e-4f || alongSlice <= 1e-4f)
        return openCos;

    float rangeWeight = 1.0f - smoothstep(0.5f, 1.0f, distanceToTap / pc.aoRadius);
    float candidate = towardEye * rsqrt(towardEye * towardEye + alongSlice * alongSlice);
    return lerp(openCos, max(openCos, candidate), rangeWeight);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= pc.halfRes))
        return;

    outAo[tid.xy] = 1.0f;
    if (pc.aoRadius <= 0.0f)
        return;

    uint2 fullSize;
    texDepth.GetDimensions(fullSize.x, fullSize.y);
    uint2 pixel;
    float depth;
    if (!GtaoSurface(texDepth, tid.xy, fullSize, pixel, depth))
        return;

    const float2 uv = (float2(pixel) + 0.5f) * pc.rcpFullRes;
    const float3 P = ReconstructWorldPos(uv, depth, pc.invViewProj);
    const float3 N = UnpackNormalOctahedron(texNormalRoughness.Load(int3(pixel, 0)).xy * 2.0f - 1.0f);
    const float3 V = SafeNormalize(frame.camPos.xyz - P, N);
    const float viewDepth = mul(frame.unjitteredViewProj, float4(P, 1.0f)).w;
    const float fade = saturate((50.0f - viewDepth) / 10.0f);
    if (fade <= 0.0f)
        return;

    uint mipWidth, mipHeight, levels;
    texAoDepth.GetDimensions(0u, mipWidth, mipHeight, levels);
    // Defaults: 6 slices * 2 directions * 4 steps = 48 horizon taps.
    // Higher quality caps at 8 * 2 * 6 = 96, independent of resolution.
    const int quality = int(min(pc.giSamples, 24u));
    const int slices = clamp(6 + (quality - 8) / 8, 6, 8);
    const int steps = clamp(4 + (quality - 8) / 8, 4, 6);
    const float2 noise = frac(0.5f + float(tid.x) * float2(0.754877666f, 0.569840296f)
                                 + float(tid.y) * float2(0.569840296f, -0.754877666f));
    const float focalY = 1.0f / max(abs(frame.invProj[1][1]), 0.1f);
    const float radiusY = min(0.5f * pc.aoRadius * focalY / max(viewDepth, 0.1f), 0.2f);
    const float radiusPx = radiusY * float(fullSize.y);
    float visible = 0.0f;
    float unoccluded = 0.0f;

    for (int slice = 0; slice < slices; ++slice) {
        float angle = (float(slice) + noise.x) * Numbers::Pi / float(slices);
        float2 axis = float2(cos(angle), sin(angle));
        float2 pixelStep = axis * pc.rcpFullRes;
        float3 D = ReconstructWorldPos(uv + pixelStep, depth, pc.invViewProj) - P;
        float3 sliceTangent = D - V * dot(D, V);
        float tangentLenSq = dot(sliceTangent, sliceTangent);
        // A collapsed reconstruction does not define a slice plane.
        if (!(tangentLenSq > 1e-20f) || !isfinite(tangentLenSq))
            continue;
        float3 T = sliceTangent * rsqrt(tangentLenSq);
        float2 projectedN = float2(dot(N, V), dot(N, T));
        float projectedLength = length(projectedN);
        if (projectedLength < 1e-4f)
            continue;
        float n = atan2(projectedN.y, projectedN.x);

        [unroll]
        for (int side = 0; side < 2; ++side) {
            float signSide = side == 0 ? 1.0f : -1.0f;
            float normalAngle = n * signSide;
            float lo = clamp(normalAngle - Numbers::HalfPi, 0.0f, Numbers::Pi);
            float hi = clamp(normalAngle + Numbers::HalfPi, 0.0f, Numbers::Pi);
            float openCos = cos(hi);
            float horizon = openCos;
            for (int i = 0; i < steps; ++i) {
                float t = (float(i) + frac(noise.y + float(slice) * 0.618033989f + float(side) * 0.5f)) / float(steps);
                float tapDistance = max(1.5f, radiusPx * t * t);
                // Use coarse footprints only for distant taps and cap at 8x8.
                uint lod = min(min(levels - 1u, 3u), uint(max(0.0f, floor(log2(tapDistance)) - 2.0f)));
                horizon = max(horizon, HorizonCos(uv + pixelStep * (signSide * tapDistance), lod, P, V, signSide * T, openCos));
            }
            float lowerIntegral = IntegrateArc(lo, normalAngle);
            float h = clamp(acos(clamp(horizon, -1.0f, 1.0f)), lo, hi);
            visible += projectedLength * max(0.0f, IntegrateArc(h, normalAngle) - lowerIntegral);
            unoccluded += projectedLength * max(0.0f, IntegrateArc(hi, normalAngle) - lowerIntegral);
        }
    }

    float occlusion = unoccluded > 1e-4f ? saturate(1.0f - visible / unoccluded - pc.aoBias) : 0.0f;
    float2 edgeFactor = smoothstep(0.0f, 0.08f, uv) * (1.0f - smoothstep(0.92f, 1.0f, uv));
    outAo[tid.xy] = 1.0f - fade * SoftClamp(saturate(occlusion * pc.aoPower * edgeFactor.x * edgeFactor.y), 0.85f);
}
