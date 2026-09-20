// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/pipelines/UIPipeline.hpp
//
// Private 2D pipeline recipe. Callers reach it only through
// `RenderContext::RenderUI`; no header in include/Zahlen names this type.

#pragma once
#include "../RenderInternal.hpp"
#include <Zahlen/Render.hpp>
#include <Zahlen/View.hpp>

namespace ZHLN::Pipelines {

// Draws a Clay geometry payload into a destination subresource.
//
// One dynamic pass, no depth, no scene state: the UI is a composition step
// over whatever the target already holds (LOAD) or, when the target was
// acquired this frame and has no defined contents yet, a clear (CLEAR).
struct UIPipeline {
    // Records the view's UI into the stream its target names: the pass resolves
    // the target through the registry and records into the destination that owns
    // it, so no caller has to know which command buffer is open.
    static void Execute(RenderContext::Impl& impl, const UIView& view, const UIDrawData& uiData) noexcept;
};

} // namespace ZHLN::Pipelines
