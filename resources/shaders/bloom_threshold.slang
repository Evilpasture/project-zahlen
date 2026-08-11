// resources/shaders/bloom_threshold.slang

public
struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Raster Vertex Stage
[shader("vertex")] public VSOutput VSMain(uint vertexID : SV_VertexID) {
    VSOutput output;
    output.uv  = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

// Explicit bindings matching BloomThresholdLayout exactly
[[vk::binding(0, 0)]] public
Texture2D<float4> texInput;
[[vk::binding(1, 0)]] public
SamplerState smp;

static const float3 kLumaBT709 = float3(0.2126, 0.7152, 0.0722);

// Raster Pixel Stage
[shader("fragment")] public float4 PSMain(VSOutput input): SV_Target0 {
    float3 color = texInput.SampleLevel(smp, input.uv, 0).rgb;

    // 1. Relative luminance (ITU-R BT.709)
    float luma = dot(color, kLumaBT709);

    // 2. Soft-Knee Thresholding (Matching original constants)
    static const float threshold = 1.0;
    static const float knee      = 0.5;

    float soft = clamp(luma - threshold + knee, 0.0, 2.0 * knee);
    soft       = (soft * soft) / (4.0 * knee + 1e-5);

    float  contribution = max(soft, luma - threshold) / max(luma, 1e-5);
    float3 brightColor  = color * contribution;

    // 3. Highlight Energy Limiter
    static const float maxLuma = 128.0;
    if (luma > maxLuma) {
        brightColor *= (maxLuma / luma);
    }

    return float4(brightColor, 1.0);
}
