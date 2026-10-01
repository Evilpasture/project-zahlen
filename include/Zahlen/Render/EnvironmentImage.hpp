// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ZHLN {

// A single compact emitter separated during asset preparation. `irradiance`
// is its RGB hemispherical irradiance divided by pi: the old analytic diffuse
// term was irradiance * max(dot(N, direction), 0). A directional LightComponent
// therefore uses color = pi * irradiance and is scaled by ambientExposure at
// shading time. `diffuseSH` is the exact cosine-convolved, /pi SH of the
// CONDITIONED (sunless) panorama, in the shader's basis/order.
struct EnvironmentSun {
    std::array<float, 3> direction {};
    std::array<float, 3> irradiance {};
    std::array<std::array<float, 3>, 9> diffuseSH {};
};

// Owned, top-down, linear RGBA float pixels. The original `rgba` is always
// preserved for the visible sky. When a sun is extracted, `lightingRgba`
// contains a locally inpainted, sunless panorama used for both diffuse SH and
// specular IBL. Empty lightingRgba means the original serves both purposes.
// Decoding and conditioning source images belong to the cooker/host, not core.
struct EnvironmentImage {
    uint32_t           width       = 0;
    uint32_t           height      = 0;
    std::vector<float> rgba;
    std::vector<float> lightingRgba;
    std::optional<EnvironmentSun> sun;
    // Optional. When zero, RenderContext hashes both panoramas and SH before
    // baking IBL. Supplied hashes must cover all prepared lighting data.
    uint64_t           contentHash = 0;
};

} // namespace ZHLN
