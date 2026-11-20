// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/passes/culling/HiZGeneratePass.hpp"
#include "render/RenderFrame.hpp"
#include "render/Renderer.hpp"
#include "render/passes/culling/CullingResources.hpp"
#include <ShaderBindings.hpp>
#include <algorithm>

namespace ZHLN::Passes {

using HiZPushConstants = Slang::HiZPushConstants;
static_assert(GpuAbi::ScenePassPayload<HiZPushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

namespace {

template <typename Target>
void GenerateHiZPyramid(VkCommandBuffer cmd, CullingResources& cull, RenderFrame& frame, Target& target, uint32_t maxMips) noexcept {
    const uint32_t width  = target.extent.width;
    const uint32_t height = target.extent.height;
    const uint32_t mips   = std::min(target.mipLevels, maxMips);

    for (uint32_t mip = 0; mip < mips; ++mip) {
        if (mip > 0) {
            Vk::MemoryBarrier::ComputeWriteToRead().Record(cmd);
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
            .reduceMin    = 0u,
        };

        const auto inDepth = mip == 0 ?
                                 Vk::ImageWrite {frame.ActiveDepthTarget().image.View()} :
                                 Vk::ImageWrite {target.image.View(VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1), VK_IMAGE_LAYOUT_GENERAL};
        const auto block   = frame.renderer.heapManager.WriteHeapParameters<Shaders::Hiz>(
            cull.hizHeapBindings, Vk::Slot<"inDepth">(inDepth), Vk::Slot<"outDepth">(target.image.View(VK_IMAGE_ASPECT_COLOR_BIT, mip, 1))
        );
        cull.hizGeneratePass.DispatchHeapIndexedThreads<Shaders::Modules::HizGenerateCS>(cmd, block, dstW, dstH, 1, hizPC);
    }
}

} // namespace

void HiZGeneratePass::operator()(VkCommandBuffer cmd) const noexcept {
    GenerateHiZPyramid(cmd, cull, frame, frame.graphResources.hizMap, kMaxGeneratedHiZMips);
}

void AoDepthGeneratePass::operator()(VkCommandBuffer cmd) const noexcept {
    const int giMode = frame.settings.post.mode;
    if (giMode == 3 || giMode == 4) {
        // GTAO samples at most mip 3; leave the remaining levels unused.
        GenerateHiZPyramid(cmd, cull, frame, frame.graphResources.aoDepth, 4u);
    }
}

} // namespace ZHLN::Passes
