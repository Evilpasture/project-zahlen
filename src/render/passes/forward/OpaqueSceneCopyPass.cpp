// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/forward/OpaqueSceneCopyPass.hpp"
#include "RenderFrame.hpp"

namespace ZHLN::Passes {

void OpaqueSceneCopyPass::operator()(VkCommandBuffer cmd) const noexcept {
    const auto& src = frame.graphResources.hdrSceneColor;
    const auto& dst = frame.graphResources.transLightingTarget;

    const VkExtent3D extent {src.extent.width, src.extent.height, 1};

    Vk::CopyImage(cmd, src.image.Handle(), Vk::ImageLayout::TransferSrcOptimal, dst.image.Handle(), Vk::ImageLayout::TransferDstOptimal, extent);

    if (dst.mipLevels > 1) {
        Vk::GenerateMipmaps(cmd, dst.image.Handle(), dst.extent.width, dst.extent.height);
    } else {
        dst.View().Mip(0).Transition<Vk::ImageLayout::TransferDstOptimal, Vk::ImageLayout::ShaderReadOnlyOptimal>(cmd);
    }
}

} // namespace ZHLN::Passes
