// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/pipelines/ComputeSimPipeline.hpp
//
// Private compute recipe (cluster culling, volumetric fog, particle updates).
// Callers reach it only through `RenderContext::DispatchSimulations`.

#pragma once
#include "../RenderInternal.hpp"
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/View.hpp>

namespace ZHLN::Pipelines {

// Records this frame's compute simulations on the async compute queue and
// submits them, signalling the frame's compute timeline. A frame that draws
// only UI never pays for it.
struct ComputeSimPipeline {
    // The error slot carries a failed submit (a lost device among them) to the
    // frame's caller; success means the simulations are queued behind the
    // frame's compute timeline.
    [[nodiscard]] static auto Submit(RenderContext::Impl& impl, float dt) noexcept -> RenderResult;
};

} // namespace ZHLN::Pipelines
