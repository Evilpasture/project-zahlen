// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"

namespace ZHLN::Passes {

// Inputs shared by the opaque and translucent reflection graph passes: the
// prefiltered environment map, original visible sky, split-sum BRDF lookup,
// blue-noise tile, and top-level acceleration structure. Their surface reads
// and output targets differ; these engine inputs do not.
struct ReflectionInputs {
    Vk::ImageWrite    prefiltered;
    Vk::ImageWrite    visualSky;
    Vk::ImageWrite    brdfLut;
    Vk::ImageWrite    blueNoise;
    Vk::AsAddressWrite tlas {};
};

[[nodiscard]] auto GatherReflectionInputs(RenderContext::Impl& impl) noexcept -> ReflectionInputs;

// Which specialization of the reflection pipeline this frame runs: bit 0 adds
// screen-space reflection, bit 1 the ray-traced one. Resolved from the frame's
// settings so a pass does not have to be told.
[[nodiscard]] auto ReflectionVariant(RenderContext::Impl& impl) noexcept -> uint32_t;

}
