// resources/shaders/blue_noise.slang
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Blue noise sampling helpers over the engine's noise texture
// (resources/shaders/LDR_RGBA_0.png -- Christoph Peters' LDR_RGBA_0, 1024x1024 RGBA8, four
// decorrelated channels).
//
// Deliberately *stateless*: no textures or samplers of its own. It is imported by
// pbr_helpers, which nine shaders import, so a module-level binding here would land in all
// of them and force a matching positional argument into all nine WriteHeap call sites.
// Callers own their bindings and pass them in, as SMAA.slang does for its LUTs.
//
// Why it exists: the ray-traced paths dithered with Interleaved Gradient Noise, a fixed
// diagonal lattice that any spatial filter (A-Trous, bilateral, TAA) smears along its own
// diagonal instead of averaging away -- the streaking on RT shadows. Blue noise pushes the
// error into the high frequencies, where those filters integrate it to a flat grey. Temporal
// scrolling uses the 2D R2 low-discrepancy sequence rather than a per-frame hash, so
// consecutive frames land on well-separated points and a temporal accumulator converges.
module blue_noise;

/// Plastic number based R2 sequence increments.
public static const float2 R2_OFFSET = float2(0.7548776662466927f, 0.5698402909980532f);

/// Decorrelated second pair, separating multi-sample draws within one frame so sample `i` of
/// a pixel does not reuse sample `i-1`'s texel.
public static const float2 R2_SAMPLE_OFFSET = float2(0.1548776662466927f, 0.3569840290998053f);

/// Nominal frame period written into `camPos.w`, and its exact inverse. 1/64 s, not 1/60:
/// the round trip must be *exact*, and 0.015625 is a power of two, so `frame *
/// FRAME_TIME_STEP` is representable with no rounding below 2^24 and multiplying back by 64
/// recovers the integer bit for bit.
public static const float FRAME_TIME_STEP = 0.015625f;
public static const float FRAME_RATE_HZ   = 64.0f;

/// Period the recovered index wraps at. Small, so `frameIndex * R2_OFFSET` below stays in a
/// range where float32 does not quantise the fractional scroll away -- which it would once
/// the counter reaches the millions (a long session, or a pooled engine across a test binary).
public static const uint FRAME_INDEX_PERIOD = 4096u;

/// Recover an integer frame counter from the `camPos.w` uniform slot. FrameUniforms has no
/// frameIndex field (the counter lives on AAState, which no pass binds), but RenderSystem
/// writes `camPos[3] = GetCurrentFrame() * FRAME_TIME_STEP`, so it is already in every block
/// these shaders see -- reading it back avoids widening the GPU ABI and shifting every later
/// offset in the struct.
///
/// The previous encoding used 0.0166f inverted by 60.2409...f; both are inexact in float32,
/// so by frame ~2000 consecutive frames decoded to the *same* index and every dither driven
/// from here froze (two successive captures came out byte-identical). The rounding term keeps
/// the decode robust even if a caller writes a time that is not an exact multiple of the step.
[ForceInline]
public uint FrameIndexFromCamPosW(float camPosW) {
    return uint(max(camPosW, 0.0f) * FRAME_RATE_HZ + 0.5f) & (FRAME_INDEX_PERIOD - 1u);
}

/**
 * Fetch four decorrelated blue noise values for one pixel.
 *
 * `repeatPoint` must be NEAREST / REPEAT: repeat addressing is what lets the UV scroll wrap
 * the tile in hardware (no `& (dim-1)` mask, no power-of-two requirement, no per-pixel wrap
 * arithmetic), and NEAREST matters because filtering a noise texture averages neighbouring
 * texels toward the mean -- precisely the correlation blue noise exists to avoid.
 *
 * Returns .xy and .zw as two independent pairs: sun-disk jitter and glossy VNDF lobe jitter
 * from one fetch.
 */
[ForceInline]
public float4 SampleBlueNoise(Texture2D<float4> noiseTex, SamplerState repeatPoint, uint2 pixelPos, uint frameIndex) {
    uint width = 0u, height = 0u;
    noiseTex.GetDimensions(width, height);
    float2 invExtent = 1.0f / float2(float(max(width, 1u)), float(max(height, 1u)));

    float2 scroll = frac(float2(float(frameIndex), float(frameIndex)) * R2_OFFSET);
    // Texel-centre addressing: +0.5 texel so NEAREST lands inside the intended
    // texel rather than on the boundary between two.
    float2 uv = (float2(pixelPos) + 0.5f) * invExtent + scroll;
    return noiseTex.SampleLevel(repeatPoint, uv, 0.0f);
}

/// Multi-sample variant: each `sampleIndex` within a frame gets its own decorrelated draw at
/// the same pixel.
[ForceInline]
public float4 SampleBlueNoiseAt(Texture2D<float4> noiseTex, SamplerState repeatPoint, uint2 pixelPos, uint frameIndex, uint sampleIndex) {
    uint width = 0u, height = 0u;
    noiseTex.GetDimensions(width, height);
    float2 invExtent = 1.0f / float2(float(max(width, 1u)), float(max(height, 1u)));

    float2 scroll = frac(float2(float(frameIndex), float(frameIndex)) * R2_OFFSET +
                         float2(float(sampleIndex), float(sampleIndex)) * R2_SAMPLE_OFFSET);
    float2 uv     = (float2(pixelPos) + 0.5f) * invExtent + scroll;
    return noiseTex.SampleLevel(repeatPoint, uv, 0.0f);
}

/// Vogel-disk point `i` of `n`, seeded by `rotation` (radians): the distribution used to
/// jitter a shadow ray across the sun. Uniform in area, with the low-discrepancy spiral
/// keeping few-sample estimates (1-2 SPP) far less clumpy than a hashed disk.
[ForceInline]
public float2 VogelDisk(uint i, uint n, float rotation) {
    const float goldenAngle = 2.399963229728653f;
    float r                 = sqrt((float(i) + 0.5f) / float(max(n, 1u)));
    float theta             = float(i) * goldenAngle + rotation;
    return float2(cos(theta), sin(theta)) * r;
}
