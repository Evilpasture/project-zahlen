// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/RayTracedNoiseMetrics.hpp
//
// Pixel-domain analysis of ray-tracing noise stability.
//
// Deliberately dependency-free: no engine headers, no reflection, no I/O. It
// operates on a caller-owned RGB8 buffer so the same code runs against a
// captured framebuffer (tests/render/TestRayTracedNoiseStability.cpp) and
// against synthesised patterns used to prove the metrics actually discriminate
// (tests/TestRayTracedNoiseMetrics.cpp). A metric that cannot tell a diagonal
// lattice from blue noise is not a test, so the discriminator is verified
// rather than assumed.
//
// The three properties that matter for a stochastic ray-traced pass:
//
//   1. MAGNITUDE   - how much residual is left frame to frame.
//   2. ISOTROPY    - whether that residual has a preferred direction.
//   3. PERIODICITY - whether it repeats at a fixed spatial lag.
//
// (2) and (3) are what separate a blue-noise dither from a lattice dither such
// as Interleaved Gradient Noise. Both can have identical magnitude -- IGN is
// not louder, it is *structured* -- so a magnitude-only check cannot catch a
// regression back to a lattice. A lattice has low gradient energy along its own
// diagonal (neighbouring pixels on the diagonal share a phase) and a large
// autocorrelation side lobe at its period; blue noise has neither.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ZHLN::Test::Noise {

/// Rec.709 luma, matching the weighting the render tests use.
[[nodiscard]] inline double Luma(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return 0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) + 0.0722 * static_cast<double>(b);
}

/// Extracts the luma plane of an RGB8 image into a caller-owned buffer.
inline void ToLuma(const uint8_t* rgb, int width, int height, double* out) noexcept {
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = Luma(rgb[i * 3u + 0u], rgb[i * 3u + 1u], rgb[i * 3u + 2u]);
    }
}

/// Per-frame residual statistics between two consecutive frames.
struct ResidualStats {
    double meanAbs          = 0.0; ///< Mean |F_t - F_{t-1}| over luma.
    double rms              = 0.0; ///< RMS of the same residual.
    double maxAbs           = 0.0; ///< Worst single-pixel change (firefly guard).
    double changedFraction  = 0.0; ///< Fraction of pixels whose change exceeds `threshold`.
    double isolatedFraction = 0.0; ///< Of changed pixels, fraction with no changed neighbour.
    bool   valid            = false;
};

/**
 * @brief Frame-to-frame residual of a static scene.
 *
 * @param threshold Luma delta above which a pixel counts as "changed". Sits
 *                  above the quantisation floor of an 8-bit framebuffer but
 *                  below a real lighting change.
 *
 * `isolatedFraction` separates converged stochastic noise (which is spatially
 * clustered because neighbouring pixels share world-space geometry) from ray
 * debris and fireflies (single-pixel outliers with no changed neighbour).
 */
[[nodiscard]] inline ResidualStats MeasureResidual(
    const uint8_t* prevRgb, const uint8_t* curRgb, int width, int height, double threshold = 2.0
) noexcept {
    ResidualStats s;
    if (prevRgb == nullptr || curRgb == nullptr || width < 3 || height < 3) {
        return s;
    }

    const std::size_t       count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<uint8_t>    changed(count, 0);
    std::vector<double>     delta(count, 0.0);
    double                  sum = 0.0, sumSq = 0.0, peak = 0.0;
    std::size_t             changedCount = 0, isolatedCount = 0;

    for (std::size_t i = 0; i < count; ++i) {
        const double d = std::abs(Luma(curRgb[i * 3u + 0u], curRgb[i * 3u + 1u], curRgb[i * 3u + 2u]) -
                                  Luma(prevRgb[i * 3u + 0u], prevRgb[i * 3u + 1u], prevRgb[i * 3u + 2u]));
        delta[i] = d;
        sum += d;
        sumSq += d * d;
        peak = std::max(peak, d);
        if (d > threshold) {
            changed[i] = 1;
            ++changedCount;
        }
    }

    // A changed pixel with no changed 4-neighbour is an outlier, not noise.
    for (int y = 1; y < height - 1; ++y) {
        for (int x = 1; x < width - 1; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
            if (changed[i] == 0) {
                continue;
            }
            const uint8_t neighbours = static_cast<uint8_t>(
                changed[i - 1] + changed[i + 1] + changed[i - static_cast<std::size_t>(width)] + changed[i + static_cast<std::size_t>(width)]
            );
            if (neighbours == 0) {
                ++isolatedCount;
            }
        }
    }

    const double n      = static_cast<double>(count);
    s.meanAbs           = sum / n;
    s.rms               = std::sqrt(sumSq / n);
    s.maxAbs            = peak;
    s.changedFraction   = static_cast<double>(changedCount) / n;
    s.isolatedFraction  = changedCount > 0 ? static_cast<double>(isolatedCount) / static_cast<double>(changedCount) : 0.0;
    s.valid             = true;
    return s;
}

/// Gradient energy along the four principal directions, each normalised by its
/// step length so a smooth field scores equally in all four.
struct DirectionalEnergy {
    double e0   = 0.0; ///< +x
    double e45  = 0.0; ///< +x+y (diagonal)
    double e90  = 0.0; ///< +y
    double e135 = 0.0; ///< -x+y (anti-diagonal)

    /// max/min across the four directions. ~1.0 is isotropic; a lattice dither
    /// aligned to a diagonal drives this well above 1 because neighbouring
    /// pixels along that diagonal share phase and so differ less.
    [[nodiscard]] double Anisotropy() const noexcept {
        const double lo = std::min(std::min(e0, e90), std::min(e45, e135));
        const double hi = std::max(std::max(e0, e90), std::max(e45, e135));
        return lo > 1e-12 ? hi / lo : 0.0;
    }
};

/**
 * @brief Directional gradient energy of a scalar field (e.g. a residual plane).
 *
 * The four energies are deliberately NOT normalised by step length. For a
 * *smooth* field a diagonal step spans sqrt(2) pixels and its squared gradient
 * is twice the axial one, so an unnormalised metric would read ~2x anisotropy
 * on a perfectly isotropic ramp. But the field analysed here is a frame-to-frame
 * residual on a static scene, which is noise-like: neighbouring texels are
 * statistically independent, so E[(f(i+d)-f(i))^2] is direction-independent and
 * every direction must score the same. Dividing the diagonals by 2 to suit the
 * smooth case makes white and blue noise both read as ~2.0 anisotropic, which
 * destroys the very discrimination this metric exists to provide (measured:
 * blue 2.08, white 2.03 against a 1.05 threshold).
 *
 * Consequence: this is a noise-structure metric, not a general smoothness
 * metric. Do not point it at a field with strong edges.
 */
[[nodiscard]] inline DirectionalEnergy MeasureDirectionalEnergy(const double* field, int width, int height) noexcept {
    DirectionalEnergy out;
    if (field == nullptr || width < 3 || height < 3) {
        return out;
    }

    double s0 = 0.0, s45 = 0.0, s90 = 0.0, s135 = 0.0;
    std::size_t n0 = 0, n45 = 0, n90 = 0, n135 = 0;
    const int   w = width;

    for (int y = 0; y < height - 1; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
            if (y + 1 < height) {
                const double d = field[i + static_cast<std::size_t>(w)] - field[i];
                s90 += d * d;
                ++n90;
            }
            if (x + 1 < width) {
                const double d = field[i + 1] - field[i];
                s0 += d * d;
                ++n0;
                if (y + 1 < height) {
                    const double dd = field[i + static_cast<std::size_t>(w) + 1] - field[i];
                    s45 += dd * dd;
                    ++n45;
                }
            }
            if (x - 1 >= 0 && y + 1 < height) {
                const double dd = field[i + static_cast<std::size_t>(w) - 1] - field[i];
                s135 += dd * dd;
                ++n135;
            }
        }
    }

    out.e0   = n0 > 0 ? s0 / static_cast<double>(n0) : 0.0;
    out.e45  = n45 > 0 ? s45 / static_cast<double>(n45) : 0.0;
    out.e90  = n90 > 0 ? s90 / static_cast<double>(n90) : 0.0;
    out.e135 = n135 > 0 ? s135 / static_cast<double>(n135) : 0.0;
    return out;
}

/**
 * @brief Largest POSITIVE normalised autocorrelation within `maxLag`.
 *
 * Returns max over non-zero lags of r(lag) / r(0), where r is the mean-centred
 * autocorrelation. Only positive lobes count, deliberately: a periodic lattice
 * correlates POSITIVELY with itself at its own period, which is the defect this
 * detects. Blue noise correlates NEGATIVELY at short lag -- that slight
 * anti-correlation is the mechanism by which it pushes energy into the high
 * frequencies, i.e. the property being tested for, not against. Taking the
 * absolute value would score the shipped blue noise tile at 0.19 and fail it
 * against the same threshold white noise passes at 0.03.
 */
[[nodiscard]] inline double AutocorrelationSideLobe(const double* field, int width, int height, int maxLag = 4) noexcept {
    if (field == nullptr || width < 2 * maxLag + 3 || height < 2 * maxLag + 3) {
        return 0.0;
    }

    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    double            mean  = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        mean += field[i];
    }
    mean /= static_cast<double>(count);

    std::vector<double> c(count);
    double              var = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        c[i] = field[i] - mean;
        var += c[i] * c[i];
    }
    if (var <= 1e-12) {
        return 0.0;
    }

    double peak = 0.0;
    for (int dy = -maxLag; dy <= maxLag; ++dy) {
        for (int dx = -maxLag; dx <= maxLag; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            double  acc = 0.0;
            int     n   = 0;
            const int y0 = std::max(0, -dy), y1 = std::min(height, height - dy);
            const int x0 = std::max(0, -dx), x1 = std::min(width, width - dx);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
                    const std::size_t j = static_cast<std::size_t>(y + dy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x + dx);
                    acc += c[i] * c[j];
                    ++n;
                }
            }
            if (n > 0) {
                // Signed: a negative lobe is blue noise working as intended.
                peak = std::max(peak, acc / var);
            }
        }
    }
    return peak;
}

/// Least-squares slope of `y` against sample index. Negative means the residual
/// is shrinking over time, which is what a temporal accumulator should do on a
/// static scene; a positive or oscillating slope means the noise is being
/// regenerated rather than converged.
[[nodiscard]] inline double LinearSlope(const std::vector<double>& y) noexcept {
    const auto n = static_cast<double>(y.size());
    if (n < 2.0) {
        return 0.0;
    }
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double x = static_cast<double>(i);
        sx += x;
        sy += y[i];
        sxx += x * x;
        sxy += x * y[i];
    }
    const double denom = n * sxx - sx * sx;
    return std::abs(denom) > 1e-12 ? (n * sxy - sx * sy) / denom : 0.0;
}

} // namespace ZHLN::Test::Noise
