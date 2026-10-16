// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/culling/HiZGeneratePass.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>

namespace ZHLN::Passes {

struct HiZPushConstants {
    float    rcpSrcWidth;
    float    rcpSrcHeight;
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t isFirstPass;
};
static_assert(GpuAbi::ScenePassPayload<HiZPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void HiZGeneratePass::operator()(VkCommandBuffer cmd) const noexcept {
    using enum Vk::BarrierStage;
    using enum Vk::BarrierAccess;
    const uint32_t width  = frame.graphResources.hizMap.extent.width;
    const uint32_t height = frame.graphResources.hizMap.extent.height;
    const uint32_t mips   = std::min(frame.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);

    for (uint32_t mip = 0; mip < mips; ++mip) {
        if (mip > 0) {
            Vk::MemoryBarrier(cmd, Compute, ShaderWrite, Compute, ShaderRead);
        }

        const uint32_t srcW = std::max(1u, width >> (mip == 0 ? 0 : mip - 1));
        const uint32_t srcH = std::max(1u, height >> (mip == 0 ? 0 : mip - 1));
        const uint32_t dstW = std::max(1u, width >> mip);
        const uint32_t dstH = std::max(1u, height >> mip);

        const HiZPushConstants hizPC = {
            .rcpSrcWidth  = 1.0f / static_cast<float>(srcW),
            .rcpSrcHeight = 1.0f / static_cast<float>(srcH),
            .srcWidth     = srcW,
            .srcHeight    = srcH,
            .isFirstPass  = mip == 0 ? 1u : 0u
        };

        const auto inDepth = mip == 0 ?
                                 Vk::ImageWrite {frame.presenter.depthTarget.image.ViewInfo()} :
                                 Vk::ImageWrite {frame.graphResources.hizMap.image.ViewInfo(VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1), VK_IMAGE_LAYOUT_GENERAL};
        const auto block   = frame.heapManager.WriteHeapParameters<Shaders::Hiz>(
            cull.hizHeapBindings, Vk::Slot<"inDepth">(inDepth),
            Vk::Slot<"outDepth">(frame.graphResources.hizMap.image.ViewInfo(VK_IMAGE_ASPECT_COLOR_BIT, mip, 1))
        );
        cull.hizGeneratePass.DispatchHeapIndexedThreads<Shaders::Modules::HizGenerateCS>(cmd, block, dstW, dstH, 1, hizPC);
    }
}

} // namespace ZHLN::Passes
