// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace ZHLN::Vk {

// The diffuse part of a compact HDR emitter cannot be represented faithfully
// by nine SH coefficients: truncation makes the shadowed side go negative.
// Keep the smoothly varying remainder in SH and evaluate the emitter's
// cosine-weighted irradiance analytically instead. All values are linear RGB;
// the coefficients use the same normalized basis and /pi convention as
// EvalSHBasis / EvaluateSH in the shaders.
struct SeparatedIrradianceSH {
    std::array<std::array<float, 3>, 9> coefficients {};
    std::array<float, 3> emitterDirection {};
    std::array<float, 3> emitterIrradiance {};
};

// Only separates a *single*, bright, compact emitter. An ordinary / multi-light
// panorama returns nullopt so its existing GPU SH bake remains unchanged.
[[nodiscard]] auto SeparateCompactEnvironmentEmitter(std::span<const float> rgba, uint32_t width, uint32_t height)
    -> std::optional<SeparatedIrradianceSH>;

} // namespace ZHLN::Vk
