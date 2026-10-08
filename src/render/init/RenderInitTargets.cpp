// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../RenderInternal.hpp"
#include <Zahlen/Error.hpp>

namespace ZHLN {

void ApplyImageDebugNames(RenderContext::Impl& impl) noexcept {
    const auto& ctx = impl.ctx;

    impl.targets.NameGraphTargets();

    auto* history = impl.accumulationHistory.begin();
    Vk::Debug::SetImageName(ctx, history[0].image.Handle(), "AccumHistory0");
    Vk::Debug::SetImageName(ctx, history[1].image.Handle(), "AccumHistory1");
    Vk::Debug::SetImageName(ctx, impl.presenter.depthTarget.image.Handle(), "DepthTarget");
    Vk::Debug::SetImageName(ctx, impl.targets.ShadowMapPrev().image.Handle(), "ShadowMapPrev");
    Vk::Debug::SetImageName(ctx, impl.iblPayload.brdfLutImage.Handle(), "IBL.BrdfLut");
    Vk::Debug::SetImageName(ctx, impl.iblPayload.prefilteredImage.Handle(), "IBL.PrefilteredCube");
    Vk::Debug::SetImageName(ctx, impl.ltcMatImage.Handle(), "LTC.Mat");
    Vk::Debug::SetImageName(ctx, impl.ltcAmpImage.Handle(), "LTC.Amp");

    impl.textureManager.NameSlots();

    const auto& swapchain = impl.presenter.swapchain.Get();
    for (uint32_t i = 0; i < swapchain.imageCount; ++i) {
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
        member.Destroy(allocator);
        member = std::move(*e);
        return {};
    };

    for (auto& history: accumulationHistory) {
        auto result = assign(history, CreateColorTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(allocator, ctx, ext, Vk::ImageUsage::TransferDst));
        if (!result) {
            return result;
        }
    }

    if (auto targets_res = targets.Recreate(ext, voxelExt); !targets_res) {
        return targets_res;
    }

    Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
        targets.RecordInitialLayouts(cmd);

        Vk::ClearColorAndTransition(cmd, Color4 {}, accumulationHistory.Current(), accumulationHistory.Previous());

        if (decalDepthSlot.Valid()) {
            heapManager.WriteImage(decalDepthSlot, presenter.depthTarget, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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

} // namespace ZHLN
