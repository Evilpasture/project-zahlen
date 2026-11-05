// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderFrame.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN::Passes {

// Generates the linear mip pyramid on `Res_LightingGI` right after `RtgiCompositePass`
// and `EmissivePass` finish writing mip 0, enabling deterministic Tier 2 cone-traced
// mip selection (`0.20 <= roughness <= 0.40`) in `ReflectionPass` with zero temporal noise.
struct LightingMipPass: VkD3D::RenderPass<"LightingMip", VkD3D::TransferDstWriteThenShaderRead<Res_LightingGI>> {
    RenderFrame& frame;

    void operator()(VkCommandBuffer cmd) const noexcept {
        const auto& dst = frame.graphResources.lightingGI;
        if (dst.mipLevels > 1) {
            Vk::GenerateMipmaps(cmd, dst.image.Handle(), dst.extent.width, dst.extent.height);
        } else {
            dst.View().Mip(0).Transition<Vk::ImageLayout::TransferDstOptimal, Vk::ImageLayout::ShaderReadOnlyOptimal>(cmd);
        }
    }
};

} // namespace ZHLN::Passes
