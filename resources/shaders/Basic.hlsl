cbuffer Constants : register(b1) {
    matrix transform;
};

struct VertexIn {
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct VertexOut {
    float4 position : SV_Position;
    float4 color    : COLOR;
};

VertexOut vsMain(VertexIn input) {
    VertexOut output;
    output.position = mul(transform, float4(input.position, 1.0));
    output.color = input.color;
    return output;
}

float4 fsMain(VertexOut input) : SV_Target {
    return input.color;
}