// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include "pipeline/FullscreenPass.hpp"

namespace ZHLN {

using LightingLayout = Vk::ReflectedLayout;
using BlitLayout     = Vk::ReflectedLayout;

struct PostProcessResources {
    Vk::FullscreenPass<LightingLayout> lighting;
    Vk::FullscreenPass<BlitLayout>     blit;

    Vk::ImageView ltcMat;
    Vk::ImageView ltcAmp;
};

} // namespace ZHLN
