// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderTargetTags.hpp"
#include "graph/RenderPassDecl.hpp"

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

struct SsrHiZGeneratePass: Vk::RenderPass<"SsrHiZGenerate", Vk::ShaderRead<Res_Depth>, Vk::ComputeWrite<Res_HiZ>> {
    CullingResources&   cull;
    RenderFrame&        frame;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
