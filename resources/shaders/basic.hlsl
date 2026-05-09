struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color    : COLOR;
};

struct VSOutput {
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
};

struct PushConstants {
    float4x4 transform;
};

// Push constants are explicitly defined for SPIR-V output
[[vk::push_constant]]
PushConstants pc;

VSOutput VSMain(VSInput input) {
    VSOutput output;
    // Note: HLSL is column-major by default in DXC, matching your engine math
    output.pos = mul(pc.transform, float4(input.position, 1.0f));
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    return input.color;
}