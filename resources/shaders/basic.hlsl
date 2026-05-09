struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 normal   : NORMAL;   // Hardware unpacks the 10_10_10_2 to float4 automatically
    [[vk::location(2)]] float4 tangent  : TANGENT;  // Hardware unpacks 10_10_10_2
    [[vk::location(3)]] float2 uv       : TEXCOORD; // Hardware unpacks Half2 to float2
    [[vk::location(4)]] float4 color    : COLOR;    // Hardware unpacks RGBA8 to float4
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