// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include "pipeline/ComputePass.hpp"
#include "pipeline/FullscreenPass.hpp"

namespace ZHLN {

using LightingLayout = Vk::ReflectedLayout;
using BlitLayout     = Vk::ReflectedLayout;

// Everything that runs after the scene is lit: the tonemap chain's producers
// (threshold and the down/up Kawase pyramid), plus the two fullscreen passes
// and the LTC lookup-table views.
struct PostProcessResources {
    Vk::FullscreenPass<LightingLayout> lighting;
    Vk::FullscreenPass<BlitLayout>     blit;

    Vk::BoundComputePass<Vk::DynamicComputePass> bloomThreshold;
    Vk::BoundComputePass<Vk::DynamicComputePass> bloomDown;
    Vk::BoundComputePass<Vk::DynamicComputePass> bloomUp;

    Vk::ImageView ltcMat;
    Vk::ImageView ltcAmp;
};

} // namespace ZHLN
