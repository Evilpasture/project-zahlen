Texture2D<float4> texCurrent  : register(t0);
Texture2D<float4> texHistory  : register(t1);
Texture2D<float2> texVelocity : register(t2);
SamplerState smpLinear        : register(s1);

struct PushConstants {
    float feedback;
};
[[vk::push_constant]] PushConstants pc;

float4 PSMain(float2 uv : TEXCOORD0) : SV_Target {
    float2 velocity = texVelocity.Sample(smpLinear, uv).rg;
    float2 historyUV = uv - velocity;
    float4 current = texCurrent.Sample(smpLinear, uv);
    
    if(any(historyUV < 0.0) || any(historyUV > 1.0)) { return current; } // Reject offscreen

    float4 history = texHistory.Sample(smpLinear, historyUV);

    // 3x3 Neighborhood Clamping (Anti-ghosting)
    float4 m1 = 0, m2 = 0;
    for(int x = -1; x <= 1; ++x) {
        for(int y = -1; y <= 1; ++y) {
            float4 c = texCurrent.Sample(smpLinear, uv, int2(x, y));
            m1 += c; m2 += c * c;
        }
    }
    float4 mean = m1 / 9.0;
    float4 stddev = sqrt(max(0, (m2 / 9.0) - (mean * mean)));
    
    history = clamp(history, mean - stddev, mean + stddev);

    return lerp(current, history, pc.feedback);
}