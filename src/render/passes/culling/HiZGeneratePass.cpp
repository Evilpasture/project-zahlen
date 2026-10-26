// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/culling/HiZGeneratePass.hpp"
#include "FrameContext.hpp"
#include "passes/culling/CullingResources.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>

namespace ZHLN::Passes {

using HiZPushConstants = Slang::HiZPushConstants;
static_assert(GpuAbi::ScenePassPayload<HiZPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

namespace {

void GenerateHiZPyramid(VkCommandBuffer cmd, CullingResources& cull, RenderFrameContext& frame, uint32_t reduceMin) noexcept {
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
            .isFirstPass  = mip == 0 ? 1u : 0u,
            .reduceMin    = reduceMin,
        };

        const auto inDepth = mip == 0 ?
                                 Vk::ImageWrite {frame.ActiveDepthTarget().image.View()} :
                                 Vk::ImageWrite {frame.graphResources.hizMap.image.View(VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1), VK_IMAGE_LAYOUT_GENERAL};
        const auto block   = frame.heapManager.WriteHeapParameters<Shaders::Hiz>(
            cull.hizHeapBindings, Vk::Slot<"inDepth">(inDepth),
            Vk::Slot<"outDepth">(frame.graphResources.hizMap.image.View(VK_IMAGE_ASPECT_COLOR_BIT, mip, 1))
        );
        cull.hizGeneratePass.DispatchHeapIndexedThreads<Shaders::Modules::HizGenerateCS>(cmd, block, dstW, dstH, 1, hizPC);
    }
}

} // namespace

void HiZGeneratePass::operator()(VkCommandBuffer cmd) const noexcept {
    GenerateHiZPyramid(cmd, cull, frame, 0u);
}

void SsrHiZGeneratePass::operator()(VkCommandBuffer cmd) const noexcept {
    if (frame.settings.post.enableSSR == 0 || frame.settings.environment.fullBright != 0) {
        return;
    }
    GenerateHiZPyramid(cmd, cull, frame, 1u);
}

} // namespace ZHLN::Passes
