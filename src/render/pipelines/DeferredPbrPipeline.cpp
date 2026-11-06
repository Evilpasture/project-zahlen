// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/pipelines/DeferredPbrPipeline.hpp"
#include "render/RenderInternal.hpp"

namespace ZHLN::Pipelines {

void DeferredPbrPipeline::Execute(RenderContext::Impl& impl, VkCommandBuffer cmd, const SceneView& view, const GraphicsSettings& settings) noexcept {
    impl.PrepareSceneFrame(cmd, view);

    impl.RecordSceneFrame({cmd}, view, settings);
}

} // namespace ZHLN::Pipelines
