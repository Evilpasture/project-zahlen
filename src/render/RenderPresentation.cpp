// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/RenderPresentation.cpp
//
// The end of a frame, for every window the frame was drawn into. Two jobs, both the
// renderer's because both are about the *frame* rather than the swapchain:
//
//   * a destination vended but never written is handed defined contents -- the background
//     colour -- rather than an image whose contents are undefined;
//   * the frame's submission is ordered behind the other queues it used (the transfer
//     ring's timeline, async compute's), which the RHI's presenter does not know and
//     should not want to.
//
// Everything Vulkan-specific below is naming: which semaphores the submission waits on,
// which layout the image was left in. The transition, submit and present are
// `Vk::SwapchainPresenter::Present`'s; the recovery a suboptimal present needs -- retire the
// window's records, rebuild, try again next frame -- is the registry's and this file's.

#include "RenderInternal.hpp"
#include "OpenGLHacks/HostBlit.hpp"
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdint>
#include <span>

namespace ZHLN {

auto RenderContext::Impl::ReconcileDestination(DestinationRegistry::WindowEntry& dest) noexcept -> FrameOutcome<ReconcileReceipt> {
    // Only an acquired destination is closed, and an acquired one always named
    // a record: no handle here means the destination was rebuilt or retired
    // under this frame, and the image this frame acquired went with it. There
    // is nothing to present, and that is an answer rather than a crash.
    if (dest.imageIndex >= dest.recordHandles.size()) {
        return std::unexpected(DestinationError::SlotRetired);
    }
    const DestinationRegistry::Handle handle = dest.recordHandles[dest.imageIndex];
    if (!handle.Valid() || handle.Index() >= destinations.Records().size()) {
        return std::unexpected(DestinationError::SlotRetired);
    }
    DestinationRegistry::Record& record = destinations.Records()[handle.Index()];

    // What a pass left in it, if anything: the receipt is the frame's own
    // bookkeeping and this is the frame's last look at it.
    const auto receipt = record.GetRenderedContent();
    if (!receipt) {
        return std::unexpected(receipt.error());
    }
    if (receipt->has_value()) {
        // A pass wrote it: the destination holds the frame, and there is
        // nothing to add to it. The receipt carries the layout the last
        // writer left, so presentation never re-resolves the record for it.
        return ReconcileReceipt {.rendered = **receipt, .layout = record.trackedLayout};
    }

    // Nothing wrote it, and the frame is about to show it. A vended image's
    // tracked layout starts at UNDEFINED, so handing this one to the presenter
    // as it stands would put whatever the driver left in it on screen: the
    // frame establishes its own contents here -- the background colour -- and
    // says so in the receipt, because a defined image holding no frame is not
    // the same answer to a read-back as one nothing has touched.
    if (!dest.recording.IsOpen()) {
        // The stream that would carry the clear is gone (the destination was
        // rebuilt), so the frame cannot establish anything for it. Said with
        // std::nullopt: nothing was written here, not even by the frame.
        return std::nullopt;
    }

    const VkClearColorValue clear {
        .float32 = {kClearColorScene.r, kClearColorScene.g, kClearColorScene.b, kClearColorScene.a},
    };
    Vk::ClearColorImage(dest.recording.Command(), record.image.handle, clear);
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
    // The frame's own non-failure: if any window's present did not go through as
    // asked, the frame is still this -- drawn, not shown as asked, already
    // rebuilt for. Nullopt means every present went through.
    std::optional<PresentSuboptimal> result {};
    // The first window-local present error, if any. A lost device bails the
    // loop immediately; any other failure is this window's, so the remaining
    // windows still present and advance, and the error is reported once the
    // loop is done.
    std::optional<ErrorCode> firstError {};

    for (auto& dest: destinations.Windows()) {
        if (!dest.imageAcquired) {
            continue;
        }

        Vk::SwapchainPresenter& destPresenter = dest.Presenter();

        // What this destination holds for the frame, decided here because here is where it
        // is about to be shown. A destination a pass wrote is presented as the frame it
        // holds; one nothing wrote is closed with the frame's background first. A destination
        // the frame can no longer speak for is not presented at all.
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

        // The waits this frame's other queues impose on the present submit. The
        // presenter waits the image-available semaphore itself; these are the
        // renderer's to name, because only it knows what else ran this frame.
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

        // The source layout of the present transition: whatever the last
        // writer left, ridden here by the reconcile receipt in the vocabulary
        // a pass speaks (AttachmentLayout) -- the demotion to a raw Vulkan
        // layout belongs to this step, which is the one about to present.
        // ReconcileDestination has closed this destination by now, so "the
        // last writer" is a pass or the frame's own fill, and the image holds
        // something the frame defined either way.
        const ReconcileReceipt& receipt       = **reconciled;
        const VkImageLayout     currentLayout = Vk::ToVkImageLayout(receipt.layout);

        // Records the transition into the frame's command buffer, ends the
        // recording, submits and presents -- in that order, which is why it is
        // one call.
        auto presented = destPresenter.Present(
            ctx.GraphicsQueue(), ctx.PresentQueue(), dest.recording.Command(), dest.imageIndex, currentLayout,
            std::span<const VkSemaphoreSubmitInfo> {waits.data(), waitCount}
        );
        // The presenter ended that recording to put the transition in the
        // submitted stream -- that is its published order, and it holds on both
        // outcomes -- so the handle is retired here rather than closed again.
        dest.recording.Discard();
        if (!presented) {
            // A lost device is the one present failure the frame loop cannot
            // carry on past -- the device behind EVERY window is gone, so
            // presenting the rest is futile and bailing is the honest move.
            // The increment is diagnostics only; recovery is driven by the
            // error return below, not by it.
            if (presented.error().Is(FrameResult::DeviceLost)) {
                Vk::Instance::IncrementNumericalDeviceLoss();
                return std::unexpected(presented.error());
            }
            // Any other present failure is this window's (its surface, its
            // swapchain), not the device's: record the first one, retire this
            // window's acquisition, and let the remaining windows present and
            // advance -- bailing here would leave their AdvanceFrame()
            // uncalled and their parity permanently desynchronised. The error
            // is reported once the loop is done. The one outcome that never
            // arrives here is "the swapchain and the surface disagreed":
            // that is PresentSuboptimal in the value slot, handled below.
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

        // Host presentation (macOS). A destination with no swapchain has no
        // vkQueuePresent to go through: in HostBlit mode the frame lives in the
        // offscreen headless target and the plugin copies it out on its own
        // fence -- which waits on the submit above -- and blits it through its
        // own OpenGL window. Closing that window ends the session, exactly like
        // closing any other engine window.
        if constexpr (isMac) {
            if (!presents && dest.IsPrimary() && presentationMode == PresentationMode::HostBlit && dest.target != nullptr) {
                auto& blitTarget = destPresenter.headlessColorTarget;
                if (blitTarget.Valid()) {
                    // No host window handle is handed over. Every engine window is
                    // created with GLFW_NO_API, so it has no GL context and the
                    // plugin was always going to reject it and open its own 2.1
                    // window (see ResolveWindow in HostBlitSwapchain.cpp); asking
                    // for that directly is the same presentation, and it keeps the
                    // last window type name out of the renderer.
                    if (!HostBlit::Present(
                            blitTarget.image, nullptr, blitTarget.extent.width, blitTarget.extent.height, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                        )) {
                        dest.target->Close();
                    }
                }
            }
        }

        // A present that did not go through as asked (PresentSuboptimal) leaves
        // the window's images and this window's records out of step with the
        // surface: rebuild, retire what was cached against the old generation,
        // and let the next frame vend again. The frame itself still counts as
        // presented up to this point.
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
            // The frame's own non-failure, carried out by EndFrame: the frame
            // was drawn, the present of one of its windows did not go through as
            // asked, and this window is already rebuilt for it.
            result = PresentSuboptimal {};
        }

        // Retire the acquisition and advance this window's own parity. The
        // timeline value is deliberately *not* stepped here: BeginFrame does
        // that for the primary schedule, and an extra window never records
        // compute, so its timeline stays 0 and adds no wait.
        dest.imageAcquired = false;
        destPresenter.AdvanceFrame();
    }

    if (firstError) {
        return std::unexpected(*firstError);
    }
    return result;
}

} // namespace ZHLN
