// resources/shaders/bloom_threshold_cs.slang
//
// Compute port of the bloom bright-pass. Part of the single-dispatch-chain
// Dual Kawase bloom: the whole cascade (threshold -> 3x down -> 3x up) now
// runs as back-to-back compute dispatches inside ONE graph pass instead of
// seven separate raster passes, removing seven vkCmdBeginRendering/vkCmdEnd-
// Rendering cycles and their image layout transitions per frame.

// The push block must stay word-for-word identical to RenderContext::Impl::
// KawasePushConstants {int mode; float rcpWidth; float rcpHeight; float
// padding}, which every dispatch of the chain pushes at offset 0. The
// threshold stage ignores `mode` (it is always the first link).
struct ThresholdPush {
    int mode;
    float rcpWidth;
    float rcpHeight;
    float padding;
};

[vk::push_constant]
ThresholdPush pc;

Texture2D<float4> texInput;
SamplerState smp;
// The G-Buffer emission channel, straight from the deferred pass. See the glow
// section of CSMain: it enters the blur chain on its own, ungated.
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

    // 1. Relative luminance (ITU-R BT.709)
    float luma = dot(color, kLumaBT709);

    // 2. Soft-Knee Thresholding (Matching original constants)
    static const float threshold = 1.0;
    static const float knee      = 0.5;

    float soft = clamp(luma - threshold + knee, 0.0, 2.0 * knee);
    soft       = (soft * soft) / (4.0 * knee + 1e-5);

    float contribution = max(soft, luma - threshold) / max(luma, 1e-5);
    float3 brightColor = color * contribution;

    // 3. Highlight Energy Limiter
    static const float maxLuma = 128.0;
    if (luma > maxLuma) {
        brightColor *= (maxLuma / luma);
    }

    // 4. Glow layer.
    //
    // The bright pass above only sees what is already over the threshold, and
    // since emission stopped being multiplied by incident light (it owns a
    // G-Buffer channel now, see lighting.slang) an emitter tops out at its own
    // emissive factor. glTF clamps that factor to [0,1] unless the asset ships
    // KHR_materials_emissive_strength, so a neon material lands at luma ~0.7 --
    // under the threshold, and the halo that used to come free from emission
    // being scaled by the sun disappeared with it.
    //
    // Babylon.js does not gate its glow on luminance either: GlowLayer renders
    // the emissive channel by itself, blurs it and adds it back at `intensity`,
    // independent of how bright the rest of the frame is. That is what neon
    // looks like, and it is what this adds -- the emission is fed into the same
    // Kawase cascade with no threshold, so the halo scales with the emissive
    // factor instead of switching on at 1.0.
    static const float kGlowIntensity = 1.0;

    float3 glow = texEmissive.SampleLevel(smp, uv, 0).rgb * kGlowIntensity;

    outImage[tid.xy] = float4(brightColor + glow, 1.0);
}
