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

        const Vk::ImageWrite inDepth = mip == 0 ? Vk::ImageWrite {impl.presenter.depthTarget.view} :
                                                  Vk::ImageWrite {impl.graphResources.hizMap.mipViews[mip - 1], VK_IMAGE_LAYOUT_GENERAL};
        const Vk::HeapBlockBase block = impl.heapManager.WriteHeapParameters<Shaders::Hiz>(
            impl.ctx, impl.hizHeapBindings, Vk::Slot<"inDepth">(inDepth),
            Vk::Slot<"outDepth">(impl.graphResources.hizMap.mipViews[mip])
        );
        impl.hizGeneratePass.DispatchHeapIndexedThreads<Shaders::Modules::HizGenerateCS>(impl.ctx, cmd, block, dstW, dstH, 1, hizPC);
    }
}

}
