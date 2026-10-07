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
// Deliberately dependency-free: no engine headers, no reflection, no device.
// Callers own the pixels.
//
// stb_image_write is header-only, so exactly one TU per test binary must
// define ZHLN_TEST_IMAGE_WRITE_IMPL before including this header; that TU owns
// the implementation and every other TU links against it. Defining it twice in
// one binary is a duplicate-symbol link error. (This is stb_image_WRITE; the
// decode half, STB_IMAGE_IMPLEMENTATION, already lives in src/engine/stbi_impl.c
// inside zahlen_engine and must not be defined in a test TU.)

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(ZHLN_TEST_IMAGE_WRITE_IMPL)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
// Vendored stb uses sprintf in its HDR writer; the macOS SDK marks it
// deprecated. Scope the deprecation noise to the vendored header.
#if defined(_MSC_VER)
    #pragma warning(push)
    #pragma warning(disable : 4996)
#else
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <stb_image_write.h>
#if defined(_MSC_VER)
    #pragma warning(pop)
#else
    #pragma GCC diagnostic pop
#endif

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

// The .png sibling of a .ppm path, so a capture can be dumped in both formats
// without the caller spelling the substitution twice.
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

// `--convert-ppm FILE...`: convert already-captured PPM frames to PNG without
// re-running a suite, so the diagnostics from a failing run can be attached to
// a report.
//
// Returns false when argv is not that invocation, so a group runner can fall
// through to running its suites. Two suites carried this verbatim; it is a
// property of the capture format, not of either suite.
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

// A window in normalized frame coordinates, so the same measurement survives a
// resolution change.
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

// Per-channel classification over a window.
//
// The 45 floor and the 1.35 channel ratio are what make "dominant" mean a
// visible hue rather than a rounding artefact; 0.60 for the mixes keeps yellow
// from counting amber. Note that these are absolute 8-bit thresholds: they are
// insensitive to modest exposure changes but not to a drastic one, so prefer
// expressing a gate as a SHARE of stats.pixels rather than an absolute count.
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

// Share of the window whose hue is dominated by `channel`, with the level
// floor expressed relative to the window's own brightest pixel.
//
// MeasureSubRegion's dominant* counters gate on an absolute 8-bit floor of
// 45, which is the right call for a lit scene but reports a flat zero for a
// subject that is correct and unambiguous but dim: an unlit green emitter
// measuring meanRGB (0.1, 32.7, 0.1) -- a green-to-red ratio of nearly 300 --
// scores 0.00 green, because no pixel reaches 45. Use this when the question
// is "what colour is this subject" rather than "is this subject bright".
//
// `levelFraction` of maxLuma keeps unwritten background out of the count
// (their channel ratios are noise), and `ratio` is the same 1.35 separation
// MeasureSubRegion uses.
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

// `minRowFraction` skips the top of the frame, for scenes where the upper rows
// are sky and would otherwise dominate the counts.
//
// "lit" is luma-based (Luma > 40). A pure blue pixel can never reach that
// (0.0722 * 255 = 18.4), so in a scene lit by saturated primaries this metric
// grades the palette rather than the lighting -- prefer the per-channel counts
// or MeasureSubRegion's dominant/mix shares when hue is the point.
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

// Bounding box and average colour of the pixels that differ by more than
// `threshold`, for localising an instability instead of averaging it away.
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

// Dumps the changed-pixel bounding box as .ppm + .png, so an instability can
// be looked at rather than inferred from a count.
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

// Writes |a - b| scaled 4x, so sub-visible drift becomes inspectable.
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

// ============================================================================
// Temporal Noise Analysis
// ============================================================================
//
// Folded in from tests/render/NoiseFrameCapture.hpp, which carried a second
// RgbImage/LoadPPM pair of its own -- the drift helpers/ImageTesting.hpp was
// written to end, left half-finished. The two stacks named the same bytes
// differently, so a frame read by one suite could not be handed to the
// metrics of the other without a copy. There is one RgbImage now.
//
// These operate on luma PLANES (row-major double, width*height) rather than
// on RGB, because every one of them is about how a scalar field behaves over
// time -- magnitude, isotropy, periodicity -- and none of that survives being
// averaged across channels first.

// Signed luma difference, row-major, same dimensions as the inputs.
[[nodiscard]] inline std::vector<double> LumaDifference(const RgbImage& a, const RgbImage& b) {
    const std::size_t n = static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height);
    std::vector<double> diff(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t p = i * 3u;
        diff[i] = Luma(b.rgb[p], b.rgb[p + 1u], b.rgb[p + 2u]) - Luma(a.rgb[p], a.rgb[p + 1u], a.rgb[p + 2u]);
    }
    return diff;
}

// Half-open rectangle: x in [x0, x1), y in [y0, y1). Default-constructed is
// empty, which is what "nothing changed" looks like.
struct BBox {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;

    [[nodiscard]] bool Empty() const noexcept { return x1 <= x0 || y1 <= y0; }
    [[nodiscard]] int  Width() const noexcept { return std::max(0, x1 - x0); }
    [[nodiscard]] int  Height() const noexcept { return std::max(0, y1 - y0); }
};

// Bounding box of every pixel whose |difference| exceeds `threshold`, grown by
// `margin` and clamped to the frame.
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

// Copies `b` out of a full-frame, row-major field into a tightly packed field
// of its own, so the metric loops see no padding.
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

// RMS of `field` restricted to `b`. This is the penumbra's own energy; the
// whole-frame RMS that MeasureResidual reports is diluted by the unchanged
// majority and is only useful as a gross sanity number.
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


// ---------------------------------------------------------------------------
// Temporal noise magnitude.
//
// The structural metrics above say what SHAPE the noise has. This says whether
// there is the right AMOUNT of it.
//
// A one-sample-per-pixel stochastic shadow makes each penumbra pixel a
// Bernoulli draw: the ray either sees the sun or it does not, so across frames
// that pixel takes exactly two values, A (shadowed) and B (lit), with
// probability p of landing on B. Therefore
//
//     mean     = A + p*d            where d = B - A
//     variance = p*(1-p)*d^2
//
// and crucially this is exact whatever the tone curve does, because the tone
// curve is applied before the draw -- it moves A and B but cannot create a
// third value. So measuring mean and variance per pixel and comparing against
// p*(1-p)*d^2 tests the estimator directly:
//
//     ratio ~= 1     the noise is exactly what 1 SPP must produce
//     ratio ~= 0     the dither is not reaching the pixel at all
//     ratio  > 1     more variance than Bernoulli allows -- instability,
//                    fireflies, or a second noise source on top
//
// Validated on synthetic shadows with a known sample count: the fit returns
// 1.0000 at 1 SPP both linear and tone-mapped, which is the case that matters
// because lighting.slang calls CalculateShadowRayTraced without a `samples`
// argument and so uses the 1u default. Above 1 SPP the model still holds
// against the true coverage (measured variance / theory = 0.97, 1.05, 1.03 at
// N = 1, 2, 4) but the *fitted* coverage drifts, so the ratio reads 0.55 and
// 0.27 instead of 0.50 and 0.25. If the renderer ever goes multi-sample this
// threshold must be recalibrated to 1/N -- that is a deliberate tripwire, not
// a silent pass.
//
// `d` and `A` are estimated from the pixels themselves rather than assumed:
// every penumbra pixel on a single material shares the same two levels, so `d`
// is a high percentile of the per-pixel (max-min) range (a high percentile
// because a pixel whose p is near 0 or 1 may never sample its rare value in a
// finite run, which only ever *underestimates* the range).
// ---------------------------------------------------------------------------

// Running per-pixel temporal statistics over a sequence of frames.
struct TemporalMoments {
    int                   width  = 0;
    int                   height = 0;
    std::vector<double>   sum;
    std::vector<double>   sumSq;
    std::vector<double>   lo;
    std::vector<double>   hi;
    std::vector<uint32_t> count;
    // Times a sample extended the running [lo, hi] by more than
    // `clusterTolerance`. This is a level-count probe, not an outlier count:
    // a pixel that only ever takes two values needs exactly one such event
    // (the first time its second level shows up), a three-valued pixel needs
    // two, and so on. So offCluster summed over a pixel is roughly
    // (distinct levels - 1), and divided by the frame count it should sit
    // near 1/frames for a clean 1 SPP shadow.
    std::vector<uint32_t> offCluster;

    void Reset(int w, int h) {
        width  = w;
        height = h;
        const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
        sum.assign(n, 0.0);
        sumSq.assign(n, 0.0);
        lo.assign(n, 0.0);
        hi.assign(n, 0.0);
        count.assign(n, 0u);
        offCluster.assign(n, 0u);
    }

    // Folds in one frame's luma plane (row-major, width*height).
    void AddLuma(const double* luma, double clusterTolerance) {
        const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        for (std::size_t i = 0; i < n; ++i) {
            const double v = luma[i];
            if (count[i] == 0u) {
                lo[i] = v;
                hi[i] = v;
            } else {
                // Off-cluster is judged against the range seen so far; a value
                // that extends the range is by definition on its new edge.
                if (v < lo[i] - clusterTolerance || v > hi[i] + clusterTolerance) {
                    ++offCluster[i];
                }
                lo[i] = std::min(lo[i], v);
                hi[i] = std::max(hi[i], v);
            }
            sum[i] += v;
            sumSq[i] += v * v;
            ++count[i];
        }
    }
};

// Temporal convergence of a stationary noise field, measured the way a
// denoiser would exploit it: RMS over `b` of (running mean after n samples)
// minus (final mean over N samples). Integrable noise falls like
// sigma*sqrt(1/n - 1/N); a frozen dither reads ~0 at every snapshot; a
// diverging field grows. Deliberately NOT consecutive-frame RMS: under the
// engine's exponential history feedback that metric has a floor of
// feedbackWeight * sigma and plateaus once the history is full, so a fitted
// trend can never account for much of the mean.
[[nodiscard]] inline std::vector<double> RunningMeanResidualSeries(
    const std::vector<std::pair<int, std::vector<double>>>& snapshots, const std::vector<double>& finalMean, int width, const BBox& b
) {
    std::vector<double> out;
    out.reserve(snapshots.size());
    for (const auto& [n, mean]: snapshots) {
        (void) n;
        double        sum   = 0.0;
        std::size_t   count = 0;
        for (int y = b.y0; y < b.y1; ++y) {
            for (int x = b.x0; x < b.x1; ++x) {
                const double d = mean[static_cast<std::size_t>(y) * width + x] - finalMean[static_cast<std::size_t>(y) * width + x];
                sum += d * d;
                ++count;
            }
        }
        out.push_back(count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0);
    }
    return out;
}

// Luma plane of an RGB8 frame.
[[nodiscard]] inline std::vector<double> LumaPlane(const RgbImage& img) {
    const std::size_t n = static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    std::vector<double> out(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t p = i * 3u;
        out[i] = Luma(img.rgb[p], img.rgb[p + 1u], img.rgb[p + 2u]);
    }
    return out;
}

struct BernoulliFit {
    double shadowedLevel  = 0.0; // A, the shadowed luma level.
    double amplitude      = 0.0; // d = B - A, the full flip amplitude.
    double measuredVarSum = 0.0; // Sum of per-pixel temporal variance.
    double expectedVarSum = 0.0; // Sum of p*(1-p)*d^2 over the same pixels.
    double ratio          = 0.0; // measured / expected; 1.0 means a true 1 SPP.
    double offClusterFrac = 0.0; // Samples on neither level; ~0 means two-valued.
    double coverageMin    = 1.0; // Min fitted coverage p over used pixels.
    double coverageMax    = 0.0; // Max fitted coverage p over used pixels.
    int    pixelsUsed     = 0;   // Pixels with pLo < p < pHi.
    int    pixelsInRegion = 0;
    bool   valid          = false;
};

// Not in a `detail` namespace, because configure/check_namespace_governance.py
// refuses a plain one in a header that carries no template code and grants no
// allowlist entry for it -- and this header's only translation unit of its own
// is ImageWriteImpl.cpp, which exists to own the stb implementation and is not
// compiled by every consumer. So the percentile helper sits in the open: it is
// a test helper, and a caller who misuses it has only this comment to answer to.
[[nodiscard]] inline double Percentile(std::vector<double> v, double q) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1) + 0.5);
    return v[std::min(idx, v.size() - 1)];
}

// Fits the two-level Bernoulli model over `b`, using only pixels whose implied
// coverage sits strictly inside (pLo, pHi). Near the ends a finite run may
// never sample the rare value, which corrupts both the range estimate and the
// variance, so those pixels are excluded rather than trusted.
[[nodiscard]] inline BernoulliFit
FitBernoulliNoise(const TemporalMoments& m, const BBox& b, double clusterTolerance, double pLo = 0.10, double pHi = 0.90) {
    BernoulliFit out;
    if (b.Empty() || m.width <= 0 || m.count.empty()) {
        return out;
    }

    // One row per usable pixel, in scan order.
    struct Sample {
        double mean;
        double var;
        double lo;
        double hi;
    };
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(b.Width()) * static_cast<std::size_t>(b.Height()));

    double sampleTotal   = 0.0;
    double offClusterAll = 0.0;
    for (int y = 0; y < b.Height(); ++y) {
        for (int x = 0; x < b.Width(); ++x) {
            const std::size_t i = static_cast<std::size_t>(b.y0 + y) * static_cast<std::size_t>(m.width) + static_cast<std::size_t>(b.x0 + x);
            sampleTotal += static_cast<double>(m.count[i]);
            offClusterAll += static_cast<double>(m.offCluster[i]);
            if (m.count[i] < 2u) {
                continue;
            }
            const double n    = static_cast<double>(m.count[i]);
            const double mean = m.sum[i] / n;
            ++out.pixelsInRegion;
            samples.push_back({mean, std::max(0.0, m.sumSq[i] / n - mean * mean), m.lo[i], m.hi[i]});
        }
    }
    out.offClusterFrac = sampleTotal > 0.0 ? offClusterAll / sampleTotal : 0.0;
    if (samples.empty()) {
        return out;
    }

    std::vector<double> lows, ranges;
    lows.reserve(samples.size());
    ranges.reserve(samples.size());
    for (const Sample& s: samples) {
        lows.push_back(s.lo);
        ranges.push_back(s.hi - s.lo);
    }
    out.shadowedLevel = Percentile(lows, 0.05);
    // 90th percentile of the per-pixel range: high enough to ignore pixels that
    // never sampled their rare value (which only ever underestimates the
    // range), low enough not to be dragged by a lone firefly.
    out.amplitude = Percentile(ranges, 0.90);
    if (out.amplitude <= clusterTolerance) {
        return out; // nothing is actually flipping
    }

    const double d2 = out.amplitude * out.amplitude;
    // Coverage span is measured over EVERY pixel in the region (clamped), not
    // just the fitted window: it is the test that the shadow actually develops
    // from lit to shadowed. A sun disk larger than the occluder leaves p stuck
    // near the lit end and this span collapses -- exactly the failure mode the
    // captured speckle frame showed.
    for (const Sample& s: samples) {
        const double p = std::min(1.0, std::max(0.0, (s.mean - out.shadowedLevel) / out.amplitude));
        out.coverageMin = std::min(out.coverageMin, p);
        out.coverageMax = std::max(out.coverageMax, p);
    }
    for (const Sample& s: samples) {
        const double p = (s.mean - out.shadowedLevel) / out.amplitude;
        if (p <= pLo || p >= pHi) {
            continue;
        }
        ++out.pixelsUsed;
        out.measuredVarSum += s.var;
        out.expectedVarSum += p * (1.0 - p) * d2;
    }
    out.valid = out.pixelsUsed > 0 && out.expectedVarSum > 0.0;
    out.ratio = out.valid ? out.measuredVarSum / out.expectedVarSum : 0.0;
    return out;
}

// ============================================================================
// Invariant Gates
// ============================================================================
//
// Absolute pixel counts ("gold pixels > 100") encode today's look, so every
// fidelity improvement fails them. The gates below are the replacement shapes
// (see tests/INVARIANT_TESTING.md):
//
// - noise-relative: an effect must exceed the run's own measured floor
//   (MeasureNoiseFloor, DescribeSeries, ExceedsNoise),
// - differential: A vs B captured in the same process, compared pooled
//   (CompareFrames, DownsampleBox) rather than counted,
// - share/ratio hue: DominantHueShare and YellowShare, whose floors are
//   relative to the window's own brightest pixel.
//
// Presence floors (e.g. share > 0.01) are fine: they pin existence, not look.

struct SeriesStats {
    double mean   = 0.0;
    double stddev = 0.0;
    double min    = 0.0;
    double max    = 0.0;
    size_t count  = 0;
};

// Mean/stddev/min/max of a metric series, e.g. per-frame mean luma over a
// warmup window. The stddev is what noise-relative gates scale against.
[[nodiscard]] inline SeriesStats DescribeSeries(const std::vector<double>& values) {
    SeriesStats stats;
    stats.count = values.size();
    if (values.empty()) {
        return stats;
    }
    double sum = 0.0;
    double mn  = values[0];
    double mx  = values[0];
    for (const double v: values) {
        sum += v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    stats.mean = sum / static_cast<double>(values.size());
    stats.min  = mn;
    stats.max  = mx;
    if (values.size() > 1) {
        double variance = 0.0;
        for (const double v: values) {
            const double d = v - stats.mean;
            variance += d * d;
        }
        stats.stddev = std::sqrt(variance / static_cast<double>(values.size() - 1));
    }
    return stats;
}

// True when `effect` clears k sigma of measured noise floor AND an absolute
// presence floor. The k-sigma term scales with the run's own jitter
// (exposure, device, driver); the floor keeps a bit-exact-zero noise run
// from passing on dust. Prefer this over `effect > CONSTANT`.
[[nodiscard]] inline bool ExceedsNoise(double effect, double noiseStd, double k = 3.0, double floor = 0.0) noexcept {
    return effect > k * noiseStd && effect > floor;
}

[[nodiscard]] inline double RelativeDifference(double a, double b) noexcept {
    const double denom = std::max({std::abs(a), std::abs(b), 1e-9});
    return std::abs(a - b) / denom;
}

// Monotonicity with a slack band, for sweeps that must move one way
// (roughness rows, radius responses, LOD steps) without pinning the values.
[[nodiscard]] inline bool IsMonotonicNonDecreasing(const std::vector<double>& values, double tolerance = 0.0) noexcept {
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i] + tolerance < values[i - 1]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool IsMonotonicNonIncreasing(const std::vector<double>& values, double tolerance = 0.0) noexcept {
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i] > values[i - 1] + tolerance) {
            return false;
        }
    }
    return true;
}

// The engine's own frame-to-frame jitter over `frames` (consecutive captures
// of an unchanged scene): mean/stddev of pairwise meanAbs plus the worst
// pooled over-32 fraction. Gate temporal effects against this instead of a
// constant: `ExceedsNoise(effect.meanAbs, floor.meanAbsStd)` fails a real
// flicker and survives a noisier-or-quieter renderer.
struct NoiseFloor {
    double meanAbsMean = 0.0;
    double meanAbsStd  = 0.0;
    double frac32Max   = 0.0;
    size_t pairs       = 0;
};

[[nodiscard]] inline NoiseFloor MeasureNoiseFloor(const std::vector<RgbImage>& frames) {
    NoiseFloor floor;
    if (frames.size() < 2) {
        return floor;
    }
    std::vector<double> means;
    means.reserve(frames.size() - 1);
    for (size_t i = 1; i < frames.size(); ++i) {
        const FrameDiff diff = CompareFrames(frames[i - 1], frames[i]);
        means.push_back(diff.meanAbs);
        floor.frac32Max = std::max(floor.frac32Max, diff.frac32);
        ++floor.pairs;
    }
    const SeriesStats stats = DescribeSeries(means);
    floor.meanAbsMean       = stats.mean;
    floor.meanAbsStd        = stats.stddev;
    return floor;
}

// Box-downsampled copy, for pooled A/B comparisons that must ignore
// one-pixel TAA edges and denoiser jitter. Compare the outputs with
// CompareFrames and gate on the pooled fractions, not on counts.
[[nodiscard]] inline RgbImage DownsampleBox(const RgbImage& img, int outWidth, int outHeight) {
    RgbImage out;
    if (!img.Valid() || outWidth <= 0 || outHeight <= 0) {
        return out;
    }
    out.width  = outWidth;
    out.height = outHeight;
    out.rgb.resize(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 3u);
    for (int y = 0; y < outHeight; ++y) {
        const int y0 = y * img.height / outHeight;
        const int y1 = (y + 1) * img.height / outHeight;
        for (int x = 0; x < outWidth; ++x) {
            const int x0 = x * img.width / outWidth;
            const int x1 = (x + 1) * img.width / outWidth;
            uint64_t    sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const size_t p = (static_cast<size_t>(sy) * static_cast<size_t>(img.width) + static_cast<size_t>(sx)) * 3u;
                    sumR += img.rgb[p + 0];
                    sumG += img.rgb[p + 1];
                    sumB += img.rgb[p + 2];
                    ++count;
                }
            }
            const size_t d = (static_cast<size_t>(y) * static_cast<size_t>(outWidth) + static_cast<size_t>(x)) * 3u;
            out.rgb[d + 0] = count > 0 ? static_cast<uint8_t>(sumR / count) : 0;
            out.rgb[d + 1] = count > 0 ? static_cast<uint8_t>(sumG / count) : 0;
            out.rgb[d + 2] = count > 0 ? static_cast<uint8_t>(sumB / count) : 0;
        }
    }
    return out;
}

// Crops an image to a normalized rect (clamped to the frame), for gating on
// a predicted region -- where a mirror image must land, what a probe window
// actually contains -- instead of the whole frame. Empty rects and invalid
// images crop to invalid.
[[nodiscard]] inline RgbImage CropImage(const RgbImage& img, NormalizedRect rect) {
    if (!img.Valid()) {
        return {};
    }
    const int x0 = std::clamp(static_cast<int>(rect.x0 * static_cast<double>(img.width)), 0, img.width);
    const int y0 = std::clamp(static_cast<int>(rect.y0 * static_cast<double>(img.height)), 0, img.height);
    const int x1 = std::clamp(static_cast<int>(rect.x1 * static_cast<double>(img.width)), 0, img.width);
    const int y1 = std::clamp(static_cast<int>(rect.y1 * static_cast<double>(img.height)), 0, img.height);
    if (x1 <= x0 || y1 <= y0) {
        return {};
    }
    RgbImage out;
    out.width  = x1 - x0;
    out.height = y1 - y0;
    out.rgb.resize(static_cast<size_t>(out.width * out.height * 3));
    for (int y = y0; y < y1; ++y) {
        const size_t src = (static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x0)) * 3u;
        const size_t dst = (static_cast<size_t>(y - y0) * static_cast<size_t>(out.width)) * 3u;
        std::copy_n(img.rgb.data() + src, static_cast<size_t>(out.width * 3), out.rgb.data() + dst);
    }
    return out;
}

// Share of the window reading warm: pixels above `levelFraction` of the
// window's own max luma whose R and G both exceed `ratio` x B. The sibling
// of DominantHueShare for hues no single channel dominates (gold, sodium
// vapour, candlelight). Like DominantHueShare the floor is relative, so an
// exposure change moves it with the picture instead of failing the test.
[[nodiscard]] inline double YellowShare(
    const RgbImage& img, const NormalizedRect& rect, double ratio = 1.3, double levelFraction = 0.25
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
    uint32_t     warm      = 0;

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

            if (std::min(r, g) >= ratio * b) {
                ++warm;
            }
        }
    }

    return total > 0 ? static_cast<double>(warm) / static_cast<double>(total) : 0.0;
}

} // namespace ZHLN::Test::Image
