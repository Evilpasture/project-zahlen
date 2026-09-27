
struct ThresholdPush {
    int mode;
    float rcpWidth;
    float rcpHeight;
    float glowIntensity;
};

[vk::push_constant]
ThresholdPush pc;

Texture2D<float4> texInput;
SamplerState smp;
Texture2D<float4> texEmissive;
RWTexture2D<float4> outImage;

static const float3 kLumaBT709 = float3(0.2126, 0.7152, 0.0722);

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint2 dstSize;
    outImage.GetDimensions(dstSize.x, dstSize.y);
    if (any(tid.xy >= dstSize)) {
        return;
    }

    float2 uv = (float2(tid.xy) + 0.5f) / float2(dstSize);

    float3 color = texInput.SampleLevel(smp, uv, 0).rgb;

    float3 emissive = texEmissive.SampleLevel(smp, uv, 0).rgb;
    float3 surface  = max(color - emissive, 0.0f);

    float luma = dot(surface, kLumaBT709);

    static const float threshold = 1.0;
    static const float knee      = 0.5;

    float soft = clamp(luma - threshold + knee, 0.0, 2.0 * knee);
    soft       = (soft * soft) / (4.0 * knee + 1e-5);

    float contribution = max(soft, luma - threshold) / max(luma, 1e-5);
    float3 brightColor = surface * contribution;

    static const float maxLuma = 128.0;
    if (luma > maxLuma) {
        brightColor *= (maxLuma / luma);
    }

    outImage[tid.xy] = float4(brightColor + emissive * max(pc.glowIntensity, 0.0f), 1.0);
}
