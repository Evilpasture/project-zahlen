// resources/shaders/bloom_down_cs.slang
//
// Dual Kawase downsample, compute port. Source-texel offsets ride in the push
// constants (rcpWidth/rcpHeight = 1 / SOURCE resolution); the destination UV
// is derived from the dispatch thread id, so one shader serves every cascade
// level without recompilation.

struct KawasePush {
    int   mode; // 0 = downsample (kept for parity with the raster-era layout)
    float rcpWidth;
    float rcpHeight;
    float padding;
};

[vk::push_constant]
KawasePush pc;

Texture2D<float4> texInput;
SamplerState smp;
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

    // --- DUAL KAWASE DOWNSAMPLE ---
    // Bilinear average of 4 samples offset by half a source texel to capture
    // 16 pixels of detail
    float2 texelSize = float2(pc.rcpWidth, pc.rcpHeight);
    float2 halfPixel = texelSize * 0.5f;
    float3 sum       = 0.0f;
    sum += texInput.SampleLevel(smp, uv + float2(-halfPixel.x, -halfPixel.y), 0.0f).rgb;
    sum += texInput.SampleLevel(smp, uv + float2(halfPixel.x, -halfPixel.y), 0.0f).rgb;
    sum += texInput.SampleLevel(smp, uv + float2(-halfPixel.x, halfPixel.y), 0.0f).rgb;
    sum += texInput.SampleLevel(smp, uv + float2(halfPixel.x, halfPixel.y), 0.0f).rgb;

    outImage[tid.xy] = float4(sum * 0.25f, 1.0f);
}
