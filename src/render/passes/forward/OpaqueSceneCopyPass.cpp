// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/forward/OpaqueSceneCopyPass.hpp"
#include "RenderFrame.hpp"

namespace ZHLN::Passes {

void OpaqueSceneCopyPass::operator()(VkCommandBuffer cmd) const noexcept {
    const auto& src = frame.graphResources.hdrSceneColor;
    const auto& dst = frame.graphResources.transLightingTarget;

    const VkExtent3D extent {src.extent.width, src.extent.height, 1};

    Vk::CopyImage(cmd, src.image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, extent);

    if (dst.mipLevels > 1) {
        Vk::GenerateMipmaps(cmd, dst.image.Handle(), dst.extent.width, dst.extent.height);
    } else {
        dst.View().Mip(0).Transition<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd);
    }
}

} // namespace ZHLN::Passes
