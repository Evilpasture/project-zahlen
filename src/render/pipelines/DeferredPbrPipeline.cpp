// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "DeferredPbrPipeline.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Pipelines {

void DeferredPbrPipeline::Execute(RenderContext::Impl& impl, VkCommandBuffer cmd, const SceneView& view, const GraphicsSettings& settings) noexcept {
    if (cmd == VK_NULL_HANDLE) {
        ZHLN::Log("[RenderScene] No command buffer is open for this frame; scene skipped.");
        return;
    }

    impl.PrepareSceneFrame(cmd, view);

    impl.RecordSceneFrame({cmd}, view, settings);
}

}
