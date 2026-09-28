// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "RenderInternal.hpp"
#include "OpenGLHacks/HostBlit.hpp"
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdint>
#include <span>

namespace ZHLN {

auto RenderContext::Impl::ReconcileDestination(DestinationRegistry::WindowEntry& dest) noexcept -> FrameOutcome<ReconcileReceipt> {
    if (dest.imageIndex >= dest.recordHandles.size()) {
        return std::unexpected(DestinationError::SlotRetired);
    }
    const DestinationRegistry::Handle handle = dest.recordHandles[dest.imageIndex];
    if (!handle.Valid() || handle.Index() >= destinations.Records().size()) {
        return std::unexpected(DestinationError::SlotRetired);
    }
    DestinationRegistry::Record& record = destinations.Records()[handle.Index()];

    const auto receipt = record.GetRenderedContent();
    if (!receipt) {
        return std::unexpected(receipt.error());
    }
    if (receipt->has_value()) {
        return ReconcileReceipt {.rendered = **receipt, .layout = record.trackedLayout};
    }

    if (!dest.recording.IsOpen()) {
        return std::nullopt;
    }

    const VkClearColorValue clear {
        .float32 = {kClearColorScene.r, kClearColorScene.g, kClearColorScene.b, kClearColorScene.a},
    };
    Vk::ClearColorImage(dest.recording.Command(), record.image.Handle(), clear);
    record.trackedLayout = Vk::AttachmentLayout::ColorAttachment;
    record.content       = DestinationRegistry::Rendered {.by = DestinationRegistry::Rendered::By::FrameFill};

    if (!destinations.UnwrittenWarned()) {
        ZHLN::Log(
            "[Render] Destination 0x{:016X} (extent {}x{}) was vended but no pass wrote it this frame; the frame's background is presented in "
            "its place.",
            record.handle.Raw(), record.image.extent.width, record.image.extent.height
        );
        destinations.NoteUnwrittenWarned();
    }
    return ReconcileReceipt {.rendered = *record.content, .layout = record.trackedLayout};
}

auto RenderContext::Impl::PresentUsedWindows() noexcept -> FrameOutcome<PresentSuboptimal> {
    std::optional<PresentSuboptimal> result {};
    std::optional<ErrorCode> firstError {};

    for (auto& dest: destinations.Windows()) {
        if (!dest.imageAcquired) {
            continue;
        }

        Vk::SwapchainPresenter& destPresenter = dest.Presenter();

        const auto reconciled = ReconcileDestination(dest);
        if (!reconciled) {
            ZHLN::Log(
                "[Render] Destination for window {:p} has no image left to present ({}); the frame does not present it.",
                static_cast<const void*>(dest.target), reconciled.error()
            );
            dest.imageAcquired = false;
            destPresenter.AdvanceFrame();
            continue;
        }
        if (!reconciled->has_value()) {
            ZHLN::Log(
                "[Render] Destination for window {:p} has no stream to close it with (rebuilt under the frame); the frame does not present it.",
                static_cast<const void*>(dest.target)
            );
            dest.imageAcquired = false;
            destPresenter.AdvanceFrame();
            continue;
        }

        const bool     presents = destPresenter.HasSwapchain();
        const uint32_t slot     = destPresenter.frameIndex;

        std::array<VkSemaphoreSubmitInfo, 3> waits {};
        uint32_t                             waitCount = 0;
        const uint64_t                       stagingValue = transferRingBuffer.GetCurrentValue();
        if (transferRingBuffer.GetSemaphore() != VK_NULL_HANDLE && stagingValue > 0) {
            waits[waitCount++] =
                Vk::MakeSemaphoreSubmitInfo(transferRingBuffer.GetSemaphore(), stagingValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        }
        const uint64_t    computeValue    = destPresenter.sync.GetTimelineValue(slot);
        const VkSemaphore computeTimeline = destPresenter.sync.ComputeTimeline(slot);
        if (computeTimeline != VK_NULL_HANDLE && computeValue > 0 && frameState.computeSubmitted) {
            waits[waitCount++] = Vk::MakeSemaphoreSubmitInfo(computeTimeline, computeValue, Vk::kAsyncComputeConsumerStages);
        }

        const ReconcileReceipt& receipt       = **reconciled;
        const VkImageLayout     currentLayout = Vk::ToVkImageLayout(receipt.layout);

        auto presented = destPresenter.Present(
            ctx.GraphicsQueue(), ctx.PresentQueue(), dest.recording.Command(), dest.imageIndex, currentLayout,
            std::span<const VkSemaphoreSubmitInfo> {waits.data(), waitCount}
        );
        dest.recording.Discard();
        if (!presented) {
            if (presented.error().Is(FrameResult::DeviceLost)) {
                Vk::Instance::IncrementNumericalDeviceLoss();
                return std::unexpected(presented.error());
            }
            if (!firstError) {
                firstError = presented.error();
            }
            ZHLN::Log(
                "[Render] Present for window {:p} failed ({}); the frame presents its other windows and reports the error at the end.",
                static_cast<const void*>(dest.target), presented.error()
            );
            dest.imageAcquired = false;
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
                    ZHLN::Log("[Render] Destination rebuild after present failed; retrying next frame.");
                }
            }
            destinations.Retire(dest.target);
            dest.recordHandles.clear();
            dest.cachedGeneration = destPresenter.resourceGeneration;
            result = PresentSuboptimal {};
        }

        dest.imageAcquired = false;
        destPresenter.AdvanceFrame();
    }

    if (firstError) {
        return std::unexpected(*firstError);
    }
    return result;
}

}
