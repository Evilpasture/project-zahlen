// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "OpenGLHacks/HostBlit.hpp"
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <utility>

namespace ZHLN {

auto RenderContext::Impl::ReconcileDestination(FrameDestinations::Window& dest, Vk::CommandRecorder& recorder) noexcept
    -> std::expected<Vk::AttachmentLayout, ErrorCode> {
    if (!dest.acquired) {
        return std::unexpected(DestinationError::ExpiredFrameTarget);
    }
    auto& image = *dest.acquired;
    if (image.drawn) {
        return image.layout;
    }

    const VkCommandBuffer cmd = recorder.Handle();
    Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, image.image.Handle());
    Vk::DynamicPass(image.image.Extent2D())
        .ClearColor(image.image.Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(), kClearColorScene)
        .Execute(cmd, [] {});
    image.layout = Vk::AttachmentLayout::ColorAttachment;

    if (!diagnostics.warnedUnwrittenTarget) {
        ZHLN::Log(
            "[Render] Acquired window target (extent {}x{}) was not written this frame; presenting the background instead.",
            image.image.extent.width, image.image.extent.height
        );
        diagnostics.warnedUnwrittenTarget = true;
    }
    return image.layout;
}

auto RenderContext::Impl::PresentUsedWindows() noexcept -> FrameOutcome<PresentSuboptimal> {
    std::optional<PresentSuboptimal> result {};
    std::optional<ErrorCode> firstError {};

    for (auto& dest: destinations.Windows()) {
        if (!dest.acquired) {
            continue;
        }
        Vk::SwapchainPresenter& destPresenter = dest.Presenter();
        auto recording = destinations.FindRecording(dest.id);
        if (!recording || !recording->recorder.IsRecording()) {
            ZHLN::Log("[Render] Window target has no command stream to present.");
            dest.acquired.reset();
            destPresenter.AdvanceFrame();
            continue;
        }
        const auto reconciled = ReconcileDestination(dest, recording->recorder);
        if (!reconciled) {
            ZHLN::Log("[Render] Window target cannot be presented: {}.", reconciled.error());
            dest.acquired.reset();
            destPresenter.AdvanceFrame();
            continue;
        }

        const bool presents = destPresenter.HasSwapchain();

        std::array<Vk::SemaphoreSubmit, 3> waits {};
        uint32_t waitCount = 0;
        const uint64_t stagingValue = transferRingBuffer.GetCurrentValue();
        if (transferRingBuffer.GetSemaphore() != VK_NULL_HANDLE && stagingValue > 0) {
            waits[waitCount++] = Vk::SemaphoreSubmit::Timeline(transferRingBuffer.GetSemaphore(), stagingValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        }

        // Tell-Don't-Ask: ask FrameSync for a ready-made compute dependency
        if (frameState.computeSubmitted) {
            if (auto dep = presenter.sync.GetComputeWaitDependency(presenter.frameIndex, Vk::kAsyncComputeConsumerStages)) {
                waits[waitCount++] = *dep;
            }
        }

        const VkImageLayout currentLayout = Vk::ToVkImageLayout(*reconciled);
        destPresenter.PreparePresent(recording->recorder, dest.acquired->imageIndex, currentLayout);
        auto executable = std::move(recording->recorder).End();
        if (!executable) {
            if (Vk::IsDeviceLost(executable.error())) {
                ctx.Instance().IncrementDeviceLost();
                return std::unexpected(AsFrameError(executable.error()));
            }
            if (!firstError) { firstError = AsFrameError(executable.error()); }
            dest.acquired.reset();
            destPresenter.AdvanceFrame();
            continue;
        }
        auto presented = destPresenter.Present(
            ctx.GraphicsQueue(), ctx.PresentQueue(), std::move(*executable), dest.acquired->imageIndex,
            std::span<const Vk::SemaphoreSubmit> {waits.data(), waitCount}
        );
        if (!presented) {
            if (Vk::IsDeviceLost(presented.error())) {
                ctx.Instance().IncrementDeviceLost();
                return std::unexpected(AsFrameError(presented.error()));
            }
            if (!firstError) {
                firstError = presented.error();
            }
            ZHLN::Log("[Render] Present for window {:p} failed ({}); presenting other windows.", static_cast<const void*>(dest.target), presented.error());
            dest.acquired.reset();
            destPresenter.AdvanceFrame();
            continue;
        }

        if constexpr (isMac) {
            if (!presents && dest.IsPrimary() && presentationMode == PresentationMode::HostBlit && dest.target != nullptr) {
                auto& blitTarget = destPresenter.headlessColorTarget;
                if (blitTarget.Valid()) {
                    if (!HostBlit::Present(
                            blitTarget.image, nullptr, blitTarget.extent.width, blitTarget.extent.height, Vk::kHeadlessColorFormat,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                        )) {
                        dest.target->Close();
                    }
                }
            }
        }

        if (presented->has_value()) {
            const Extent2D size = dest.target != nullptr ? dest.target->GetFramebufferExtent() : Extent2D {};
            if (size.width != 0 && size.height != 0) {
                if (!destPresenter.Rebuild(size.width, size.height)) {
                    ZHLN::Log("[Render] Window rebuild after present failed; retrying next frame.");
                }
            }

            dest.acquired.reset();
            dest.cachedGeneration = destPresenter.resourceGeneration;
            result = PresentSuboptimal {};
        }

        destPresenter.AdvanceFrame();
    }

    if (firstError) {
        return std::unexpected(*firstError);
    }
    return result;
}

}
