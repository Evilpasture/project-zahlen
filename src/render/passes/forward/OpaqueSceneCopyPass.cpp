// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/forward/OpaqueSceneCopyPass.hpp"

namespace ZHLN::Passes {

void OpaqueSceneCopyPass::operator()(VkCommandBuffer cmd) const noexcept {
    const auto& src = impl.graphResources.hdrSceneColor;
    const auto& dst = impl.graphResources.transLightingTarget;

    VkImageCopy region {};
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.extent                    = {src.extent.width, src.extent.height, 1};
    vkCmdCopyImage(cmd, src.image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image.Handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // The graph transitions the whole target to transfer-dst on entry. This
    // blits each level from the preceding one and transitions *all* mips to
    // shader-read on exit; the pass usage declares that exit state to the graph.
    if (dst.mipLevels > 1) {
        Vk::GenerateMipmaps(cmd, dst.image.Handle(), dst.extent.width, dst.extent.height);
    } else {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
            cmd, dst.image.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
        );
    }
}

}
