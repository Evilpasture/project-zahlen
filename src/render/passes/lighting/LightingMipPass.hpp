// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "FrameContext.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN::Passes {

// Generates the linear mip pyramid on `Res_LightingGI` right after `RtgiCompositePass`
// and `EmissivePass` finish writing mip 0, enabling deterministic Tier 2 cone-traced
// mip selection (`0.20 <= roughness <= 0.40`) in `ReflectionPass` with zero temporal noise.
struct LightingMipPass: Vk::RenderPass<"LightingMip", Vk::TransferDstWriteThenShaderRead<Res_LightingGI>> {
    RenderFrameContext& frame;

    void operator()(VkCommandBuffer cmd) const noexcept {
        const auto& dst = frame.graphResources.lightingGI;
        if (dst.mipLevels > 1) {
            Vk::GenerateMipmaps(cmd, dst.image.Handle(), dst.extent.width, dst.extent.height);
        } else {
            dst.View().Mip(0).Transition<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd);
        }
    }
};

} // namespace ZHLN::Passes
