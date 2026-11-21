// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/VulkanHeader.hpp"

#include "render/RenderInternal.hpp"
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <render/Render.hpp>
#include <Zahlen/Render/View.hpp>

namespace ZHLN::Pipelines {

struct DeferredPbrPipeline {
    static void Execute(RenderContext::Impl& impl, VkCommandBuffer cmd, const SceneView& view, const GraphicsSettings& settings) noexcept;
};

} // namespace ZHLN::Pipelines
