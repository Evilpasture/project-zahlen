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

/// Running per-pixel temporal statistics over a sequence of frames.
struct TemporalMoments {
    int                   width  = 0;
    int                   height = 0;
    std::vector<double>   sum;
    std::vector<double>   sumSq;
    std::vector<double>   lo;
    std::vector<double>   hi;
    std::vector<uint32_t> count;
    /// Times a sample extended the running [lo, hi] by more than
    /// `clusterTolerance`. This is a level-count probe, not an outlier count:
    /// a pixel that only ever takes two values needs exactly one such event
    /// (the first time its second level shows up), a three-valued pixel needs
    /// two, and so on. So offCluster summed over a pixel is roughly
    /// (distinct levels - 1), and divided by the frame count it should sit
    /// near 1/frames for a clean 1 SPP shadow.
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

    /// Folds in one frame's luma plane (row-major, width*height).
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

/// Luma plane of an RGB8 frame.
[[nodiscard]] inline std::vector<double> LumaPlane(const RgbImage& img) {
    const std::size_t n = static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    std::vector<double> out(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t p = i * 3u;
        out[i] = Noise::Luma(img.rgb[p], img.rgb[p + 1u], img.rgb[p + 2u]);
    }
    return out;
}

struct BernoulliFit {
    double shadowedLevel  = 0.0; ///< A, the shadowed luma level.
    double amplitude      = 0.0; ///< d = B - A, the full flip amplitude.
    double measuredVarSum = 0.0; ///< Sum of per-pixel temporal variance.
    double expectedVarSum = 0.0; ///< Sum of p*(1-p)*d^2 over the same pixels.
    double ratio          = 0.0; ///< measured / expected; 1.0 means a true 1 SPP.
    double offClusterFrac = 0.0; ///< Samples on neither level; ~0 means two-valued.
    double coverageMin    = 1.0; ///< Min fitted coverage p over used pixels.
    double coverageMax    = 0.0; ///< Max fitted coverage p over used pixels.
    int    pixelsUsed     = 0;   ///< Pixels with pLo < p < pHi.
    int    pixelsInRegion = 0;
    bool   valid          = false;
};

namespace detail {
[[nodiscard]] inline double Percentile(std::vector<double> v, double q) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const auto idx = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1) + 0.5);
    return v[std::min(idx, v.size() - 1)];
}
} // namespace detail

/// Fits the two-level Bernoulli model over `b`, using only pixels whose implied
/// coverage sits strictly inside (pLo, pHi). Near the ends a finite run may
/// never sample the rare value, which corrupts both the range estimate and the
/// variance, so those pixels are excluded rather than trusted.
[[nodiscard]] inline BernoulliFit
FitBernoulliNoise(const TemporalMoments& m, const BBox& b, double clusterTolerance, double pLo = 0.10, double pHi = 0.90) {
    BernoulliFit out;
    if (b.Empty() || m.width <= 0 || m.count.empty()) {
        return out;
    }

    /// One row per usable pixel, in scan order.
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
    out.shadowedLevel = detail::Percentile(lows, 0.05);
    // 90th percentile of the per-pixel range: high enough to ignore pixels that
    // never sampled their rare value (which only ever underestimates the
    // range), low enough not to be dragged by a lone firefly.
    out.amplitude = detail::Percentile(ranges, 0.90);
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

} // namespace ZHLN::Test::Frame
