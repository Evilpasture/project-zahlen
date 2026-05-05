struct VSInput {
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 norm : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct PSInput {
    float4 pos : SV_Position;
    float3 norm : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PushConstants {
    float4x4 mvp;
};

[[vk::push_constant]] PushConstants pc;

PSInput VSMain(VSInput input) {
    PSInput output;
    output.pos = mul(pc.mvp, float4(input.pos, 1.0));
    output.norm = input.norm; // In a real engine, multiply by transpose(inverse(model))
    output.uv = input.uv;
    return output;
}

[[vk::binding(0, 0)]] Texture2D baseColorTex;
[[vk::binding(1, 0)]] SamplerState baseColorSampler;

float4 PSMain(PSInput input) : SV_Target {
    float4 color = baseColorTex.Sample(baseColorSampler, input.uv);
    
    // Discard transparent pixels (Intel Sponza uses alpha testing for foliage)
    if (color.a < 0.5) discard;

    // Simple fixed directional light (sunlight)
    float3 lightDir = normalize(float3(0.5, 1.0, 0.3));
    
    // Basic N dot L diffuse lighting, clamped with an ambient term of 0.1
    float ndotl = max(dot(normalize(input.norm), lightDir), 0.1);
    
    // Apply a slight warm tint to the sunlight
    float3 lightColor = float3(1.0, 0.95, 0.85) * ndotl;
    
    return float4(color.rgb * lightColor, color.a);
}