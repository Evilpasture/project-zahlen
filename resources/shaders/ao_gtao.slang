// resources/shaders/ao_gtao.slang
//
// Half-resolution GTAO (ground-truth ambient occlusion) horizon search,
// split out of lighting.slang's inline ambient term. The inline version ran
// the 4-slice horizon loop inside the full-resolution lighting pass; the
// occlusion signal is low-frequency, so evaluating it here at quarter pixel
// count and depth-weighted upsampling it in lighting buys back the loop's
// ALU + depth-tap cost without reviving the old full-screen HDR round-trip
// (the target is a single R8 channel, not an HDR intermediate).
//
// Covers the AO-only GI modes (giMode 3/4). The pass is only recorded in
// those modes; lighting.slang's sample-AO branch is the sole consumer.
//
// Like rtr_half.slang, each thread evaluates the 2x2 block's top-left
// full-resolution pixel, so the Weyl/rotation jitter and the depth+normal
// inputs are exactly what the inline pass would have read for that pixel.

import hash;
import pbr_helpers;
import uniforms;

struct GtaoPushConstants {
    uint2    halfRes;    // AO target extent (dispatch domain)
    float2   rcpFullRes; // 1 / full resolution, for the center-pixel UV
    float    time;       // noise phase (FrameUniforms.camPos.w)
    float    aoRadius;
    float    aoBias;
    float    aoPower;
    uint     giSamples;
    float4x4 invViewProj; // jittered, matches lighting's world reconstruction
    float4x4 viewProj;    // focal length comes from viewProj[1][1], as inline
};
[vk::push_constant]
GtaoPushConstants pc;

Texture2D<float> texDepth;
Texture2D<float4> texNormalRoughness;
SamplerState pointSampler;
ConstantBuffer<FrameUniforms> frame;
RWTexture2D<float> outAo;

[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (any(tid.xy >= pc.halfRes)) {
        return;
    }

    // Sky / cleared pixels: no occlusion.
    outAo[tid.xy] = 1.0f;

    const uint2 fullPos = tid.xy * 2u;
    const float depth   = texDepth.Load(int3(fullPos, 0)).r;
    if (depth >= 1.0f) {
        return;
    }

    const float4 nr = texNormalRoughness.Load(int3(fullPos, 0));
    const float3 N  = UnpackNormalOctahedron(nr.xy * 2.0f - 1.0f);

    const float2 uv       = (float2(fullPos) + 0.5f) * pc.rcpFullRes;
    const float3 worldPos = ReconstructWorldPos(uv, depth, pc.invViewProj);
    const float  viewDepth = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;

    // --- DEFINE SMOOTH FADE BOUNDARY FROM 40m TO 50m ---
    const float fade = saturate((50.0f - viewDepth) / 10.0f);
    if (fade <= 0.0f) {
        return;
    }

    float2 edgeFactor = smoothstep(0.0f, 0.08f, uv) * smoothstep(1.0f, 0.92f, uv);
    float  screenFade = edgeFactor.x * edgeFactor.y;

    // --- GTAO horizon search (verbatim port of the lighting.slang branch) ---
    float occlusion = 0.0f;

    const float angle        = GetStableWeylNoise(fullPos, pc.time) * 2.0f * 3.14159265f;
    const int   steps        = max(int(pc.giSamples) / 6, 1);
    const float jitterOffset = GetRotationAngle(uv, pc.time);

    uint dw, dh;
    texDepth.GetDimensions(dw, dh);
    const float aspect      = float(dw) / max(float(dh), 1.0f);
    const float focalLength = abs(pc.viewProj[1][1]);
    // FIX: UV radius projection depends strictly on view-space depth (z-depth)
    const float uvRadius = min((pc.aoRadius * focalLength) / max(viewDepth, 0.1f), 0.2f);

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

    const float rawAo = 1.0f - SoftClamp(saturate((occlusion / 4.0f) * pc.aoPower * screenFade), 0.85f);
    outAo[tid.xy]     = lerp(1.0f, rawAo, fade);
}
