// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "IrradianceSH.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

namespace ZHLN::Vk {
namespace {

constexpr double kPi = std::numbers::pi_v<double>;
constexpr std::array<double, 9> kCosineConvolution = {1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25};

// Match resources/shaders/sampling.slang and pbr_helpers.slang exactly: the
// equirectangular image uses x = cos(phi), z = sin(phi), with phi = 2*pi*(u-.5).
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

// Do not replace the GPU SH with a separation that would still ring below
// zero. The compact emitter test is intentionally conservative; other HDRs
// continue to use the existing bake if their remainder is not smooth enough.
[[nodiscard]] auto NonnegativeEverywhere(const std::array<std::array<float, 3>, 9>& sh) noexcept -> bool {
    constexpr int kDirections = 4096;
    constexpr double goldenAngle = kPi * (3.0 - 2.2360679774997896964); // pi * (3 - sqrt(5))
    for (int i = 0; i < kDirections; ++i) {
        const double z = 1.0 - 2.0 * (static_cast<double>(i) + 0.5) / kDirections;
        const double r = std::sqrt(1.0 - z * z);
        const double phi = goldenAngle * i;
        const auto basis = Basis(r * std::cos(phi), r * std::sin(phi), z);
        for (int channel = 0; channel < 3; ++channel) {
            double irradiance = 0.0;
            for (int c = 0; c < 9; ++c) irradiance += sh[c][channel] * basis[c];
            if (irradiance < -1e-5) return false;
        }
    }
    return true;
}

} // namespace

auto SeparateCompactEnvironmentEmitter(std::span<const float> rgba, uint32_t width, uint32_t height)
    -> std::optional<SeparatedIrradianceSH> {
    if (width == 0 || height == 0 || rgba.size() != static_cast<size_t>(width) * height * 4u) return std::nullopt;

    const double deltaPhi = 2.0 * kPi / width;
    const double deltaTheta = kPi / height;
    double totalLuminance = 0.0;
    for (uint32_t y = 0; y < height; ++y) {
        const double texelAngle = deltaPhi * (std::cos(deltaTheta * y) - std::cos(deltaTheta * (y + 1)));
        for (uint32_t x = 0; x < width; ++x) {
            const float* pixel = rgba.data() + (static_cast<size_t>(y) * width + x) * 4u;
            if (!std::isfinite(pixel[0]) || !std::isfinite(pixel[1]) || !std::isfinite(pixel[2]) ||
                pixel[0] < 0.0f || pixel[1] < 0.0f || pixel[2] < 0.0f) return std::nullopt;
            totalLuminance += Luminance(pixel) * texelAngle;
        }
    }
    if (totalLuminance <= 0.0) return std::nullopt;

    // Only a tiny source that carries a substantial part of the light needs
    // de-ringing. Requiring >20% of total energy and an angular concentration
    // of >0.998 avoids changing normal skies or multi-softbox environments.
    const double threshold = 16.0 * totalLuminance / (4.0 * kPi);
    double hotLuminance = 0.0;
    std::array<double, 3> moment {};
    for (uint32_t y = 0; y < height; ++y) {
        const double theta = deltaTheta * (y + 0.5);
        const double v = std::cos(theta);
        const double r = std::sin(theta);
        const double texelAngle = deltaPhi * (std::cos(deltaTheta * y) - std::cos(deltaTheta * (y + 1)));
        for (uint32_t x = 0; x < width; ++x) {
            const float* pixel = rgba.data() + (static_cast<size_t>(y) * width + x) * 4u;
            const double lum = Luminance(pixel);
            if (lum <= threshold) continue;
            const double phi = deltaPhi * (x + 0.5) - kPi;
            const double light = lum * texelAngle;
            hotLuminance += light;
            moment[0] += light * r * std::cos(phi);
            moment[1] += light * v;
            moment[2] += light * r * std::sin(phi);
        }
    }
    const double momentLength = std::sqrt(moment[0] * moment[0] + moment[1] * moment[1] + moment[2] * moment[2]);
    if (hotLuminance < 0.2 * totalLuminance || momentLength < 0.998 * hotLuminance) return std::nullopt;

    SeparatedIrradianceSH result {};
    for (int axis = 0; axis < 3; ++axis) result.emitterDirection[axis] = static_cast<float>(moment[axis] / momentLength);

    // Exact solid-angle integration of the *remaining* panorama. In
    // particular, do not subtract sun SH from the GPU's 16k-sample estimate:
    // a subpixel HDR sun can be mis-sampled by several percent, and the
    // subtraction would leave its colored ringing behind.
    std::array<std::array<double, 3>, 9> remainder {};
    std::array<double, 3> emitterIrradiance {};
    std::vector<double> cosPhi(width), sinPhi(width);
    for (uint32_t x = 0; x < width; ++x) {
        const double phi = deltaPhi * (x + 0.5) - kPi;
        cosPhi[x] = std::cos(phi);
        sinPhi[x] = std::sin(phi);
    }
    for (uint32_t y = 0; y < height; ++y) {
        const double theta = deltaTheta * (y + 0.5);
        const double v = std::cos(theta);
        const double r = std::sin(theta);
        const double texelAngle = deltaPhi * (std::cos(deltaTheta * y) - std::cos(deltaTheta * (y + 1)));
        for (uint32_t x = 0; x < width; ++x) {
            const float* pixel = rgba.data() + (static_cast<size_t>(y) * width + x) * 4u;
            if (Luminance(pixel) > threshold) {
                for (int channel = 0; channel < 3; ++channel) emitterIrradiance[channel] += pixel[channel] * texelAngle / kPi;
            } else {
                const auto basis = Basis(r * cosPhi[x], v, r * sinPhi[x]);
                for (int c = 0; c < 9; ++c) {
                    const double scale = basis[c] * texelAngle * kCosineConvolution[c];
                    for (int channel = 0; channel < 3; ++channel) remainder[c][channel] += pixel[channel] * scale;
                }
            }
        }
    }
    for (int c = 0; c < 9; ++c) {
        for (int channel = 0; channel < 3; ++channel) {
            if (!std::isfinite(remainder[c][channel]) || std::abs(remainder[c][channel]) > std::numeric_limits<float>::max())
                return std::nullopt;
            result.coefficients[c][channel] = static_cast<float>(remainder[c][channel]);
        }
    }
    for (int channel = 0; channel < 3; ++channel) {
        if (!std::isfinite(emitterIrradiance[channel]) || emitterIrradiance[channel] > std::numeric_limits<float>::max())
            return std::nullopt;
        result.emitterIrradiance[channel] = static_cast<float>(emitterIrradiance[channel]);
    }
    if (!NonnegativeEverywhere(result.coefficients)) return std::nullopt;
    return result;
}

} // namespace ZHLN::Vk
