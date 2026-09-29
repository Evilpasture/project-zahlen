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

auto RenderContext::Impl::ReconcileDestination(FrameDestinations::Window& dest) noexcept -> FrameOutcome<Vk::AttachmentLayout> {
    if (!dest.acquired) {
        return std::unexpected(DestinationError::ExpiredFrameTarget);
    }
    auto& image = *dest.acquired;
    if (image.drawn) {
        return image.layout;
    }
    if (!dest.recorder.IsRecording()) {
        return std::nullopt;
    }

    const VkClearColorValue clear {
        .float32 = {kClearColorScene.r, kClearColorScene.g, kClearColorScene.b, kClearColorScene.a},
    };
    Vk::ClearColorImage(dest.recorder.Handle(), image.image.Handle(), clear);
    image.layout = Vk::AttachmentLayout::ColorAttachment;

    if (!warnedUnwrittenTarget) {
        ZHLN::Log(
            "[Render] Acquired window target (extent {}x{}) was not written this frame; presenting the background instead.",
            image.image.extent.width, image.image.extent.height
        );
        warnedUnwrittenTarget = true;
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

        const auto reconciled = ReconcileDestination(dest);
        if (!reconciled) {
            ZHLN::Log("[Render] Window target cannot be presented: {}.", reconciled.error());
            dest.acquired.reset();
            destPresenter.AdvanceFrame();
            continue;
        }
        if (!reconciled->has_value()) {
            ZHLN::Log("[Render] Window target has no command stream to present.");
            dest.acquired.reset();
            destPresenter.AdvanceFrame();
            continue;
        }

        const bool presents = destPresenter.HasSwapchain();

        std::array<VkSemaphoreSubmitInfo, 3> waits {};
        uint32_t waitCount = 0;
        const uint64_t stagingValue = transferRingBuffer.GetCurrentValue();
        if (transferRingBuffer.GetSemaphore() != VK_NULL_HANDLE && stagingValue > 0) {
            waits[waitCount++] = Vk::MakeSemaphoreSubmitInfo(transferRingBuffer.GetSemaphore(), stagingValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        }
        // Simulations signal the primary presenter's timeline, regardless of
        // which window consumes their results.
        const uint64_t computeValue = presenter.sync.GetTimelineValue(presenter.frameIndex);
        const VkSemaphore computeTimeline = presenter.sync.ComputeTimeline(presenter.frameIndex);
        if (computeTimeline != VK_NULL_HANDLE && computeValue > 0 && frameState.computeSubmitted) {
            waits[waitCount++] = Vk::MakeSemaphoreSubmitInfo(computeTimeline, computeValue, Vk::kAsyncComputeConsumerStages);
        }

        const VkImageLayout currentLayout = Vk::ToVkImageLayout(**reconciled);
        destPresenter.PreparePresent(dest.recorder, dest.acquired->imageIndex, currentLayout);
        auto executable = std::move(dest.recorder).End();
        if (!executable) {
            if (executable.error().Is(FrameResult::DeviceLost)) {
                Vk::Instance::IncrementNumericalDeviceLoss();
                return std::unexpected(executable.error());
            }
            if (!firstError) { firstError = executable.error(); }
            dest.acquired.reset();
            destPresenter.AdvanceFrame();
            continue;
        }
        auto presented = destPresenter.Present(
            ctx.GraphicsQueue(), ctx.PresentQueue(), std::move(*executable), dest.acquired->imageIndex,
            std::span<const VkSemaphoreSubmitInfo> {waits.data(), waitCount}
        );
        if (!presented) {
            if (presented.error().Is(FrameResult::DeviceLost)) {
                Vk::Instance::IncrementNumericalDeviceLoss();
                return std::unexpected(presented.error());
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
            // A rebuild retires the borrowed ImageSlice immediately; even
            // headless screenshots must not inspect it after this point.
            dest.acquired.reset();
            dest.cachedGeneration = destPresenter.resourceGeneration;
            result = PresentSuboptimal {};
        }
        // For headless captures, keep the last successfully presented image
        // and its drawn bit until the next BeginFrame discards that frame state.
        destPresenter.AdvanceFrame();
    }

    if (firstError) {
        return std::unexpected(*firstError);
    }
    return result;
}

}
