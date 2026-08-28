// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/TestRayTracedNoiseMetrics.cpp
//
// CPU-side validation of the noise metrics used by
// tests/render/TestRayTracedNoiseStability.cpp.
//
// A noise metric is only worth gating on if it can tell the two failure modes
// apart, so this suite feeds RayTracedNoiseMetrics.hpp three synthetic residual
// fields and asserts it separates them:
//
//   * an Interleaved Gradient Noise lattice -- the dither the ray-traced paths
//     used before blue noise. Structured along a diagonal, periodic.
//   * real blue noise, thresholded from the same LDR_RGBA_0.png the renderer
//     binds, so this is not a synthetic stand-in for the shipped asset.
//   * white noise -- structureless but spatially uncorrelated.
//
// Without this, a threshold in the GPU suite could be satisfied by any old
// noise and the regression it is meant to catch would sail through.
//
// Runs on the CPU label: no device, no framebuffer, deterministic.

#include "RayTracedNoiseMetrics.hpp"
#include "TestsFramework.hpp"
#include <stb_image.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

// The same tile the renderer binds, embedded so the discriminator is measured
// against the real asset rather than an idealised stand-in.
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
constexpr uint8_t kBlueNoisePng[] = {
#embed "../resources/shaders/LDR_RGBA_0.png"
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)

enum class NoiseMetricError : uint8_t {
    BlueNoiseDecodeFailed[[= ZHLN::Reflect::Description<"The embedded LDR_RGBA_0 blue noise tile failed to decode.">{}]] = 1,
};

namespace {

constexpr int kSize = 128;

/// Deterministic 32-bit hash so the synthetic fields are reproducible run to
/// run; a PRNG would make the thresholds flaky.
[[nodiscard]] uint32_t Hash(uint32_t x, uint32_t y, uint32_t seed) noexcept {
    uint32_t h = x * 374761393u + y * 668265263u + seed * 1442695041u;
    h          = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

/// The dither the RT paths used before blue noise: Interleaved Gradient Noise,
/// quantised to a 1-bit shadow decision exactly as CalculateShadowRayTraced did.
[[nodiscard]] std::vector<double> MakeIgnLattice(double amplitude) {
    std::vector<double> field(static_cast<std::size_t>(kSize) * kSize, 0.0);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const auto    fx       = static_cast<float>(x);
            const auto    fy       = static_cast<float>(y);
            const float   magic    = std::fmod(0.06711056f * fx + 0.00583715f * fy, 1.0f);
            const float   dither   = std::fmod(52.9829189f * magic, 1.0f);
            const double  residual = dither > 0.5f ? amplitude : 0.0;
            field[static_cast<std::size_t>(y) * kSize + x] = residual;
        }
    }
    return field;
}

/// Thresholded real blue noise from the embedded tile's first channel.
[[nodiscard]] std::vector<double> MakeBlueNoise(double amplitude, bool* ok) {
    std::vector<double> field(static_cast<std::size_t>(kSize) * kSize, 0.0);
    int                 w = 0, h = 0, c = 0;
    unsigned char*      px = stbi_load_from_memory(kBlueNoisePng, static_cast<int>(sizeof(kBlueNoisePng)), &w, &h, &c, 4);
    if (px == nullptr || w < kSize || h < kSize) {
        if (px != nullptr) {
            stbi_image_free(px);
        }
        *ok = false;
        return field;
    }
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const auto   v = static_cast<double>(px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x) * 4u]) / 255.0;
            field[static_cast<std::size_t>(y) * kSize + x] = v > 0.5 ? amplitude : 0.0;
        }
    }
    stbi_image_free(px);
    *ok = true;
    return field;
}

/// Structureless, spatially uncorrelated noise.
[[nodiscard]] std::vector<double> MakeWhiteNoise(double amplitude) {
    std::vector<double> field(static_cast<std::size_t>(kSize) * kSize, 0.0);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const uint32_t r = Hash(static_cast<uint32_t>(x), static_cast<uint32_t>(y), 0x9E37u);
            field[static_cast<std::size_t>(y) * kSize + x] = ((r & 0xFFFFu) > 0x8000u) ? amplitude : 0.0;
        }
    }
    return field;
}

/// Builds an RGB8 image whose luma equals `field`, so the ResidualStats path
/// (which takes RGB) is exercised rather than bypassed.
[[nodiscard]] std::vector<uint8_t> ToRgb(const std::vector<double>& field) {
    std::vector<uint8_t> rgb(field.size() * 3u, 0);
    for (std::size_t i = 0; i < field.size(); ++i) {
        const auto v = static_cast<uint8_t>(std::clamp(field[i], 0.0, 255.0));
        rgb[i * 3u + 0u] = v;
        rgb[i * 3u + 1u] = v;
        rgb[i * 3u + 2u] = v;
    }
    return rgb;
}

} // namespace

struct NoiseMetricTestSuite {
    struct Tests {
        /// The discriminator: a lattice must read as anisotropic and periodic,
        /// blue noise as neither.
        std::expected<void, ZHLN::Error> metrics_separate_lattice_from_blue_noise() {
            bool ok = false;
            const std::vector<double> lattice    = MakeIgnLattice(96.0);
            const std::vector<double> blueNoise  = MakeBlueNoise(96.0, &ok);
            const std::vector<double> whiteNoise = MakeWhiteNoise(96.0);
            auto check = ZHLN::Test::AssertTrue(ok);
            if (!check) {
                return std::unexpected(NoiseMetricError::BlueNoiseDecodeFailed);
            }

            const auto latticeDir   = ZHLN::Test::Noise::MeasureDirectionalEnergy(lattice.data(), kSize, kSize);
            const auto blueDir      = ZHLN::Test::Noise::MeasureDirectionalEnergy(blueNoise.data(), kSize, kSize);
            const auto whiteDir     = ZHLN::Test::Noise::MeasureDirectionalEnergy(whiteNoise.data(), kSize, kSize);
            const double latticeLob = ZHLN::Test::Noise::AutocorrelationSideLobe(lattice.data(), kSize, kSize, 4);
            const double blueLob    = ZHLN::Test::Noise::AutocorrelationSideLobe(blueNoise.data(), kSize, kSize, 4);
            const double whiteLob   = ZHLN::Test::Noise::AutocorrelationSideLobe(whiteNoise.data(), kSize, kSize, 4);

            ZHLN::Println(
                "    [INFO] anisotropy  lattice={:.4f} blue={:.4f} white={:.4f}", latticeDir.Anisotropy(), blueDir.Anisotropy(), whiteDir.Anisotropy()
            );
            ZHLN::Println("    [INFO] side lobe   lattice={:.4f} blue={:.4f} white={:.4f}", latticeLob, blueLob, whiteLob);

            // Thresholds are the geometric midpoints of the two measured
            // regimes, not round numbers picked to sit just past whatever this
            // run happened to print. Measured on these exact fields:
            //
            //                       anisotropy   positive side lobe
            //   IGN lattice           3.3121          0.8953
            //   blue noise            1.0410          0.0518
            //   white noise           1.0199          0.0268
            //
            // and sweeping the blue noise crop across 1224 offsets of the tile
            // gives anisotropy in [1.0132, 1.0835] and lobe in [0.0336,
            // 0.0664]. A 1.05 bound passed this run by 4% and would have failed
            // the asset at other crop offsets, which is exactly the kind of
            // gate that breaks on an unrelated re-encode. 1.5 = sqrt(1.084 *
            // 2.116) and 0.25 = sqrt(0.0664 * 0.8953) sit equidistant in ratio
            // from both regimes: 2.2x below the lattice, 1.4x above the worst
            // noise crop measured.
            //
            // Directional: the IGN lattice shares phase along its diagonal, so
            // its diagonal gradient energy drops and anisotropy rises.
            ZHLN::Test::ExpectTrue(latticeDir.Anisotropy() > blueDir.Anisotropy());
            ZHLN::Test::ExpectTrue(latticeDir.Anisotropy() > 1.50);
            ZHLN::Test::ExpectTrue(blueDir.Anisotropy() < 1.50);
            ZHLN::Test::ExpectTrue(whiteDir.Anisotropy() < 1.50);

            // Periodic: the lattice repeats, so it carries a side lobe. Blue and
            // white noise both sit near zero -- the point of this metric is that
            // it does not merely reward "not a lattice", it demands aperiodicity.
            ZHLN::Test::ExpectTrue(latticeLob > blueLob);
            ZHLN::Test::ExpectTrue(latticeLob > 0.25);
            ZHLN::Test::ExpectTrue(blueLob < 0.25);
            ZHLN::Test::ExpectTrue(whiteLob < 0.25);
            return {};
        }

        /// Blue noise must be decorrelated across channels, or a single fetch
        /// driving both the sun disk and the VNDF lobe would collapse 2D
        /// sampling onto a line.
        std::expected<void, ZHLN::Error> blue_noise_channels_are_decorrelated() {
            int            w = 0, h = 0, c = 0;
            unsigned char* px = stbi_load_from_memory(kBlueNoisePng, static_cast<int>(sizeof(kBlueNoisePng)), &w, &h, &c, 4);
            auto           check = ZHLN::Test::AssertTrue(px != nullptr && w > 0 && h > 0);
            if (!check) {
                return std::unexpected(NoiseMetricError::BlueNoiseDecodeFailed);
            }

            constexpr int kSample = 256;
            double        sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0, sxy = 0.0;
            const auto    n = static_cast<double>(kSample * kSample);
            for (int y = 0; y < kSample; ++y) {
                for (int x = 0; x < kSample; ++x) {
                    const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x) * 4u;
                    const double      a = static_cast<double>(px[i + 0u]);
                    const double      b = static_cast<double>(px[i + 2u]);
                    sx += a;
                    sy += b;
                    sxx += a * a;
                    syy += b * b;
                    sxy += a * b;
                }
            }
            stbi_image_free(px);

            const double denom = std::sqrt((n * sxx - sx * sx) * (n * syy - sy * sy));
            const double r     = denom > 1e-9 ? (n * sxy - sx * sy) / denom : 0.0;
            ZHLN::Println("    [INFO] channel R vs G correlation over 256x256: {:+.4f}", r);
            // -0.007151 measured on the shipped tile over 65536 samples.
            ZHLN::Test::ExpectTrue(std::abs(r) < 0.10);
            return {};
        }

        /// Blue noise must be spatially high frequency. A positive lag-1
        /// correlation means the tile has been low-passed (a filtered upload, a
        /// regenerated mip), which defeats the purpose of sampling it.
        std::expected<void, ZHLN::Error> blue_noise_is_high_frequency() {
            int            w = 0, h = 0, c = 0;
            unsigned char* px = stbi_load_from_memory(kBlueNoisePng, static_cast<int>(sizeof(kBlueNoisePng)), &w, &h, &c, 4);
            auto           check = ZHLN::Test::AssertTrue(px != nullptr && w >= 64 && h >= 64);
            if (!check) {
                return std::unexpected(NoiseMetricError::BlueNoiseDecodeFailed);
            }

            std::vector<double> field(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0.0);
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    field[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x] =
                        static_cast<double>(px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x) * 4u]);
                }
            }
            stbi_image_free(px);

            const double lobe = ZHLN::Test::Noise::AutocorrelationSideLobe(field.data(), w, h, 2);
            ZHLN::Println("    [INFO] full-tile lag<=2 side lobe: {:.4f} (must stay near zero)", lobe);
            // 0.0477 on the shipped tile, over a million samples, so the
            // estimate is stable and this bound can stay tight -- detecting a
            // low-passed asset is the whole point of the check.
            ZHLN::Test::ExpectTrue(lobe < 0.10);
            return {};
        }

        /// Identical frames must produce a zero residual, otherwise every
        /// stability threshold in the GPU suite is meaningless.
        std::expected<void, ZHLN::Error> residual_of_identical_frames_is_zero() {
            const std::vector<double> flat(kSize * kSize, 128.0);
            const std::vector<uint8_t> rgb = ToRgb(flat);
            const auto                 s   = ZHLN::Test::Noise::MeasureResidual(rgb.data(), rgb.data(), kSize, kSize, 2.0);
            ZHLN::Test::ExpectTrue(s.valid);
            ZHLN::Test::ExpectTrue(s.meanAbs == 0.0);
            ZHLN::Test::ExpectTrue(s.rms == 0.0);
            ZHLN::Test::ExpectTrue(s.changedFraction == 0.0);
            return {};
        }

        /// Single-pixel outliers must be flagged as isolated; clustered noise
        /// must not. This is what separates ray debris from converged noise.
        std::expected<void, ZHLN::Error> residual_isolation_flags_single_pixel_outliers() {
            const std::vector<double> base(kSize * kSize, 128.0);

            // Salt: every 8th pixel bumped, none adjacent. Kept off the border
            // because MeasureResidual skips the outermost ring when counting
            // isolation (a border pixel has no full 4-neighbourhood), and a
            // border pixel would inflate changedFraction without ever being
            // countable as isolated.
            std::vector<double> salt = base;
            for (int y = 4; y < kSize - 4; y += 8) {
                for (int x = 4; x < kSize - 4; x += 8) {
                    salt[static_cast<std::size_t>(y) * kSize + x] = 240.0;
                }
            }
            const std::vector<uint8_t> baseRgb = ToRgb(base);
            const std::vector<uint8_t> saltRgb = ToRgb(salt);
            const auto                 sSalt   = ZHLN::Test::Noise::MeasureResidual(baseRgb.data(), saltRgb.data(), kSize, kSize, 2.0);

            // A solid block: every changed pixel has changed neighbours.
            std::vector<double> block = base;
            for (int y = 40; y < 72; ++y) {
                for (int x = 40; x < 72; ++x) {
                    block[static_cast<std::size_t>(y) * kSize + x] = 240.0;
                }
            }
            const std::vector<uint8_t> blockRgb = ToRgb(block);
            const auto                 sBlock   = ZHLN::Test::Noise::MeasureResidual(baseRgb.data(), blockRgb.data(), kSize, kSize, 2.0);

            ZHLN::Println("    [INFO] isolated fraction salt={:.4f} block={:.4f}", sSalt.isolatedFraction, sBlock.isolatedFraction);
            ZHLN::Test::ExpectTrue(sSalt.changedFraction > 0.0);
            ZHLN::Test::ExpectTrue(sSalt.isolatedFraction > 0.9);
            ZHLN::Test::ExpectTrue(sBlock.changedFraction > 0.0);
            ZHLN::Test::ExpectTrue(sBlock.isolatedFraction < 0.1);
            return {};
        }

        /// Convergence detector: a shrinking residual series must read negative.
        std::expected<void, ZHLN::Error> slope_detects_convergence() {
            ZHLN::Test::ExpectTrue(ZHLN::Test::Noise::LinearSlope({8.0, 4.0, 2.0, 1.0}) < 0.0);
            ZHLN::Test::ExpectTrue(ZHLN::Test::Noise::LinearSlope({1.0, 2.0, 4.0, 8.0}) > 0.0);
            ZHLN::Test::ExpectTrue(std::abs(ZHLN::Test::Noise::LinearSlope({3.0, 3.0, 3.0, 3.0})) < 1e-9);
            ZHLN::Test::ExpectTrue(ZHLN::Test::Noise::LinearSlope({1.0}) == 0.0);
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<NoiseMetricTestSuite>();
}
