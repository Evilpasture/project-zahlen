// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/init/RenderInitTargets.cpp
#include "../RenderInternal.hpp"
#include <Zahlen/Error.hpp>
#include <array>

namespace ZHLN {

void ApplyImageDebugNames(RenderContext::Impl& impl) noexcept {
    const auto& ctx = impl.ctx;

    // The graph targets name themselves; what follows is the imagery that lives
    // outside that bundle and belongs to the render context.
    impl.targets.NameGraphTargets();

    Vk::Debug::SetImageName(ctx, impl.frames.accumBuffers[0].image.Handle(), "AccumHistory0");
    Vk::Debug::SetImageName(ctx, impl.frames.accumBuffers[1].image.Handle(), "AccumHistory1");
    Vk::Debug::SetImageName(ctx, impl.presenter.depthTarget.image.Handle(), "DepthTarget");
    Vk::Debug::SetImageName(ctx, impl.targets.ShadowMapPrev().image.Handle(), "ShadowMapPrev");
    Vk::Debug::SetImageName(ctx, impl.iblPayload.brdfLutImage.Handle(), "IBL.BrdfLut");
    Vk::Debug::SetImageName(ctx, impl.iblPayload.prefilteredImage.Handle(), "IBL.PrefilteredCube");
    Vk::Debug::SetImageName(ctx, impl.ltcMatImage.Handle(), "LTC.Mat");
    Vk::Debug::SetImageName(ctx, impl.ltcAmpImage.Handle(), "LTC.Amp");

    // The bindless slots are the texture manager's to label; it skips the ones
    // released and awaiting reclamation, which hold no image.
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

    const auto& voxelDispatch = volumetricClearPass.fixedDispatchSize;
    if (voxelDispatch[0] == 0 || voxelDispatch[1] == 0 || voxelDispatch[2] == 0) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    const VkExtent3D voxelExt = {.width = voxelDispatch[0], .height = voxelDispatch[1], .depth = voxelDispatch[2]};

    auto assign = [&](auto& member, auto e) -> std::expected<void, ErrorCode> {
        if (!e) {
            return std::unexpected(e.error());
        }
        member = std::move(*e);
        return {};
    };

    // The frame's accumulation history is double-buffered frame state rather
    // than a graph target, so it is allocated here and not by TargetManager --
    // but with the same helper, and in the same order it always was: before the
    // reflected bundle, so a failure here short-circuits the rest.
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

    // One immediate submission for the whole resize. The graph targets'
    // transitions are recorded by the manager into this command buffer rather
    // than submitted by it, because the accumulation-history clear and the
    // presentation depth transition belong to the same event: splitting them
    // into separate submits would add a device wait to every resize.
    Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
        targets.RecordInitialLayouts(cmd);

        // TAA samples AccumCurr on frame 0, so the history has to start as
        // zeroes rather than VRAM garbage.
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

        // VK_EXT_descriptor_heap: the decal pass samples the depth target
        // through the heap, so rewrite its descriptor whenever the target is
        // recreated (the old view was destroyed). Depth/stencil sampled-image
        // descriptors must select exactly one aspect (VUID-VkImageDescriptorInfoEXT-pView-11430);
        // decal.slang only reads the depth value.
        if (decalDepthSlot.Valid()) {
            const auto info = Vk::MakeViewCreateInfo2D(presenter.depthTarget.image.Handle(), VK_FORMAT_D32_SFLOAT_S8_UINT, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
            heapManager.WriteImage(decalDepthSlot, info, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        // The presentation depth belongs to the swapchain presenter, not to the
        // graph bundle, so its transition stays here.
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL>(
            cmd, presenter.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
        );
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
            cmd, presenter.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
        );
    });

    WriteTransLightingToHeap();

    // The Hi-Z and culling descriptor blocks are written where those dispatches
    // are recorded (MakeHiZGeneratePass / CullingPass), from the frame's
    // transient partition.

    ApplyImageDebugNames(*this);
    return {};
}

} // namespace ZHLN
