// resources/shaders/particle_render.hlsl
#pragma pack_matrix(column_major)
#include "uniforms.hlsl"

struct Particle {
    float3 position;
    float  life;
    float3 velocity;
    float  maxLife;
    float4 color;
    float  size;
    float3 _pad;
};

struct ParticleRenderPushConstants {
    uint64_t particleBufferAddr;
};
[[vk::push_constant]] ParticleRenderPushConstants pc;

[[vk::binding(2, 0)]] ConstantBuffer<FrameUniforms> frame;

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
};

// Fast GPU 3D Hash
float Hash31(float3 p) {
    p = frac(p * 0.1031f);
    p += dot(p, p.zyx + 31.32f);
    return frac((p.x + p.y) * p.z);
}

VSOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    VSOutput output;

    uint64_t addr = pc.particleBufferAddr + instanceID * sizeof(Particle);
    Particle p    = vk::RawBufferLoad<Particle>(addr, 4);

    float2 quadUVs[6] = {float2(0, 0), float2(0, 1), float2(1, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
    float2 offsets[6] = {float2(-0.5f, -0.5f), float2(-0.5f, 0.5f), float2(0.5f, -0.5f), float2(0.5f, -0.5f), float2(-0.5f, 0.5f), float2(0.5f, 0.5f)};

    float2 uv     = quadUVs[vertexID];
    float2 offset = offsets[vertexID] * p.size;

    // Use the static instanceID as a stable seed instead of the moving position
    float seed      = float(instanceID);
    float baseRand  = Hash31(float3(seed, 1.0f, 2.0f));
    float spinSpeed = 0.5f + Hash31(float3(seed, 3.0f, 4.0f)) * 1.5f;

    // Calculate the particle's age and use it to drive a smooth rotation
    float age   = max(0.0f, p.maxLife - p.life);
    float angle = baseRand * 6.2831853f + age * spinSpeed;

    float cosA = cos(angle);
    float sinA = sin(angle);

    float2 rotatedOffset = float2(offset.x * cosA - offset.y * sinA, offset.x * sinA + offset.y * cosA);

    float3 camRight = float3(frame.viewProj[0][0], frame.viewProj[1][0], frame.viewProj[2][0]);
    float3 camUp    = float3(frame.viewProj[0][1], frame.viewProj[1][1], frame.viewProj[2][1]);

    float3 worldPos = p.position + camRight * rotatedOffset.x + camUp * rotatedOffset.y;

    output.pos = mul(frame.viewProj, float4(worldPos, 1.0f));
    output.uv  = uv;

    float lifeNormalized = saturate(p.life / max(p.maxLife, 0.001f));
    float alphaFade      = sin(lifeNormalized * 3.14159265f);

    // --- BOOST RGB TO MATCH HDR LIGHTING ---
    float3 hdrSnowColor = p.color.rgb * (frame.ambientExposure * 0.8f);

    output.color = float4(hdrSnowColor, p.color.a * alphaFade);
    return output;
}

float sdSegment(float2 p, float2 a, float2 b) {
    float2 pa = p - a, ba = b - a;
    float  h = clamp(dot(pa, ba) / dot(ba, ba), 0.0f, 1.0f);
    return length(pa - ba * h);
}

float4 PSMain(VSOutput input): SV_Target0 {
    // Map UV coordinates from [0, 1] to [-1, 1]
    float2 p = (input.uv - 0.5f) * 2.0f;

    // Fast circular boundary cull to optimize fill-rate
    float r = length(p);
    if (r > 0.92f) {
        discard;
    }

    // 12-fold polar coordinate folding (mirrors 360-degrees down to a 30-degree wedge [0, pi/6])
    float theta = atan2(p.y, p.x) + 3.14159265f / 6.0f;
    if (theta < 0.0f) {
        theta += 2.0f * 3.14159265f;
    }
    float  sector       = 3.14159265f / 3.0f; // 60 degrees (pi / 3)
    float  theta_folded = abs(fmod(theta, sector) - sector * 0.5f);
    float2 pf           = float2(cos(theta_folded), sin(theta_folded)) * r;

    // Construct Snowflake signed distance fields (SDF)
    float d = 1e9f;

    // 1. Main Stem:
    float d_stem = sdSegment(pf, float2(0.0f, 0.0f), float2(0.85f, 0.0f)) - 0.02f;
    d            = min(d, d_stem);

    // 2. Outer branch:
    float d_outer = sdSegment(pf, float2(0.60f, 0.0f), float2(0.75f, 0.20f)) - 0.0175f;
    d             = min(d, d_outer);

    // 3. Middle branch:
    float d_middle = sdSegment(pf, float2(0.40f, 0.0f), float2(0.55f, 0.25f)) - 0.0175f;
    d              = min(d, d_middle);

    // 4. Middle sub-tips:
    float d_subtip = sdSegment(pf, float2(0.51f, 0.18f), float2(0.43f, 0.22f)) - 0.0125f;
    d              = min(d, d_subtip);

    // 5. Inner branch:
    float d_inner = sdSegment(pf, float2(0.20f, 0.0f), float2(0.30f, 0.15f)) - 0.015f;
    d             = min(d, d_inner);

    // 6. Hexagon outline:
    float d_hex = sdSegment(pf, float2(0.12f, 0.0f), float2(0.104f, 0.06f)) - 0.01f;
    d           = min(d, d_hex);

    // 7. Decorative tips (circles):
    float d_tip1 = length(pf - float2(0.85f, 0.0f)) - 0.03f;
    d            = min(d, d_tip1);

    float d_tip2 = length(pf - float2(0.75f, 0.20f)) - 0.02f;
    d            = min(d, d_tip2);

    // 8. Center accent dot:
    float d_center = length(pf) - 0.04f;
    d              = min(d, d_center);

    // Sub-pixel screen-space derivative anti-aliasing
    float aa = fwidth(d);
    if (aa == 0.0f) {
        aa = 0.015f;
    }
    float alpha = smoothstep(aa, -aa, d);

    if (alpha <= 0.001f) {
        discard;
    }

    return float4(input.color.rgb, input.color.a * alpha);
}
