// resources/shaders/bloom_blur.slang

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")] VSOutput VSMain(uint vertexID : SV_VertexID) {
    VSOutput output;
    output.uv  = float2((vertexID << 1) & 2, vertexID & 2);
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

struct PushConstants {
    int   mode; // 0 = Downsample, 1 = Upsample
    float rcpWidth;
    float rcpHeight;
    float padding;
};

[vk::push_constant] PushConstants pc;

Texture2D<float4> texInput;
SamplerState      smp;
Texture2D<float4> texLow;

[shader("fragment")] float4 PSMain(VSOutput input): SV_Target0 {
    float2 texelSize = float2(pc.rcpWidth, pc.rcpHeight);

    if (pc.mode == 0) {
        // --- DUAL KAWASE DOWNSAMPLE ---
        // Bilinear average of 4 samples offset by half a texel to capture 16 pixels of detail
        float2 halfPixel = texelSize * 0.5f;
        float3 sum       = 0.0f;
        sum += texInput.SampleLevel(smp, input.uv + float2(-halfPixel.x, -halfPixel.y), 0.0f).rgb;
        sum += texInput.SampleLevel(smp, input.uv + float2(halfPixel.x, -halfPixel.y), 0.0f).rgb;
        sum += texInput.SampleLevel(smp, input.uv + float2(-halfPixel.x, halfPixel.y), 0.0f).rgb;
        sum += texInput.SampleLevel(smp, input.uv + float2(halfPixel.x, halfPixel.y), 0.0f).rgb;
        return float4(sum * 0.25f, 1.0f);
    } else {
        // --- DUAL KAWASE UPSAMPLE ---
        // 8 taps (4 corners weighted 1, 4 edges weighted 2)
        float2 halfPixel = texelSize * 0.5f;
        float3 sum       = 0.0f;

        // Corners (Weight 1)
        sum += texInput.SampleLevel(smp, input.uv + float2(-halfPixel.x * 2.0f, -halfPixel.y * 2.0f), 0.0f).rgb;
        sum += texInput.SampleLevel(smp, input.uv + float2(halfPixel.x * 2.0f, -halfPixel.y * 2.0f), 0.0f).rgb;
        sum += texInput.SampleLevel(smp, input.uv + float2(-halfPixel.x * 2.0f, halfPixel.y * 2.0f), 0.0f).rgb;
        sum += texInput.SampleLevel(smp, input.uv + float2(halfPixel.x * 2.0f, halfPixel.y * 2.0f), 0.0f).rgb;

        // Edges (Weight 2)
        sum += texInput.SampleLevel(smp, input.uv + float2(-halfPixel.x * 2.0f, 0.0f), 0.0f).rgb * 2.0f;
        sum += texInput.SampleLevel(smp, input.uv + float2(halfPixel.x * 2.0f, 0.0f), 0.0f).rgb * 2.0f;
        sum += texInput.SampleLevel(smp, input.uv + float2(0.0f, -halfPixel.y * 2.0f), 0.0f).rgb * 2.0f;
        sum += texInput.SampleLevel(smp, input.uv + float2(0.0f, halfPixel.y * 2.0f), 0.0f).rgb * 2.0f;

        float3 upsampled = sum / 12.0f;

        // Additive combine with corresponding same-resolution downsample stage
        float3 low = texLow.SampleLevel(smp, input.uv, 0.0f).rgb;
        return float4(upsampled + low, 1.0f);
    }
}
