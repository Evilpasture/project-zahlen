// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/pipelines/DeferredPbrPipeline.hpp
//
// Private 3D pipeline recipe. Callers reach it only through
// `RenderContext::RenderScene`; no header in include/Zahlen names this type,
// and the pass list, the graph type and the internal targets stay private to
// src/render/.

#pragma once
#include "../RenderInternal.hpp"
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/View.hpp>

namespace ZHLN::Pipelines {

/// Uploads the scene state the graph reads (skinning, draw sort, instance
/// data, TLAS) and records the deferred frame graph for one view.
struct DeferredPbrPipeline {
    static void Execute(RenderContext::Impl& impl, VkCommandBuffer cmd, const SceneView& view, const GraphicsSettings& settings) noexcept;
};

} // namespace ZHLN::Pipelines
