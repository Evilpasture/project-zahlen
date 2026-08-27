// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/NoiseFrameCapture.hpp
//
// PPM readback plus the region-selection helpers the GPU noise suite measures
// through. Kept in its own header, with no engine dependency, so the crop and
// statistics maths can be compiled and exercised on synthetic frames without a
// GPU -- the metrics themselves (tests/RayTracedNoiseMetrics.hpp) are only as
// trustworthy as the pixels handed to them, and this is the code that decides
// which pixels those are.
//
// Why a region at all: the penumbra of a raised occluder is a few dozen pixels
// wide in an otherwise static 640x480 frame. Handing the whole frame to
// MeasureDirectionalEnergy or AutocorrelationSideLobe would average a narrow
// band against ~300k exact zeros, and zeros are direction-neutral and
// correlation-neutral, so the metric would read "perfect" no matter what the
// dither does. Every structural measurement is therefore taken over the
// bounding box of the pixels that actually changed between two frames.

#pragma once

#include "RayTracedNoiseMetrics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace ZHLN::Test::Frame {

/// An 8-bit RGB image, the layout both PPM and CaptureScreenshotPPM use.
struct RgbImage {
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgb;

    [[nodiscard]] bool Valid() const noexcept {
        return width > 0 && height > 0 && rgb.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u;
    }
};

/// Reads a binary (P6) PPM. Returns an invalid image on any failure rather
/// than throwing, so a missing capture shows up as a clean test error.
[[nodiscard]] inline RgbImage LoadPPM(const std::string& path) {
    RgbImage      img;
    std::ifstream ppm(path, std::ios::binary);
    if (!ppm.is_open()) {
        return img;
    }
    std::string header;
    int         maxColor = 0;
    ppm >> header >> img.width >> img.height >> maxColor;
    ppm.get();
    if (header != "P6" || img.width <= 0 || img.height <= 0 || maxColor != 255) {
        return {};
    }
    img.rgb.resize(static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height) * 3u);
    ppm.read(reinterpret_cast<char*>(img.rgb.data()), static_cast<std::streamsize>(img.rgb.size()));
    if (static_cast<std::size_t>(ppm.gcount()) != img.rgb.size()) {
        return {};
    }
    return img;
}

/// Writes a binary P6 PPM. Only used to dump synthetic fixtures from the CPU
/// side; harmless to link into the GPU suite unused.
[[nodiscard]] inline bool SavePPM(const std::string& path, const uint8_t* rgb, int width, int height) {
    std::ofstream ppm(path, std::ios::binary);
    if (!ppm.is_open()) {
        return false;
    }
    ppm << "P6\n" << width << ' ' << height << "\n255\n";
    ppm.write(reinterpret_cast<const char*>(rgb), static_cast<std::streamsize>(width) * static_cast<std::streamsize>(height) * 3);
    return static_cast<bool>(ppm);
}

/// Signed luma difference, row-major, same dimensions as the inputs.
[[nodiscard]] inline std::vector<double> LumaDifference(const RgbImage& a, const RgbImage& b) {
    const std::size_t n = static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height);
    std::vector<double> diff(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t p = i * 3u;
        diff[i] = Noise::Luma(b.rgb[p], b.rgb[p + 1u], b.rgb[p + 2u]) - Noise::Luma(a.rgb[p], a.rgb[p + 1u], a.rgb[p + 2u]);
    }
    return diff;
}

/// Half-open rectangle: x in [x0, x1), y in [y0, y1). Default-constructed is
/// empty, which is what "nothing changed" looks like.
struct BBox {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;

    [[nodiscard]] bool Empty() const noexcept { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] int  Width() const noexcept { return std::max(0, x1 - x0); }
    [[nodiscard]] int  Height() const noexcept { return std::max(0, y1 - y0); }
};

/// Bounding box of every pixel whose |difference| exceeds `threshold`, grown by
/// `margin` and clamped to the frame.
[[nodiscard]] inline BBox BBoxOfChangedPixels(const double* diff, int width, int height, double threshold, int margin = 0) {
    BBox b;
    b.x0 = width;
    b.y0 = height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            if (std::abs(diff[i]) > threshold) {
                b.x0 = std::min(b.x0, x);
                b.x1 = std::max(b.x1, x + 1);
                b.y0 = std::min(b.y0, y);
                b.y1 = std::max(b.y1, y + 1);
            }
        }
    }
    if (b.Empty()) {
        return {};
    }
    b.x0 = std::max(0, b.x0 - margin);
    b.y0 = std::max(0, b.y0 - margin);
    b.x1 = std::min(width, b.x1 + margin);
    b.y1 = std::min(height, b.y1 + margin);
    return b;
}

/// Copies `b` out of a full-frame, row-major field into a tightly packed field
/// of its own, so the metric loops see no padding.
[[nodiscard]] inline std::vector<double> Crop(const double* field, int srcWidth, const BBox& b) {
    std::vector<double> out(static_cast<std::size_t>(b.Width()) * static_cast<std::size_t>(b.Height()), 0.0);
    for (int y = 0; y < b.Height(); ++y) {
        for (int x = 0; x < b.Width(); ++x) {
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(b.Width()) + static_cast<std::size_t>(x)] =
                field[static_cast<std::size_t>(b.y0 + y) * static_cast<std::size_t>(srcWidth) + static_cast<std::size_t>(b.x0 + x)];
        }
    }
    return out;
}

/// RMS of `field` restricted to `b`. This is the penumbra's own energy; the
/// whole-frame RMS that MeasureResidual reports is diluted by the unchanged
/// majority and is only useful as a gross sanity number.
[[nodiscard]] inline double RmsInRegion(const double* field, int srcWidth, const BBox& b) {
    if (b.Empty()) {
        return 0.0;
    }
    double acc = 0.0;
    for (int y = 0; y < b.Height(); ++y) {
        for (int x = 0; x < b.Width(); ++x) {
            const double v = field[static_cast<std::size_t>(b.y0 + y) * static_cast<std::size_t>(srcWidth) + static_cast<std::size_t>(b.x0 + x)];
            acc += v * v;
        }
    }
    return std::sqrt(acc / static_cast<double>(b.Width() * b.Height()));
}

} // namespace ZHLN::Test::Frame
