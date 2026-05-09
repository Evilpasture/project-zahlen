[[vk::binding(0, 0)]] Texture2D sceneColor;
[[vk::binding(1, 0)]] SamplerState defaultSampler;

struct VSInput { uint vID : SV_VertexID; };
struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

PSInput VSMain(VSInput input) {
    PSInput output;
    output.uv = float2((input.vID << 1) & 2, input.vID & 2);
    output.pos = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    output.uv.y = 1.0 - output.uv.y; // Flip Y for Vulkan
    return output;
}

float4 PSMain(PSInput input) : SV_Target {
    return sceneColor.Sample(defaultSampler, input.uv);
}