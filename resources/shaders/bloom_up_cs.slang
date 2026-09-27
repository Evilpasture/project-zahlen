
struct KawasePush {
    int   mode;
    float rcpWidth;
    float rcpHeight;
    float glowIntensity;
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

    float2 texelSize = float2(pc.rcpWidth, pc.rcpHeight);
    float2 halfPixel = texelSize * 0.5f;
    float3 sum       = 0.0f;

    sum += texInput.SampleLevel(smp, uv + float2(-halfPixel.x * 2.0f, -halfPixel.y * 2.0f), 0.0f).rgb;
    sum += texInput.SampleLevel(smp, uv + float2(halfPixel.x * 2.0f, -halfPixel.y * 2.0f), 0.0f).rgb;
    sum += texInput.SampleLevel(smp, uv + float2(-halfPixel.x * 2.0f, halfPixel.y * 2.0f), 0.0f).rgb;
    sum += texInput.SampleLevel(smp, uv + float2(halfPixel.x * 2.0f, halfPixel.y * 2.0f), 0.0f).rgb;

    sum += texInput.SampleLevel(smp, uv + float2(-halfPixel.x * 2.0f, 0.0f), 0.0f).rgb * 2.0f;
    sum += texInput.SampleLevel(smp, uv + float2(halfPixel.x * 2.0f, 0.0f), 0.0f).rgb * 2.0f;
    sum += texInput.SampleLevel(smp, uv + float2(0.0f, -halfPixel.y * 2.0f), 0.0f).rgb * 2.0f;
    sum += texInput.SampleLevel(smp, uv + float2(0.0f, halfPixel.y * 2.0f), 0.0f).rgb * 2.0f;

    float3 upsampled = sum / 12.0f;

    float3 low = texLow.SampleLevel(smp, uv, 0.0f).rgb;

    outImage[tid.xy] = float4(upsampled + low, 1.0f);
}
