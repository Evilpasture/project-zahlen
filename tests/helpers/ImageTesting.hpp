// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/helpers/ImageTesting.hpp
//
// Frame I/O and pixel statistics shared by the GPU test suites.
//
// Every render test that inspects a capture had grown its own copy of this:
// RgbImage/LoadPPM in ten files, Luma in five, and three divergent spellings of
// the same region statistics (SubRegionStats / RegionStats, NormalizedRect /
// NormRect, dominantRed / redDom). The copies had already drifted -- one
// validated the "P6" magic and the read length, another did not -- so a
// truncated capture could pass in one suite and fail in another.
//
// Deliberately dependency-free, like tests/render/NoiseFrameCapture.hpp: no
// engine headers, no reflection, no device. Callers own the pixels.
//
// stb_image_write is header-only, so exactly one TU per test binary must
// define ZHLN_TEST_IMAGE_WRITE_IMPL before including this header; that TU owns
// the implementation and every other TU links against it. Defining it twice in
// one binary is a duplicate-symbol link error. (This is stb_image_WRITE; the
// decode half, STB_IMAGE_IMPLEMENTATION, already lives in extern/stbi_impl.c
// inside zahlen_engine and must not be defined in a test TU.)

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(ZHLN_TEST_IMAGE_WRITE_IMPL)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include <stb_image_write.h>

namespace ZHLN::Test::Image {

// ============================================================================
// Frame Container & I/O
// ============================================================================

struct RgbImage {
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgb;

    [[nodiscard]] bool Valid() const noexcept {
        return width > 0 && height > 0 && rgb.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    }
};

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

    if (header != "P6" || img.width <= 0 || img.height <= 0) {
        return {};
    }

    img.rgb.resize(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 3u);
    ppm.read(reinterpret_cast<char*>(img.rgb.data()), static_cast<std::streamsize>(img.rgb.size()));
    if (ppm.gcount() != static_cast<std::streamsize>(img.rgb.size())) {
        return {};
    }
    return img;
}

[[nodiscard]] inline bool WritePPM(const std::string& path, const RgbImage& img) {
    if (!img.Valid()) {
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }
    out << "P6\n" << img.width << " " << img.height << "\n255\n";
    out.write(reinterpret_cast<const char*>(img.rgb.data()), static_cast<std::streamsize>(img.rgb.size()));
    return out.good();
}

/// The .png sibling of a .ppm path, so a capture can be dumped in both formats
/// without the caller spelling the substitution twice.
[[nodiscard]] inline std::string PngPathOf(std::string_view ppmPath) {
    std::string png(ppmPath);
    if (png.size() >= 4 && (png.ends_with(".ppm") || png.ends_with(".PPM"))) {
        png.resize(png.size() - 4);
    }
    png += ".png";
    return png;
}

[[nodiscard]] inline bool SavePNG(const std::string& path, const RgbImage& img) {
    if (!img.Valid()) {
        return false;
    }
    return stbi_write_png(path.c_str(), img.width, img.height, 3, img.rgb.data(), img.width * 3) != 0;
}

/// `--convert-ppm FILE...`: convert already-captured PPM frames to PNG without
/// re-running a suite, so the diagnostics from a failing run can be attached to
/// a report.
///
/// Returns false when argv is not that invocation, so a group runner can fall
/// through to running its suites. Two suites carried this verbatim; it is a
/// property of the capture format, not of either suite.
[[nodiscard]] inline bool ConvertPpmToPng(int argc, char** argv) {
    if (argc < 3 || std::string_view(argv[1]) != "--convert-ppm") {
        return false;
    }

    bool allOk = true;
    for (int i = 2; i < argc; ++i) {
        const RgbImage img = LoadPPM(argv[i]);
        if (!img.Valid()) {
            std::fprintf(stderr, "Failed to read: %s\n", argv[i]);
            allOk = false;
            continue;
        }
        const std::string png = PngPathOf(argv[i]);
        if (!SavePNG(png, img)) {
            std::fprintf(stderr, "Failed to write: %s\n", png.c_str());
            allOk = false;
            continue;
        }
        std::printf("converted %s -> %s\n", argv[i], png.c_str());
    }
    return allOk;
}

// ============================================================================
// Photometry
// ============================================================================

[[nodiscard]] inline double Luma(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return 0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) + 0.0722 * static_cast<double>(b);
}

[[nodiscard]] inline double Luma(double r, double g, double b) noexcept {
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// ============================================================================
// Region Statistics
// ============================================================================

/// A window in normalized frame coordinates, so the same measurement survives a
/// resolution change.
struct NormalizedRect {
    double x0 = 0.0, y0 = 0.0, x1 = 1.0, y1 = 1.0;
};

struct SubRegionStats {
    uint32_t pixels      = 0;
    double   meanR       = 0.0;
    double   meanG       = 0.0;
    double   meanB       = 0.0;
    double   meanLuma    = 0.0;
    double   maxLuma     = 0.0;
    uint32_t dominantRed = 0;
    uint32_t dominantGrn = 0;
    uint32_t dominantBlu = 0;
    uint32_t yellowMix   = 0;
    uint32_t cyanMix     = 0;
    uint32_t saturated   = 0;
};

/// Per-channel classification over a window.
///
/// The 45 floor and the 1.35 channel ratio are what make "dominant" mean a
/// visible hue rather than a rounding artefact; 0.60 for the mixes keeps yellow
/// from counting amber. Note that these are absolute 8-bit thresholds: they are
/// insensitive to modest exposure changes but not to a drastic one, so prefer
/// expressing a gate as a SHARE of stats.pixels rather than an absolute count.
[[nodiscard]] inline SubRegionStats MeasureSubRegion(const RgbImage& img, const NormalizedRect& rect) {
    SubRegionStats stats;
    if (!img.Valid()) {
        return stats;
    }

    const int x0 = std::clamp(static_cast<int>(rect.x0 * img.width), 0, img.width - 1);
    const int y0 = std::clamp(static_cast<int>(rect.y0 * img.height), 0, img.height - 1);
    const int x1 = std::clamp(static_cast<int>(rect.x1 * img.width), x0 + 1, img.width);
    const int y1 = std::clamp(static_cast<int>(rect.y1 * img.height), y0 + 1, img.height);

    double sumR = 0.0, sumG = 0.0, sumB = 0.0, sumL = 0.0;

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t  i = (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 3u;
            const uint8_t r = img.rgb[i + 0];
            const uint8_t g = img.rgb[i + 1];
            const uint8_t b = img.rgb[i + 2];
            const double  l = Luma(r, g, b);

            sumR += r;
            sumG += g;
            sumB += b;
            sumL += l;
            stats.maxLuma = std::max(stats.maxLuma, l);
            ++stats.pixels;

            if (r >= 45 && r >= 1.35 * g && r >= 1.35 * b) {
                ++stats.dominantRed;
            }
            if (g >= 45 && g >= 1.35 * r && g >= 1.35 * b) {
                ++stats.dominantGrn;
            }
            if (b >= 45 && b >= 1.35 * r && b >= 1.35 * g) {
                ++stats.dominantBlu;
            }
            if (r >= 45 && g >= 45 && b <= 0.60 * std::min(r, g)) {
                ++stats.yellowMix;
            }
            if (g >= 45 && b >= 45 && r <= 0.60 * std::min(g, b)) {
                ++stats.cyanMix;
            }
            if (r >= 250 && g >= 250 && b >= 250) {
                ++stats.saturated;
            }
        }
    }

    if (stats.pixels > 0) {
        const double n = static_cast<double>(stats.pixels);
        stats.meanR    = sumR / n;
        stats.meanG    = sumG / n;
        stats.meanB    = sumB / n;
        stats.meanLuma = sumL / n;
    }

    return stats;
}

enum class HueChannel : uint8_t { Red, Green, Blue };

/// Share of the window whose hue is dominated by `channel`, with the level
/// floor expressed relative to the window's own brightest pixel.
///
/// MeasureSubRegion's dominant* counters gate on an absolute 8-bit floor of
/// 45, which is the right call for a lit scene but reports a flat zero for a
/// subject that is correct and unambiguous but dim: an unlit green emitter
/// measuring meanRGB (0.1, 32.7, 0.1) -- a green-to-red ratio of nearly 300 --
/// scores 0.00 green, because no pixel reaches 45. Use this when the question
/// is "what colour is this subject" rather than "is this subject bright".
///
/// `levelFraction` of maxLuma keeps unwritten background out of the count
/// (their channel ratios are noise), and `ratio` is the same 1.35 separation
/// MeasureSubRegion uses.
[[nodiscard]] inline double DominantHueShare(
    const RgbImage& img, const NormalizedRect& rect, HueChannel channel, double ratio = 1.35, double levelFraction = 0.25
) {
    if (!img.Valid()) {
        return 0.0;
    }

    const int x0 = std::clamp(static_cast<int>(rect.x0 * img.width), 0, img.width - 1);
    const int y0 = std::clamp(static_cast<int>(rect.y0 * img.height), 0, img.height - 1);
    const int x1 = std::clamp(static_cast<int>(rect.x1 * img.width), x0 + 1, img.width);
    const int y1 = std::clamp(static_cast<int>(rect.y1 * img.height), y0 + 1, img.height);

    double maxLuma = 0.0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 3u;
            maxLuma        = std::max(maxLuma, Luma(img.rgb[i + 0], img.rgb[i + 1], img.rgb[i + 2]));
        }
    }
    if (maxLuma <= 0.0) {
        return 0.0;
    }

    const double floorLuma = levelFraction * maxLuma;
    uint32_t     total     = 0;
    uint32_t     dominant  = 0;

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 3u;
            const double r = img.rgb[i + 0];
            const double g = img.rgb[i + 1];
            const double b = img.rgb[i + 2];
            ++total;

            if (Luma(img.rgb[i + 0], img.rgb[i + 1], img.rgb[i + 2]) < floorLuma) {
                continue;
            }

            const double self  = channel == HueChannel::Red ? r : (channel == HueChannel::Green ? g : b);
            const double other = channel == HueChannel::Red ? std::max(g, b) : (channel == HueChannel::Green ? std::max(r, b) : std::max(r, g));
            if (self >= ratio * other) {
                ++dominant;
            }
        }
    }

    return total > 0 ? static_cast<double>(dominant) / static_cast<double>(total) : 0.0;
}

// ============================================================================
// Whole-Frame Statistics
// ============================================================================

struct FrameMetrics {
    uint32_t total       = 0;
    uint32_t lit         = 0;
    uint32_t dark        = 0;
    uint32_t saturated   = 0;
    uint32_t red         = 0;
    uint32_t green       = 0;
    uint32_t blue        = 0;
    uint32_t yellow      = 0;
    uint32_t cyan        = 0;
    uint32_t redPeak     = 0;
    uint32_t redIsolated = 0;
    double   meanLuma    = 0.0;
};

/// `minRowFraction` skips the top of the frame, for scenes where the upper rows
/// are sky and would otherwise dominate the counts.
///
/// "lit" is luma-based (Luma > 40). A pure blue pixel can never reach that
/// (0.0722 * 255 = 18.4), so in a scene lit by saturated primaries this metric
/// grades the palette rather than the lighting -- prefer the per-channel counts
/// or MeasureSubRegion's dominant/mix shares when hue is the point.
[[nodiscard]] inline FrameMetrics MeasureImage(const RgbImage& img, double minRowFraction = 0.0) {
    FrameMetrics m;
    if (!img.Valid()) {
        return m;
    }

    const int minRow = static_cast<int>(std::ceil(minRowFraction * static_cast<double>(img.height)));

    double lumaSum = 0.0;
    for (size_t i = 0; i < img.rgb.size(); i += 3) {
        const size_t pixel = i / 3;
        const int    y     = static_cast<int>(pixel / static_cast<size_t>(img.width));
        if (y < minRow) {
            continue;
        }

        const uint8_t r = img.rgb[i + 0];
        const uint8_t g = img.rgb[i + 1];
        const uint8_t b = img.rgb[i + 2];
        const double  l = Luma(r, g, b);

        ++m.total;
        lumaSum += l;

        if (l > 40.0) {
            ++m.lit;
        }
        if (l < 24.0) {
            ++m.dark;
        }
        if (r >= 250 && g >= 250 && b >= 250) {
            ++m.saturated;
        }
        if (r >= 60 && r >= 1.6 * static_cast<double>(g) && r >= 1.6 * static_cast<double>(b)) {
            ++m.red;
            m.redPeak = std::max(m.redPeak, static_cast<uint32_t>(r));

            const int  x               = static_cast<int>(pixel % static_cast<size_t>(img.width));
            uint32_t   blackNeighbours = 0;
            const auto isBlackAt       = [&](int nx, int ny) -> bool {
                if (nx < 0 || ny < 0 || nx >= img.width || ny >= img.height) {
                    return true;
                }
                const size_t ni = (static_cast<size_t>(ny) * static_cast<size_t>(img.width) + static_cast<size_t>(nx)) * 3u;
                return static_cast<int>(img.rgb[ni + 0]) + static_cast<int>(img.rgb[ni + 1]) + static_cast<int>(img.rgb[ni + 2]) <= 6;
            };
            blackNeighbours += isBlackAt(x - 1, y) ? 1u : 0u;
            blackNeighbours += isBlackAt(x + 1, y) ? 1u : 0u;
            blackNeighbours += isBlackAt(x, y - 1) ? 1u : 0u;
            blackNeighbours += isBlackAt(x, y + 1) ? 1u : 0u;
            if (blackNeighbours >= 3) {
                ++m.redIsolated;
            }
        }
        if (g >= 60 && g >= 1.6 * static_cast<double>(r) && g >= 1.6 * static_cast<double>(b)) {
            ++m.green;
        }
        if (b >= 60 && b >= 1.6 * static_cast<double>(r) && b >= 1.6 * static_cast<double>(g)) {
            ++m.blue;
        }
        if (r >= 60 && g >= 60 && b <= 50) {
            ++m.yellow;
        }
        if (g >= 60 && b >= 60 && r <= 50) {
            ++m.cyan;
        }
    }

    if (m.total > 0) {
        m.meanLuma = lumaSum / static_cast<double>(m.total);
    }
    return m;
}

// ============================================================================
// Temporal Comparison
// ============================================================================

struct FrameDiff {
    uint32_t over12  = 0;
    uint32_t over32  = 0;
    double   meanAbs = 0.0;
    double   frac12  = 0.0;
    double   frac32  = 0.0;
};

[[nodiscard]] inline FrameDiff CompareFrames(const RgbImage& a, const RgbImage& b) {
    FrameDiff d;
    if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height) {
        return d;
    }

    uint64_t sum = 0;
    for (size_t i = 0; i < a.rgb.size(); i += 3) {
        const int dr    = std::abs(static_cast<int>(a.rgb[i + 0]) - static_cast<int>(b.rgb[i + 0]));
        const int dg    = std::abs(static_cast<int>(a.rgb[i + 1]) - static_cast<int>(b.rgb[i + 1]));
        const int db    = std::abs(static_cast<int>(a.rgb[i + 2]) - static_cast<int>(b.rgb[i + 2]));
        const int worst = std::max({dr, dg, db});
        sum += static_cast<uint64_t>(dr + dg + db);
        if (worst > 12) {
            ++d.over12;
        }
        if (worst > 32) {
            ++d.over32;
        }
    }

    const size_t pixels = a.rgb.size() / 3;
    if (pixels > 0) {
        d.meanAbs = static_cast<double>(sum) / (static_cast<double>(pixels) * 3.0);
        d.frac12  = static_cast<double>(d.over12) / static_cast<double>(pixels);
        d.frac32  = static_cast<double>(d.over32) / static_cast<double>(pixels);
    }
    return d;
}

/// Bounding box and average colour of the pixels that differ by more than
/// `threshold`, for localising an instability instead of averaging it away.
struct ChangedRegion {
    uint32_t count     = 0;
    int      minX      = 0;
    int      maxX      = 0;
    int      minY      = 0;
    int      maxY      = 0;
    int      maxDelta  = 0;
    double   meanDelta = 0.0;
    double   aR = 0.0, aG = 0.0, aB = 0.0;
    double   bR = 0.0, bG = 0.0, bB = 0.0;
};

[[nodiscard]] inline ChangedRegion DiffRegion(const RgbImage& a, const RgbImage& b, int threshold = 32) {
    ChangedRegion r;
    if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height) {
        return r;
    }

    r.minX       = a.width;
    r.minY       = a.height;
    r.maxX       = -1;
    r.maxY       = -1;
    uint64_t sum = 0, sumAr = 0, sumAg = 0, sumAb = 0, sumBr = 0, sumBg = 0, sumBb = 0;

    for (size_t i = 0; i < a.rgb.size(); i += 3) {
        const int dr    = std::abs(static_cast<int>(a.rgb[i + 0]) - static_cast<int>(b.rgb[i + 0]));
        const int dg    = std::abs(static_cast<int>(a.rgb[i + 1]) - static_cast<int>(b.rgb[i + 1]));
        const int db    = std::abs(static_cast<int>(a.rgb[i + 2]) - static_cast<int>(b.rgb[i + 2]));
        const int worst = std::max({dr, dg, db});
        r.maxDelta      = std::max(r.maxDelta, worst);
        if (worst > threshold) {
            const size_t pixel = i / 3;
            const int    x     = static_cast<int>(pixel % static_cast<size_t>(a.width));
            const int    y     = static_cast<int>(pixel / static_cast<size_t>(a.width));
            ++r.count;
            r.minX = std::min(r.minX, x);
            r.maxX = std::max(r.maxX, x);
            r.minY = std::min(r.minY, y);
            r.maxY = std::max(r.maxY, y);
            sum += static_cast<uint64_t>(worst);
            sumAr += a.rgb[i + 0];
            sumAg += a.rgb[i + 1];
            sumAb += a.rgb[i + 2];
            sumBr += b.rgb[i + 0];
            sumBg += b.rgb[i + 1];
            sumBb += b.rgb[i + 2];
        }
    }

    if (r.count == 0) {
        r.minX = r.maxX = r.minY = r.maxY = 0;
    } else {
        r.meanDelta = static_cast<double>(sum) / static_cast<double>(r.count);
        r.aR        = static_cast<double>(sumAr) / r.count;
        r.aG        = static_cast<double>(sumAg) / r.count;
        r.aB        = static_cast<double>(sumAb) / r.count;
        r.bR        = static_cast<double>(sumBr) / r.count;
        r.bG        = static_cast<double>(sumBg) / r.count;
        r.bB        = static_cast<double>(sumBb) / r.count;
    }
    return r;
}

/// Dumps the changed-pixel bounding box as .ppm + .png, so an instability can
/// be looked at rather than inferred from a count.
inline void WriteRegionCrop(const std::string& path, const RgbImage& img, const ChangedRegion& region) {
    if (!img.Valid() || region.count == 0) {
        return;
    }
    const int x0 = std::max(0, region.minX);
    const int y0 = std::max(0, region.minY);
    const int x1 = std::min(img.width - 1, region.maxX);
    const int y1 = std::min(img.height - 1, region.maxY);
    if (x1 < x0 || y1 < y0) {
        return;
    }

    const int w = x1 - x0 + 1;
    const int h = y1 - y0 + 1;

    std::vector<uint8_t> crop(static_cast<size_t>(w) * h * 3);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const size_t src = (static_cast<size_t>(y) * img.width + static_cast<size_t>(x)) * 3u;
            const size_t dst = (static_cast<size_t>(y - y0) * w + static_cast<size_t>(x - x0)) * 3u;
            crop[dst + 0]    = img.rgb[src + 0];
            crop[dst + 1]    = img.rgb[src + 1];
            crop[dst + 2]    = img.rgb[src + 2];
        }
    }

    const RgbImage cropped {.width = w, .height = h, .rgb = crop};
    (void) WritePPM(path, cropped);
    (void) SavePNG(PngPathOf(path), cropped);
}

/// Writes |a - b| scaled 4x, so sub-visible drift becomes inspectable.
inline void WriteAmplifiedDiff(const std::string& path, const RgbImage& a, const RgbImage& b) {
    if (!a.Valid() || !b.Valid() || a.width != b.width || a.height != b.height) {
        return;
    }

    std::vector<uint8_t> amplified(a.rgb.size());
    for (size_t i = 0; i < a.rgb.size(); ++i) {
        const int d  = std::abs(static_cast<int>(a.rgb[i]) - static_cast<int>(b.rgb[i]));
        amplified[i] = static_cast<uint8_t>(std::min(255, d * 4));
    }

    const RgbImage diff {.width = a.width, .height = a.height, .rgb = amplified};
    (void) WritePPM(path, diff);
    (void) SavePNG(PngPathOf(path), diff);
}

// ============================================================================
// Series Statistics
// ============================================================================

[[nodiscard]] inline double Mean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (double v: values) {
        sum += v;
    }
    return sum / static_cast<double>(values.size());
}

[[nodiscard]] inline double StdDev(const std::vector<double>& values, double mean) {
    if (values.size() < 2) {
        return 0.0;
    }
    double sumSq = 0.0;
    for (double v: values) {
        const double d = v - mean;
        sumSq += d * d;
    }
    return std::sqrt(sumSq / static_cast<double>(values.size() - 1));
}

[[nodiscard]] inline double CoefficientOfVariation(const std::vector<double>& values) {
    const double mean = Mean(values);
    if (mean <= 1e-9) {
        return 0.0;
    }
    return StdDev(values, mean) / mean;
}

} // namespace ZHLN::Test::Image
