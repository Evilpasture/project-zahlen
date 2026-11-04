// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The SRTM and RCAS routines below are ports of AMD FidelityFX FSR 1.0
// (ffx-fsr/ffx_fsr1.h, v1.20210629), which is distributed under the MIT
// license. That portion retains AMD's copyright and is reproduced here under
// the terms of the MIT license:
//
//   Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the "Software"),
//   to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense,
//   and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//   The above copyright notice and this permission notice shall be included in
//   all copies or substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
//   THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
//
// -----------------------------------------------------------------------------
// WHY THIS MODULE EXISTS
// -----------------------------------------------------------------------------
// SMAA's thresholds and area/search tables are calibrated for *display-encoded*
// (gamma / sRGB-ish, {0..1}) color, while this renderer runs SMAA directly on
// RGBA16F HDR scene color. Feeding raw HDR radiance into a fixed 0.1 luma
// threshold makes bright pixels saturate the detector and makes neighborhood
// blending smear HDR bleed and clamp highlights.
//
// This module supplies one shared, exactly invertible perceptual encoding so
// that both stages operate in the domain SMAA expects and the AA result is
// written back as unclamped HDR:
//
//   1. SRTM (AMD's Simple Reversible Tone-Mapper): c -> c/(1 + max3(c)).
//      Maps {0..FP16_MAX} into {0..1} *without* clamping, and because the
//      divisor is a scalar it preserves the RGB ratio (chroma) and HDR bleed.
//   2. Gamma 2.0: c -> sqrt(c). AMD uses gamma 2.0 for exactly this purpose;
//      unlike an sRGB curve it is inverted by a single multiply (c*c), so the
//      round trip is cheap and exact in fp32.
//
// Decode applies the inverse gamma first, then SRTM-inverse. The max3() in the
// decoder is taken from the *decoded* (blended) sample, which is what makes a
// convex combination of encoded samples map back to a finite HDR value instead
// of being clipped.
//
// RCAS (Robust Contrast Adaptive Sharpening) also requires {0..1} display-
// encoded input, so folding it into the final blit uses the same encode/decode
// pair. No extra render target and no separate pass are needed.

module aa_common;

// AMD FSR1: "This is set at the limit of providing unnatural results for
// sharpening." FSR_RCAS_LIMIT = 0.25 - 1/16 = 0.1875.
public static const float kAaRcasLimit = 0.25f - (1.0f / 16.0f);

// RCAS sharpness is NOT a constant here: blit.slang passes
// BlitPushConstants.rcasSharpness (sourced from GISettings::rcasSharpness) to
// AaRcasSharpenHdr() every frame. AMD's FsrRcasCon(stops) = exp2(-stops), so
// the shipped default 0.8705505632961241f is 0.2 stops below maximum, and 0.0
// disables the filter. Keeping the dial in settings (rather than baking one
// "correct" value into the shader) leaves the sharpening / crunchiness trade-off
// to the person looking at the frame.

// SMAA's official gamma-space luma weights. Kept here (rather than Rec.709)
// so SMAA_THRESHOLD 0.1 stays at its calibrated value.
public static const float3 kAaPerceptualLumaWeights = float3(0.299f, 0.587f, 0.114f);

// AMD FSR1 FsrSrtmF + gamma 2.0. Exactly invertible via AaPerceptualDecode.
[ForceInline]
public float3 AaPerceptualEncode(float3 hdrRgb) {
    float3 c   = max(hdrRgb, float3(0.0f));
    float  peak = max(c.r, max(c.g, c.b));
    c = c / (1.0f + peak);       // SRTM: {0..FP16_MAX} -> {0..1}, RGB ratio preserved
    return sqrt(c);              // gamma 2.0 perceptual transfer
}

// AMD FSR1 FsrSrtmInvF preceded by the inverse gamma. The 1/32768 floor is
// AMD's own FP16-safe guard for the c = 1.0 (division by zero) case.
[ForceInline]
public float3 AaPerceptualDecode(float3 perceptualRgb) {
    float3 c    = saturate(perceptualRgb);
    c           = c * c;                       // inverse gamma 2.0
    float  peak = max(c.r, max(c.g, c.b));
    return c / max(1.0f / 32768.0f, 1.0f - peak);
}

[ForceInline]
public float AaPerceptualLuma(float3 perceptualRgb) {
    return dot(perceptualRgb, kAaPerceptualLumaWeights);
}

// -----------------------------------------------------------------------------
// AMD FSR1 RCAS, non-packed 32-bit path (FsrRcasF / FSR_RCAS_F).
//
// Minimal 3x3 cross neighborhood:
//
//        b
//      d e f
//        h
//
// The negative-lobe unsharp result is limited by the min/max of the 4-tap ring
// so it cannot ring past the local extrema. A per-channel limiter derived from
// the peak range (hitMin/hitMax) additionally prevents clipping at 0 and 1.
// `nz` is AMD's optional noise detector; FSR_RCAS_DENOISE is left off (the
// guide recommends applying grain after sharpening instead), but it is
// computed so this port is bit-for-bit the reference filter.
//
// AMD's APrxMedRcpF1/ARcpF1 fast reciprocal approximations are replaced with
// plain fp32 division plus a signed-safe epsilon. This is more accurate than
// the half-precision approximation and has no effect on the limiter's sign.
// -----------------------------------------------------------------------------
[ForceInline]
public float AaLumaTimes2(float3 c) {
    return c.b * 0.5f + (c.r * 0.5f + c.g);
}

[ForceInline]
public float3 AaSignedRcpNeg3(float3 x) {
    return float3(1.0f) / min(x, float3(-1e-5f)); // denominator is <= 0 by construction
}

public float3 AaRcasFilter(float3 b, float3 d, float3 e, float3 f, float3 h, float sharpness) {
#if defined(AA_RCAS_DENOISE)
    // AMD's optional FSR_RCAS_DENOISE noise detector. The integration guide
    // recommends applying film grain *after* RCAS instead, so this is off by
    // default; the branch is kept so the port stays a line-for-line match.
    float bL = AaLumaTimes2(b);
    float dL = AaLumaTimes2(d);
    float eL = AaLumaTimes2(e);
    float fL = AaLumaTimes2(f);
    float hL = AaLumaTimes2(h);
    float nz = 0.25f * bL + 0.25f * dL + 0.25f * fL + 0.25f * hL - eL;
    float hi = max(max(bL, dL), max(eL, max(fL, hL)));
    float lo = min(min(bL, dL), min(eL, min(fL, hL)));
    nz       = saturate(abs(nz) / max(hi - lo, 1e-5f));
    nz       = -0.5f * nz + 1.0f;
#endif

    // Min/max of the 4-tap ring.
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));

    // Limiters, high precision RCPs. peakC = (1, -4) in AMD's notation:
    //   hitMin = min(mn4,e) / (4*mx4)
    //   hitMax = (1 - max(mx4,e)) / (4*mn4 - 4)
    float3 hitMin = min(mn4, e) / max(4.0f * mx4, float3(1e-5f));
    float3 hitMax = (float3(1.0f) - max(mx4, e)) * AaSignedRcpNeg3(4.0f * mn4 - 4.0f);

    float3 lobe3 = max(-hitMin, hitMax);
    float  lobe  = max(-kAaRcasLimit,
                       min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0f)) * sharpness;

#if defined(AA_RCAS_DENOISE) // AMD's FSR_RCAS_DENOISE: sharpen less on noise.
    lobe *= nz;
#endif

    // Resolve: 4 neighbors at `lobe`, center at 1, normalized by 4*lobe+1.
    // lobe >= -0.1875 keeps the divisor >= 0.25, so this is always finite.
    float rcpL = 1.0f / (4.0f * lobe + 1.0f);
    return (lobe * (b + d + h + f) + e) * rcpL;
}

// -----------------------------------------------------------------------------
// Full 3x3 RCAS gather for a 1:1 pass. Kept in this library module (rather
// than in blit.slang) so a standalone Slangpy / Lavapipe harness can import
// and exercise exactly the same code the present pass calls, without pulling
// in BlitPushConstants and the tonemap pipeline.
//
//         b
//     d   e   f          b/d/e/f/h are integer Load()s at exact texel centers,
//         h              clamped to the texture so borders are well-defined.
//
// The five taps are encoded to {0..1} perceptual color, sharpened, and the
// result is decoded back to unclamped HDR. Where RCAS's lobe resolves to zero
// the five-tap result is exactly encode->decode of the center pixel.
// -----------------------------------------------------------------------------
public float4 AaRcasSharpenHdr(float2 uv, Texture2D<float4> tex, float sharpness) {
    uint texWidth, texHeight;
    tex.GetDimensions(texWidth, texHeight);
    int2 last = int2(texWidth, texHeight) - int2(1, 1);
    int2 sp   = clamp(int2(uv * float2(texWidth, texHeight)), int2(0, 0), last);

    float3 b  = AaPerceptualEncode(tex.Load(int3(clamp(sp + int2( 0, -1), int2(0, 0), last), 0)).rgb);
    float3 d  = AaPerceptualEncode(tex.Load(int3(clamp(sp + int2(-1,  0), int2(0, 0), last), 0)).rgb);
    float4 e4 = tex.Load(int3(sp, 0));
    float3 e  = AaPerceptualEncode(e4.rgb);
    float3 f  = AaPerceptualEncode(tex.Load(int3(clamp(sp + int2( 1,  0), int2(0, 0), last), 0)).rgb);
    float3 h  = AaPerceptualEncode(tex.Load(int3(clamp(sp + int2( 0,  1), int2(0, 0), last), 0)).rgb);

    // Alpha is pass-through: return it with the sharpened RGB so the caller
    // needs no extra sample of the input texture.
    return float4(AaPerceptualDecode(AaRcasFilter(b, d, e, f, h, sharpness)), e4.a);
}
