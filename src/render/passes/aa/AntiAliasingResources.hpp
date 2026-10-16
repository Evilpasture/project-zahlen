// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include "pipeline/FullscreenPass.hpp"

namespace ZHLN {

using TAALayout        = Vk::ReflectedLayout;
using FXAALayout       = Vk::ReflectedLayout;
using MLAALayout       = Vk::ReflectedLayout;
using SMAAEdgeLayout   = Vk::ReflectedLayout;
using SMAAWeightLayout = Vk::ReflectedLayout;
using SMAABlendLayout  = Vk::ReflectedLayout;

struct AntiAliasingResources {
    Vk::FullscreenPass<TAALayout>        taa;
    Vk::FullscreenPass<FXAALayout>       fxaa;
    Vk::FullscreenPass<MLAALayout>       mlaa;
    Vk::FullscreenPass<SMAAEdgeLayout>   smaaEdge;
    Vk::FullscreenPass<SMAAWeightLayout> smaaWeight;
    Vk::FullscreenPass<SMAABlendLayout>  smaaBlend;
};

} // namespace ZHLN
