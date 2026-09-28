// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "DeferredPbrPipeline.hpp"

namespace ZHLN::Pipelines {

void DeferredPbrPipeline::Execute(RenderContext::Impl& impl, VkCommandBuffer cmd, const SceneView& view, const GraphicsSettings& settings) noexcept {
    impl.PrepareSceneFrame(cmd, view);

    impl.RecordSceneFrame({cmd}, view, settings);
}

}
