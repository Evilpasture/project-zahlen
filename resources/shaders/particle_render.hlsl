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

VSOutput VSMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID) {
    VSOutput output;

    uint64_t addr = pc.particleBufferAddr + instanceID * sizeof(Particle);
    Particle p    = vk::RawBufferLoad<Particle>(addr, 4);

    float2 quadUVs[6] = {float2(0, 0), float2(0, 1), float2(1, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
    float2 offsets[6] = {float2(-0.5f, -0.5f), float2(-0.5f, 0.5f), float2(0.5f, -0.5f), float2(0.5f, -0.5f), float2(-0.5f, 0.5f), float2(0.5f, 0.5f)};

    float2 uv     = quadUVs[vertexID];
    float2 offset = offsets[vertexID] * p.size;

    float3 camRight = float3(frame.viewProj[0][0], frame.viewProj[1][0], frame.viewProj[2][0]);
    float3 camUp    = float3(frame.viewProj[0][1], frame.viewProj[1][1], frame.viewProj[2][1]);

    float3 worldPos = p.position + camRight * offset.x + camUp * offset.y;

    output.pos = mul(frame.viewProj, float4(worldPos, 1.0f));
    output.uv  = uv;

    float lifeNormalized = saturate(p.life / max(p.maxLife, 0.001f));
    float alphaFade      = sin(lifeNormalized * 3.14159265f);

    // --- BOOST RGB TO MATCH HDR LIGHTING ---
    float3 hdrSnowColor = p.color.rgb * (frame.ambientExposure * 0.8f);

    output.color = float4(hdrSnowColor, p.color.a * alphaFade);
    return output;
}

float4 PSMain(VSOutput input): SV_Target0 {
    // Procedural soft radial particle alpha mask
    float2 distFromCenter = input.uv - 0.5f;
    float  r2             = dot(distFromCenter, distFromCenter);
    if (r2 > 0.25f)
        discard;

    float alpha = smoothstep(0.25f, 0.0f, r2);
    return float4(input.color.rgb, input.color.a * alpha);
}
