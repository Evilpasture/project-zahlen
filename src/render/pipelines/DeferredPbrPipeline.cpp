// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/pipelines/DeferredPbrPipeline.cpp
//
// The deferred PBR recipe: everything the frame graph needs uploaded first,
// then one DispatchAAMode-selected graph. The pass list itself lives in
// RenderGraphBuilder.cpp next to the pass factories it builds from; this file
// is the entry point the facade calls, and the place where per-frame scene
// state is bound to a view.

#include "DeferredPbrPipeline.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Pipelines {

void DeferredPbrPipeline::Execute(RenderContext::Impl& impl, VkCommandBuffer cmd, const SceneView& view, const GraphicsSettings& settings) noexcept {
    if (cmd == VK_NULL_HANDLE) {
        ZHLN::Log("[RenderScene] No command buffer is open for this frame; scene skipped.");
        return;
    }

    // 1. Skinning, draw sorting, instance upload and the TLAS build: CPU-side
    //    work the graph's passes read. This also adopts the view's optics, so a
    //    second view in the same frame pushes its own matrices.
    impl.PrepareSceneFrame(cmd, view);

    // 2. The graph: Fork(shadow, g-buffer) -> HiZ -> ... -> AA/bloom tail,
    //    selected at compile time by the anti-aliasing mode.
    impl.RecordSceneFrame({cmd}, view, settings);
}

} // namespace ZHLN::Pipelines
