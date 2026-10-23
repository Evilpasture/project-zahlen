// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Netpbm serialization of a captured frame, in memory, with no I/O.
//
// The renderer stops at "here are the pixels"; this header turns them into the
// bytes of a P6 (PPM) or P7 (PAM) file. Nothing here opens, stats, or names a
// file -- `EncodePPM` returns a `std::vector<uint8_t>` and the caller decides
// whether it becomes a file, a socket write, a hash, or nothing at all.
//
// Format choice is an argument (`preserveAlpha`), not an inference from a
// filename: a caller writing `capture.pam` asks for alpha because it wants the
// alpha channel, not because its path happens to end in four particular letters.
//
// A file format is a plugin, not a core concern: core hands out a CapturedFrame
// and knows nothing about P6 headers, the same way it knows nothing about glTF
// or JSON. If a tool or a test wants a `.ppm`, it links zahlen_netpbm.

#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace ZHLN::Netpbm {

/// Serializes a frame into an in-memory Netpbm byte stream.
///
/// `preserveAlpha` selects PAM (P7, RGBA) over PPM (P6, RGB); it is only
/// meaningful for a frame that carries alpha, and is ignored for `Rgb8`.
/// `Rgba16F` frames are tone-mapped to 8 bits here rather than refused, so the
/// function is total over `PixelFormat` and cannot produce a header followed by
/// no payload.
[[nodiscard]] inline auto EncodePPM(const CapturedFrame& frame, bool preserveAlpha = false) -> std::vector<uint8_t> {
    if (!frame.Valid()) {
        return {};
    }

    const auto width  = frame.extent.width;
    const auto height = frame.extent.height;
    const auto count  = frame.PixelCount();

    // Only a frame that carries alpha in its buffer can preserve one; asking a
    // three-channel frame for PAM would mean inventing the fourth byte.
    const bool pam = preserveAlpha && frame.format != PixelFormat::Rgb8;

    const std::string header = pam ? std::format("P7\nWIDTH {}\nHEIGHT {}\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", width, height) :
                                     std::format("P6\n{} {}\n255\n", width, height);

    std::vector<uint8_t> out;
    out.reserve(header.size() + (count * (pam ? 4 : 3)));
    out.insert(out.end(), header.begin(), header.end());

    // One quantizer for the wide format, so the frame's exposure and film curve
    // are applied once per channel and nowhere else.
    const auto toLdr = [](float linear) noexcept -> uint8_t {
        const float toned = Math::AcesFilm(linear * kCaptureExposure);
        return static_cast<uint8_t>(std::clamp(toned, 0.0F, 1.0F) * 255.0F);
    };

    const auto* const texels = reinterpret_cast<const uint16_t*>(frame.pixels.data());
    for (size_t i = 0; i < count; ++i) {
        switch (frame.format) {
            case PixelFormat::Rgb8: {
                const auto* const texel = frame.pixels.data() + (i * 3);
                out.insert(out.end(), texel, texel + 3);
                break;
            }
            case PixelFormat::Rgba8: {
                const auto* const texel = frame.pixels.data() + (i * 4);
                out.push_back(texel[0]);
                out.push_back(texel[1]);
                out.push_back(texel[2]);
                if (pam) {
                    out.push_back(texel[3]);
                }
                break;
            }
            case PixelFormat::Rgba16F: {
                const auto* const texel = texels + (i * 4);
                out.push_back(toLdr(Math::HalfToFloat(texel[0])));
                out.push_back(toLdr(Math::HalfToFloat(texel[1])));
                out.push_back(toLdr(Math::HalfToFloat(texel[2])));
                if (pam) {
                    // A wide frame has no 8-bit alpha to carry over; PAM it as opaque.
                    out.push_back(255);
                }
                break;
            }
        }
    }
    return out;
}

} // namespace ZHLN::Netpbm
