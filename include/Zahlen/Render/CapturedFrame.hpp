// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the renderer hands back when a frame is captured: pixels in memory, in a
// stated format, plus the statistics a test or a tool would otherwise have to
// compute by parsing a file it just asked to be written.
//
// This is deliberately the end of the renderer's responsibility. A capture is a
// GPU readback -- a copy from an image into host-visible memory -- and that is
// all it is. Encoding those pixels into a file format and putting the bytes on a
// disk are the caller's decisions: a test asserts on `pixels` and `metrics`
// directly, a tool encodes and writes, an editor thumbnails. None of that needs
// the renderer to know what a path is.
//
// Every capture path yields tightly packed rows: `pixels.size()` is exactly
// `extent.width * extent.height * BytesPerPixel()`, with no row padding to
// stride over.

#include <Zahlen/Math/Geometry2D.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ZHLN {

enum class PixelFormat : uint8_t {
    Rgb8,
    Rgba8,
    Rgba16F,
};

/// Per-frame statistics, computed during the readback copy.
///
/// They exist so the common assertions -- "is anything lit", "is the sky
/// transparent" -- do not need a second pass over the pixels, and so a failing
/// capture says something about itself in the log.
struct FrameCaptureMetrics {
    /// Mean perceived brightness over all pixels, 0..255 (Rec. 709 weights).
    double meanLuma = 0.0;
    /// Pixels whose luma is above the near-black floor: the ones a black-frame
    /// regression would remove.
    uint64_t litPixels = 0;
    /// Pixels with alpha 0, for captures that preserve the alpha channel.
    uint64_t transparentPixels = 0;
};

// The exposure the renderer's HDR capture path applies before the film curve
// (ZHLN::Math::AcesFilm). Named once so the renderer and anything that
// re-encodes a raw `Rgba16F` frame cannot drift apart.
inline constexpr float kCaptureExposure = 0.015F;

struct CapturedFrame {
    Extent2D             extent {};
    PixelFormat          format = PixelFormat::Rgba8;
    std::vector<uint8_t> pixels;
    FrameCaptureMetrics  metrics {};

    /// Bytes that make up one pixel in this frame's format.
    [[nodiscard]] constexpr auto BytesPerPixel() const noexcept -> size_t {
        switch (format) {
            case PixelFormat::Rgb8:
                return 3;
            case PixelFormat::Rgba8:
                return 4;
            case PixelFormat::Rgba16F:
                return 8; // four half-floats
        }
        return 4;
    }

    /// Color channels per pixel, which is not the same as bytes for a wide format.
    [[nodiscard]] constexpr auto ChannelCount() const noexcept -> size_t {
        switch (format) {
            case PixelFormat::Rgb8:
                return 3;
            case PixelFormat::Rgba8:
                return 4;
            case PixelFormat::Rgba16F:
                return 4;
        }
        return 4;
    }

    [[nodiscard]] auto PixelCount() const noexcept -> size_t {
        return static_cast<size_t>(extent.width) * static_cast<size_t>(extent.height);
    }

    /// True when the buffer is exactly the extent's worth of tightly packed
    /// rows in the stated format. A reader that trusts this cannot overrun.
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return extent.width > 0 && extent.height > 0 && pixels.size() == PixelCount() * BytesPerPixel();
    }
    explicit operator bool() const noexcept {
        return Valid();
    }
};

} // namespace ZHLN
