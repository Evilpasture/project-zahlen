// resources/shaders/blue_noise.slang
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Screen-space blue noise sampling on top of the generated 64x64 LUT
// (blue_noise_data.slang).
//
// Why this exists: the ray-traced paths used to dither with Interleaved
// Gradient Noise (GetShadowDither). IGN is a fixed diagonal lattice, so any
// spatial filter -- an A-Trous wavelet, a bilateral blur, TAA -- smears it
// along its own diagonal instead of averaging it away, which is the source of
// the visible streaking on RT shadows. Blue noise pushes the error into the
// high frequencies where those same filters integrate it to a flat grey.
//
// Temporal scrolling uses the 2D R2 low-discrepancy sequence (generalised
// golden ratio) rather than a per-frame hash: consecutive frames land on
// well-separated lattice points, so a temporal accumulator converges instead
// of revisiting the same offsets and locking in a residual pattern.
module blue_noise;

__exported import blue_noise_data;

/// Plastic number based R2 sequence increments.
public static const float2 R2_OFFSET = float2(0.7548776662466927f, 0.5698402909980532f);

/// Decorrelated second pair, used to separate multi-sample draws within one
/// frame so sample `i` of a pixel does not reuse sample `i-1`'s texel.
public static const float2 R2_SAMPLE_OFFSET = float2(0.1548776662466927f, 0.3569840290998053f);

/// Tileable blue noise lookup, scrolled over time.
///
/// `pixelPos` is the integer fragment coordinate (SV_Position.xy); `frameIndex`
/// comes from FrameUniforms. Returns a decorrelated pair in [0, 1].
[ForceInline]
public float2 GetBlueNoise(uint2 pixelPos, uint frameIndex) {
    float2 scroll = frac(float2(float(frameIndex), float(frameIndex)) * R2_OFFSET) * float(BLUE_NOISE_DIM);
    // Integer wrap keeps the tile seamless; the LUT is generated with a
    // periodic distance metric so the seam carries no discontinuity.
    uint2 coord = (pixelPos + uint2(scroll)) & (BLUE_NOISE_DIM - 1u);
    return BlueNoiseFetchTexel(coord.y * BLUE_NOISE_DIM + coord.x);
}

/// Multi-sample variant: each `sampleIndex` within a frame gets its own
/// decorrelated draw at the same pixel.
[ForceInline]
public float2 GetBlueNoiseSample(uint2 pixelPos, uint frameIndex, uint sampleIndex) {
    float2 scroll = frac(float2(float(frameIndex), float(frameIndex)) * R2_OFFSET +
                         float2(float(sampleIndex), float(sampleIndex)) * R2_SAMPLE_OFFSET) *
                    float(BLUE_NOISE_DIM);
    uint2 coord = (pixelPos + uint2(scroll)) & (BLUE_NOISE_DIM - 1u);
    return BlueNoiseFetchTexel(coord.y * BLUE_NOISE_DIM + coord.x);
}

/// Scalar convenience: the first channel of GetBlueNoise.
[ForceInline]
public float GetBlueNoiseScalar(uint2 pixelPos, uint frameIndex) {
    return GetBlueNoise(pixelPos, frameIndex).x;
}

/// Recover an integer frame counter from the `camPos.w` uniform slot.
///
/// FrameUniforms has no frameIndex field (the counter lives on AAState, which
/// no render pass currently binds), but RenderSystem writes
/// `camPos[3] = GetCurrentFrame() * 0.0166f`, so the value is already in every
/// FrameUniforms block this shader can see. Inverting the constant avoids
/// widening the GPU ABI -- which would shift every later offset in the struct
/// and require a matching change in the host mirror -- just to scroll a noise
/// tile. float32 keeps ~7 significant digits: after an hour (~216k frames,
/// camPos.w ~ 3586) the recovered index is still exact to well under one frame,
/// and a fractional error only matters at the 1/64-texel level here anyway.
[ForceInline]
public uint FrameIndexFromCamPosW(float camPosW) {
    return uint(max(camPosW, 0.0f) * 60.240963855421686f);
}

/// Vogel-disk point `i` of `n`, seeded by `rotation` (radians).
///
/// The disk distribution used to jitter a shadow ray across the sun: uniform
/// in area, with the low-discrepancy spiral keeping few-sample estimates
/// (1-2 SPP) far less clumpy than a hashed disk.
[ForceInline]
public float2 VogelDisk(uint i, uint n, float rotation) {
    const float goldenAngle = 2.399963229728653f;
    float r                 = sqrt((float(i) + 0.5f) / float(max(n, 1u)));
    float theta             = float(i) * goldenAngle + rotation;
    return float2(cos(theta), sin(theta)) * r;
}
