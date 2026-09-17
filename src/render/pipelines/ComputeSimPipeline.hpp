// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/pipelines/ComputeSimPipeline.hpp
//
// Private compute recipe (cluster culling, volumetric fog, particle updates).
// Callers reach it only through `RenderContext::DispatchCompute`.

#pragma once
#include "../RenderInternal.hpp"
#include <Zahlen/Render.hpp>
#include <Zahlen/View.hpp>

namespace ZHLN::Pipelines {

/// Records this frame's compute simulations on the async compute queue and
/// submits them, signalling the frame's compute timeline. A frame that draws
/// only UI never pays for it.
struct ComputeSimPipeline {
    static void Submit(RenderContext::Impl& impl, float dt) noexcept;
};

} // namespace ZHLN::Pipelines
