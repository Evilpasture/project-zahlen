// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/culling/HiZGeneratePass.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>

namespace ZHLN::Passes {

// The reduce reads a 2x2 footprint of the source mip and writes one texel of
// the destination, so it needs both mips' sizes and nothing else.
struct HiZPushConstants {
    float    rcpSrcWidth;
    float    rcpSrcHeight;
    uint32_t srcWidth;
    uint32_t srcHeight;
    uint32_t isFirstPass;
};
static_assert(GpuAbi::ScenePassPayload<HiZPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void HiZGeneratePass::operator()(VkCommandBuffer cmd) const noexcept {
    const uint32_t width  = impl.graphResources.hizMap.extent.width;
    const uint32_t height = impl.graphResources.hizMap.extent.height;
    const uint32_t mips   = std::min(impl.graphResources.hizMap.mipLevels, kMaxGeneratedHiZMips);

    for (uint32_t mip = 0; mip < mips; ++mip) {
        if (mip > 0) {
            Vk::MemoryBarrier(cmd, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderWrite, Vk::BarrierStage::Compute, Vk::BarrierAccess::ShaderRead);
        }

        const uint32_t srcW = std::max(1u, width >> (mip == 0 ? 0 : mip - 1));
        const uint32_t srcH = std::max(1u, height >> (mip == 0 ? 0 : mip - 1));
        const uint32_t dstW = std::max(1u, width >> mip);
        const uint32_t dstH = std::max(1u, height >> mip);

        const HiZPushConstants hizPC = {
            1.0f / static_cast<float>(srcW), 1.0f / static_cast<float>(srcH), srcW, srcH, mip == 0 ? 1u : 0u
        };

        const Vk::TypedImage<VK_IMAGE_LAYOUT_GENERAL> outMip {
            .handle   = impl.graphResources.hizMap.image.Handle(),
            .view     = impl.graphResources.hizMap.mipViews[mip].Get(),
            .extent   = {.width = impl.graphResources.hizMap.extent.width, .height = impl.graphResources.hizMap.extent.height, .depth = 1},
            .aspect   = VK_IMAGE_ASPECT_COLOR_BIT,
            .format   = VK_FORMAT_R32_SFLOAT,
            .viewInfo = &impl.graphResources.hizMap.mipViewInfos[mip]
        };
        const Vk::ImageWrite inDepth = mip == 0 ? Vk::ImageWrite {
                                                      .view     = impl.presenter.depthTarget.view.Get(),
                                                      .layout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                      .viewInfo = &impl.presenter.depthTarget.viewInfo
                                                  } :
                                                  Vk::ImageWrite {
                                                      .view     = impl.graphResources.hizMap.mipViews[mip - 1].Get(),
                                                      .layout   = VK_IMAGE_LAYOUT_GENERAL,
                                                      .viewInfo = &impl.graphResources.hizMap.mipViewInfos[mip - 1]
                                                  };
        const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::Hiz>(
            impl.ctx, impl.hizHeapBindings, Vk::Slot<"inDepth">(inDepth), Vk::Slot<"outDepth">(outMip)
        );
        impl.hizGeneratePass.DispatchHeapIndexedThreads<Shaders::Modules::HizGenerateCS>(impl.ctx, cmd, block, dstW, dstH, 1, hizPC);
    }
}

}
