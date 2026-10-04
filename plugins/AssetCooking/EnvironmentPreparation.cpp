// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EnvironmentPreparation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

namespace ZHLN::AssetCooking {
namespace {

constexpr double kPi = std::numbers::pi_v<double>;
constexpr std::array<double, 9> kCosineConvolution = {1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25};

// Match resources/shaders/sampling.slang and pbr_helpers.slang: x=cos(phi),
// z=sin(phi), phi=2*pi*(u-.5), theta=pi*v, and the SH already contains /pi.
[[nodiscard]] auto Basis(double x, double y, double z) noexcept -> std::array<double, 9> {
    return {
        0.282095, -0.488603 * y, 0.488603 * z, -0.488603 * x,
        1.092548 * x * y, -1.092548 * y * z, 0.315392 * (3.0 * z * z - 1.0),
        -1.092548 * x * z, 0.546274 * (x * x - y * y)
    };
}

[[nodiscard]] auto Luminance(const float* rgb) noexcept -> double {
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
}

// Conservative as in the original irradiance fix: only promote a compact
// emitter when its smooth residual does not itself ring below zero.
[[nodiscard]] auto NonnegativeEverywhere(const std::array<std::array<float, 3>, 9>& sh) noexcept -> bool {
    constexpr int kDirections = 4096;
    constexpr double goldenAngle = kPi * (3.0 - 2.2360679774997896964);
    for (int i = 0; i < kDirections; ++i) {
        const double z = 1.0 - 2.0 * (static_cast<double>(i) + 0.5) / kDirections;
        const double r = std::sqrt(1.0 - z * z);
        const double phi = goldenAngle * i;
        const auto basis = Basis(r * std::cos(phi), r * std::sin(phi), z);
        for (int channel = 0; channel < 3; ++channel) {
            double value = 0.0;
            for (int c = 0; c < 9; ++c) value += sh[c][channel] * basis[c];
            if (value < -1e-5) return false;
        }
    }
    return true;
}

// Inpaint only masked emitter texels. Eight rays find nearby sky outside the
// hot region, with horizontal wrap at the panorama seam. This preserves the
// actual surrounding HDR colors in both IBL paths instead of introducing a
// black specular hole. Return false if a compact source has no local sky.
[[nodiscard]] auto InpaintHotPixel(const EnvironmentImage& image, const std::vector<uint8_t>& mask,
                                   uint32_t x, uint32_t y, float* replacement) noexcept -> bool {
    constexpr std::array<std::array<int, 2>, 8> rays {{
        {{-1, -1}}, {{0, -1}}, {{1, -1}}, {{-1, 0}},
        {{1, 0}}, {{-1, 1}}, {{0, 1}}, {{1, 1}}
    }};
    const int width = static_cast<int>(image.width), height = static_cast<int>(image.height);
    const int maxDistance = std::max(4, std::max(width, height) / 16);
    double sky[3] {};
    int count = 0;
    for (const auto& ray: rays) {
        for (int distance = 1; distance <= maxDistance; ++distance) {
            const int ny = static_cast<int>(y) + distance * ray[1];
            if (ny < 0 || ny >= height) break;
            const int nx = (static_cast<int>(x) + distance * ray[0] % width + width) % width;
            const size_t at = static_cast<size_t>(ny) * image.width + static_cast<uint32_t>(nx);
            if (mask[at] != 0) continue;
            const float* pixel = image.rgba.data() + at * 4u;
            for (int c = 0; c < 3; ++c) sky[c] += pixel[c];
            ++count;
            break;
        }
    }
    if (count < 2) return false;
    for (int c = 0; c < 3; ++c) replacement[c] = static_cast<float>(sky[c] / count);
    return true;
}

} // namespace

void PrepareEnvironmentImage(EnvironmentImage& image) {
    if (image.sun || !image.lightingRgba.empty() || image.width == 0 || image.height == 0 ||
        image.rgba.size() != static_cast<size_t>(image.width) * image.height * 4u) return;

    const uint32_t width = image.width, height = image.height;
    const double deltaPhi = 2.0 * kPi / width, deltaTheta = kPi / height;
    double totalLuminance = 0.0;
    for (uint32_t y = 0; y < height; ++y) {
        const double texelAngle = deltaPhi * (std::cos(deltaTheta * y) - std::cos(deltaTheta * (y + 1)));
        for (uint32_t x = 0; x < width; ++x) {
            const float* pixel = image.rgba.data() + (static_cast<size_t>(y) * width + x) * 4u;
            if (!std::isfinite(pixel[0]) || !std::isfinite(pixel[1]) || !std::isfinite(pixel[2]) ||
                pixel[0] < 0.0f || pixel[1] < 0.0f || pixel[2] < 0.0f) return;
            totalLuminance += Luminance(pixel) * texelAngle;
        }
    }
    if (totalLuminance <= 0.0 || !std::isfinite(totalLuminance)) return;

    // Deliberately retain the proven, conservative detection thresholds from
    // the earlier renderer patch. Diffuse-only SH windowing was insufficient
    // for the olives; a broad or multi-source HDR is NOT promoted to a sun.
    const double threshold = 16.0 * totalLuminance / (4.0 * kPi);
    std::vector<uint8_t> hot(static_cast<size_t>(width) * height);
    double hotLuminance = 0.0;
    std::array<double, 3> hotMoment {};
    for (uint32_t y = 0; y < height; ++y) {
        const double theta = deltaTheta * (y + 0.5);
        const double v = std::cos(theta), r = std::sin(theta);
        const double texelAngle = deltaPhi * (std::cos(deltaTheta * y) - std::cos(deltaTheta * (y + 1)));
        for (uint32_t x = 0; x < width; ++x) {
            const size_t at = static_cast<size_t>(y) * width + x;
            const double lum = Luminance(image.rgba.data() + at * 4u);
            if (lum <= threshold) continue;
            hot[at] = 1;
            const double phi = deltaPhi * (x + 0.5) - kPi;
            const double light = lum * texelAngle;
            hotLuminance += light;
            hotMoment[0] += light * r * std::cos(phi);
            hotMoment[1] += light * v;
            hotMoment[2] += light * r * std::sin(phi);
        }
    }
    const double hotLength = std::sqrt(hotMoment[0] * hotMoment[0] + hotMoment[1] * hotMoment[1] + hotMoment[2] * hotMoment[2]);
    if (hotLuminance < 0.2 * totalLuminance || hotLength < 0.998 * hotLuminance) return;

    std::vector<float> lighting = image.rgba;
    std::array<double, 3> emitterIrradiance {};
    std::array<double, 3> emitterMoment {};
    double emitterLuminance = 0.0;
    for (uint32_t y = 0; y < height; ++y) {
        const double theta = deltaTheta * (y + 0.5);
        const double v = std::cos(theta), r = std::sin(theta);
        const double texelAngle = deltaPhi * (std::cos(deltaTheta * y) - std::cos(deltaTheta * (y + 1)));
        for (uint32_t x = 0; x < width; ++x) {
            const size_t at = static_cast<size_t>(y) * width + x;
            if (hot[at] == 0) continue;
            float replacement[3] {};
            if (!InpaintHotPixel(image, hot, x, y, replacement)) return;
            const float* original = image.rgba.data() + at * 4u;
            float* conditioned = lighting.data() + at * 4u;
            float residual[3] {};
            for (int c = 0; c < 3; ++c) {
                conditioned[c] = std::min(original[c], replacement[c]);
                residual[c] = original[c] - conditioned[c];
                emitterIrradiance[c] += residual[c] * texelAngle / kPi;
            }
            const double phi = deltaPhi * (x + 0.5) - kPi;
            const double contribution = Luminance(residual) * texelAngle;
            emitterLuminance += contribution;
            emitterMoment[0] += contribution * r * std::cos(phi);
            emitterMoment[1] += contribution * v;
            emitterMoment[2] += contribution * r * std::sin(phi);
        }
    }
    const double emitterLength = std::sqrt(emitterMoment[0] * emitterMoment[0] + emitterMoment[1] * emitterMoment[1] + emitterMoment[2] * emitterMoment[2]);
    if (emitterLuminance <= 0.0 || emitterLength < 0.998 * emitterLuminance) return;

    std::array<std::array<double, 3>, 9> exactSH {};
    std::vector<double> cosPhi(width), sinPhi(width);
    for (uint32_t x = 0; x < width; ++x) {
        const double phi = deltaPhi * (x + 0.5) - kPi;
        cosPhi[x] = std::cos(phi);
        sinPhi[x] = std::sin(phi);
    }
    for (uint32_t y = 0; y < height; ++y) {
        const double theta = deltaTheta * (y + 0.5);
        const double v = std::cos(theta), r = std::sin(theta);
        const double texelAngle = deltaPhi * (std::cos(deltaTheta * y) - std::cos(deltaTheta * (y + 1)));
        for (uint32_t x = 0; x < width; ++x) {
            const auto basis = Basis(r * cosPhi[x], v, r * sinPhi[x]);
            const float* pixel = lighting.data() + (static_cast<size_t>(y) * width + x) * 4u;
            for (int c = 0; c < 9; ++c) {
                const double scale = basis[c] * texelAngle * kCosineConvolution[c];
                for (int channel = 0; channel < 3; ++channel) exactSH[c][channel] += pixel[channel] * scale;
            }
        }
    }

    EnvironmentSun sun {};
    for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(emitterIrradiance[c]) || emitterIrradiance[c] > std::numeric_limits<float>::max()) return;
        sun.direction[c] = static_cast<float>(emitterMoment[c] / emitterLength);
        sun.irradiance[c] = static_cast<float>(emitterIrradiance[c]);
    }
    for (int c = 0; c < 9; ++c) {
        for (int channel = 0; channel < 3; ++channel) {
            if (!std::isfinite(exactSH[c][channel]) || std::abs(exactSH[c][channel]) > std::numeric_limits<float>::max()) return;
            sun.diffuseSH[c][channel] = static_cast<float>(exactSH[c][channel]);
        }
    }
    if (!NonnegativeEverywhere(sun.diffuseSH)) return;

    image.lightingRgba = std::move(lighting);
    image.sun = sun;
    image.contentHash = 0; // any hash of the unconditioned source is now stale
}

} // namespace ZHLN::AssetCooking
