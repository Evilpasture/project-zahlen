// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "../RenderInternal.hpp"
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Render/View.hpp>

namespace ZHLN::Pipelines {

struct DeferredPbrPipeline {
    static void Execute(RenderContext::Impl& impl, VkCommandBuffer cmd, const SceneView& view, const GraphicsSettings& settings) noexcept;
};

}
