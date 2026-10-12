
struct ThresholdPush {
    int mode;
    float rcpWidth;
    float rcpHeight;
    float glowIntensity;
    // Camera exposure (the same value the final blit multiplies by). The
    // threshold pass reads the raw, pre-exposure HDR scene, so the bloom
    // knee has to be evaluated in exposed units; see CSMain.
    float exposure;
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

    // The scene buffer is raw HDR radiance; exposure is applied much later,
    // in the tonemap blit. Comparing raw luma against a display-referred
    // threshold of 1.0 turned every normally lit surface into a bloom source
    // (a sunlit wall at exposure 0.18 sits around raw luma 5-15). Threshold
    // the *exposed* luma instead, so only surfaces that actually reach
    // display white contribute. The output stays in raw units -- the blit
    // scales it by exposure together with the scene.
    float exposedLuma = luma * max(pc.exposure, 1e-4f);

    static const float threshold = 1.0;
    static const float knee      = 0.5;

    float soft = clamp(exposedLuma - threshold + knee, 0.0, 2.0 * knee);
    soft       = (soft * soft) / (4.0 * knee + 1e-5);

    float contribution = max(soft, exposedLuma - threshold) / max(exposedLuma, 1e-5);
    float3 brightColor = surface * contribution;

    static const float maxLuma = 128.0;
    if (exposedLuma > maxLuma) {
        brightColor *= (maxLuma / exposedLuma);
    }

    outImage[tid.xy] = float4(brightColor + emissive * max(pc.glowIntensity, 0.0f), 1.0);
}
