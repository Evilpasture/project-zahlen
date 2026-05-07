#pragma pack_matrix(column_major)

struct ShadowPush {
    float4x4 mvp; // This is the lightSpaceMatrix passed from C++
};

[[vk::push_constant]] ShadowPush pc;

struct VSInput {
    [[vk::location(0)]] float3 pos : POSITION;
};

float4 VSMain(VSInput input) : SV_POSITION {
    // Transform vertex to light-clip-space
    return mul(pc.mvp, float4(input.pos, 1.0));
}

// Pixel shader is empty; hardware handles depth write automatically
void PSMain() {}