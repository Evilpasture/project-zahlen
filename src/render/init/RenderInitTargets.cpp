// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../RenderInternal.hpp"
#include <Zahlen/Error.hpp>
#include <array>

namespace ZHLN {

void ApplyImageDebugNames(RenderContext::Impl& impl) noexcept {
    const auto& ctx = impl.ctx;

    impl.targets.NameGraphTargets();

    Vk::Debug::SetImageName(ctx, impl.frames.accumBuffers[0].image.Handle(), "AccumHistory0");
    Vk::Debug::SetImageName(ctx, impl.frames.accumBuffers[1].image.Handle(), "AccumHistory1");
    Vk::Debug::SetImageName(ctx, impl.presenter.depthTarget.image.Handle(), "DepthTarget");
    Vk::Debug::SetImageName(ctx, impl.targets.ShadowMapPrev().image.Handle(), "ShadowMapPrev");
    Vk::Debug::SetImageName(ctx, impl.iblPayload.brdfLutImage.Handle(), "IBL.BrdfLut");
    Vk::Debug::SetImageName(ctx, impl.iblPayload.prefilteredImage.Handle(), "IBL.PrefilteredCube");
    Vk::Debug::SetImageName(ctx, impl.ltcMatImage.Handle(), "LTC.Mat");
    Vk::Debug::SetImageName(ctx, impl.ltcAmpImage.Handle(), "LTC.Amp");

    impl.textureManager.NameSlots();

    const auto& swapchain = impl.presenter.swapchain.Get();
    for (uint32_t i = 0; i < swapchain.image_count; ++i) {
        Vk::Debug::SetImageName(ctx, swapchain.images[i], std::format("Swapchain{}", i));
    }
}

std::expected<void, ErrorCode> RenderContext::Impl::RecreateTargets(VkExtent2D ext) {
    if (!presenter.Rebuild(ext.width, ext.height)) {
        return std::unexpected(Vk::PresentationError::SwapchainCreationFailed);
    }

    const VkExtent3D voxelExt = fog.VoxelDispatchExtent();
    if (voxelExt.width == 0 || voxelExt.height == 0 || voxelExt.depth == 0) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    auto assign = [&](auto& member, auto e) -> std::expected<void, ErrorCode> {
        if (!e) {
            return std::unexpected(e.error());
        }
        member = std::move(*e);
        return {};
    };

    std::expected<void, ErrorCode> result {};
    result = assign(frames.accumBuffers[0], CreateColorTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(allocator, ctx, ext, Vk::ImageUsage::TransferDst));
    if (result) {
        result = assign(frames.accumBuffers[1], CreateColorTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(allocator, ctx, ext, Vk::ImageUsage::TransferDst));
    }
    if (!result) {
        return result;
    }

    if (auto targets_res = targets.Recreate(ext, voxelExt); !targets_res) {
        return targets_res;
    }

    Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
        targets.RecordInitialLayouts(cmd);

        const VkClearColorValue       clearBlack = {.float32 = {0.0F, 0.0F, 0.0F, 0.0F}};
        const VkImageSubresourceRange clearRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount     = VK_REMAINING_ARRAY_LAYERS
        };
        const std::array accumImages = {frames.accumBuffers[0].image.Handle(), frames.accumBuffers[1].image.Handle()};
        for (const auto img: accumImages) {
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
            vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearBlack, 1, &clearRange);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
        }

        if (decalDepthSlot.Valid()) {
            const auto info = Vk::MakeViewCreateInfo2D(presenter.depthTarget.image.Handle(), VK_FORMAT_D32_SFLOAT_S8_UINT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
            heapManager.WriteImage(decalDepthSlot, info, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>(
            cmd, presenter.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
        );
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
            cmd, presenter.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
        );
    });

    WriteTransLightingToHeap();


    ApplyImageDebugNames(*this);
    return {};
}

}
