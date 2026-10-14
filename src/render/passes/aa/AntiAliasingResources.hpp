// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/passes/aa/AntiAliasingResources.hpp
//
// The six anti-aliasing pipelines, owned as one group: TAA, FXAA, MLAA and
// SMAA's three stages. Step 2 of rendercontext-impl-decomposition.md -- each of
// these is read by exactly one pass, so they belong to the passes rather than
// being parked on RenderContext::Impl where all 67 translation units of the
// renderer can see them.
//
// The layout aliases live here with the passes that use them. They are all
// Vk::ReflectedLayout today, kept as distinct names because the shader each one
// reflects is distinct and the names are what the pass headers read.
//
// Build and hot-reload stay in init/RenderInitPostProcess.cpp: they go through
// BuildPassHelper, which needs the Impl for its device, heap, shader-loading and
// pipeline-binary state. This struct is the ownership half only.

#pragma once

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
