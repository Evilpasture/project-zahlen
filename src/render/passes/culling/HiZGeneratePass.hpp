// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/VulkanHeader.hpp"

#include "render/RenderTargetTags.hpp"
#include "vulkan/graph/RenderPassDecl.hpp"

namespace ZHLN {
struct CullingResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct HiZGeneratePass: Vk::RenderPass<"HiZGenerate", Vk::ShaderRead<Res_Depth>, Vk::ComputeWrite<Res_HiZ>> {
    CullingResources&   cull;
    RenderFrame&        frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

// Keep the culling pyramid intact; AO needs the depth after opaque resolve
// and viewmodel rendering, not the earlier occluder-only snapshot.
struct AoDepthGeneratePass: Vk::RenderPass<"AoDepthGenerate", Vk::FragmentAndComputeRead<Res_Depth>, Vk::ComputeWrite<Res_AoDepth>> {
    CullingResources& cull;
    RenderFrame&      frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
