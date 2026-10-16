// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "FrameContext.hpp"
#include "Rendering.hpp"
namespace ZHLN::Passes {

struct ReflectionInputs {
    Vk::ImageWrite     prefiltered;
    Vk::ImageWrite     visualSky;
    Vk::ImageWrite     brdfLut;
    Vk::ImageWrite     blueNoise;
    Vk::AsAddressWrite tlas {};
};

[[nodiscard]] auto GatherReflectionInputs(RenderFrameContext& frame) noexcept -> ReflectionInputs;

[[nodiscard]] auto ReflectionVariant(RenderFrameContext& frame) noexcept -> uint32_t;

} // namespace ZHLN::Passes
