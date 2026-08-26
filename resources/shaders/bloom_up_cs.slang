// resources/shaders/bloom_up_cs.slang
//
// Dual Kawase upsample with additive recombination, compute port. texInput is
// the next-lower mip level (sampled with source-texel offsets from the push
// constants); texLow is the same-resolution downsample stage that gets folded
// back in, exactly like the raster variant did.

struct KawasePush {
    int   mode; // 1 = upsample (kept for parity with the raster-era layout)
    float rcpWidth;
    float rcpHeight;
    float padding;
};

[vk::push_constant]
KawasePush pc;

Texture2D<float4> texInput;
SamplerState smp;
Texture2D<float4> texLow;
RWTexture2D<float4> outImage;

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint2 dstSize;
    outImage.GetDimensions(dstSize.x, dstSize.y);
    if (any(tid.xy >= dstSize)) {
        return;
    }

    float2 uv = (float2(tid.xy) + 0.5f) / float2(dstSize);

    // --- DUAL KAWASE UPSAMPLE ---
    // 8 taps (4 corners weighted 1, 4 edges weighted 2)
    float2 texelSize = float2(pc.rcpWidth, pc.rcpHeight);
    float2 halfPixel = texelSize * 0.5f;
    float3 sum       = 0.0f;

    // Corners (Weight 1)
    sum += texInput.SampleLevel(smp, uv + float2(-halfPixel.x * 2.0f, -halfPixel.y * 2.0f), 0.0f).rgb;
    sum += texInput.SampleLevel(smp, uv + float2(halfPixel.x * 2.0f, -halfPixel.y * 2.0f), 0.0f).rgb;
    sum += texInput.SampleLevel(smp, uv + float2(-halfPixel.x * 2.0f, halfPixel.y * 2.0f), 0.0f).rgb;
    sum += texInput.SampleLevel(smp, uv + float2(halfPixel.x * 2.0f, halfPixel.y * 2.0f), 0.0f).rgb;

    // Edges (Weight 2)
    sum += texInput.SampleLevel(smp, uv + float2(-halfPixel.x * 2.0f, 0.0f), 0.0f).rgb * 2.0f;
    sum += texInput.SampleLevel(smp, uv + float2(halfPixel.x * 2.0f, 0.0f), 0.0f).rgb * 2.0f;
    sum += texInput.SampleLevel(smp, uv + float2(0.0f, -halfPixel.y * 2.0f), 0.0f).rgb * 2.0f;
    sum += texInput.SampleLevel(smp, uv + float2(0.0f, halfPixel.y * 2.0f), 0.0f).rgb * 2.0f;

    float3 upsampled = sum / 12.0f;

    // Additive combine with corresponding same-resolution downsample stage
    float3 low = texLow.SampleLevel(smp, uv, 0.0f).rgb;

    outImage[tid.xy] = float4(upsampled + low, 1.0f);
}
