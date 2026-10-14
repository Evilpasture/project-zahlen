// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/passes/postprocess/PostProcessResources.hpp
//
// The two fullscreen passes and the lighting pass's lookup tables -- step 2 of
// rendercontext-impl-decomposition.md.
//
// The LTC view infos ride along because ClusteredLightingPass is their only
// reader: they are written by InitLightingLUTs and read back as descriptor
// slots, so they travel with the pipeline that consumes them rather than
// staying loose on Impl. They are still zeroed in Impl's teardown, before the
// backing images are destroyed.

#pragma once

#include "Rendering.hpp"
#include "pipeline/FullscreenPass.hpp"

namespace ZHLN {

using LightingLayout = Vk::ReflectedLayout;
using BlitLayout     = Vk::ReflectedLayout;

struct PostProcessResources {
    Vk::FullscreenPass<LightingLayout> lighting;
    Vk::FullscreenPass<BlitLayout>     blit;

    VkImageViewCreateInfo ltcMat {};
    VkImageViewCreateInfo ltcAmp {};
};

} // namespace ZHLN
