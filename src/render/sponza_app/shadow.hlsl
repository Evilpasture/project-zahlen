struct VSInput { [[vk::location(0)]] float3 pos : POSITION; };
struct PushConstants { float4x4 mvp; };
[[vk::push_constant]] PushConstants pc;

float4 VSMain(VSInput input) : SV_Position {
    return mul(pc.mvp, float4(input.pos, 1.0));
}

// Dummy pixel shader because our engine expects 2 stages for graphics pipelines
void PSMain() {}