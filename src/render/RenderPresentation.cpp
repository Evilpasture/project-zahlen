// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/RenderPresentation.cpp
//
// The end of a frame, for every window the frame was drawn into.
//
// Two jobs, both of them the renderer's because both are about the *frame* and
// not about the swapchain:
//
//   * a destination that was vended but never written has to be handed
//     presentation defined contents -- the background colour -- rather than an
//     image whose contents are undefined;
//   * the frame's submission has to be ordered behind the other queues it used
//     (the transfer ring's timeline, the async compute frame's), which is
//     knowledge the RHI's presenter does not have and should not want.
//
// Everything Vulkan-specific below is naming: which semaphores the submission
// waits on, which layout the image was last left in. The transition, the submit
// and the present are `Vk::SwapchainPresenter::Present`'s, and the recovery a
// suboptimal present needs -- retire the window's records, rebuild, try again
// next frame -- is the registry's and this file's.

#include "RenderInternal.hpp"
#include "OpenGLHacks/HostBlit.hpp"
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdint>
#include <span>

namespace ZHLN {

void RenderContext::Impl::FillUnwrittenDestinations() noexcept {
    for (DestinationRegistry::WindowEntry& dest: destinations.Windows()) {
        if (!dest.imageAcquired || dest.imageIndex >= dest.recordHandles.size()) {
            continue;
        }
        const DestinationRegistry::Handle handle = dest.recordHandles[dest.imageIndex];
        if (!handle.Valid() || handle.Index() >= destinations.Records().size()) {
            continue;
        }
        DestinationRegistry::Record& record = destinations.Records()[handle.Index()];
        // A receipt at this point means a pass wrote the image: the frame's own
        // fill is the only other writer, and if it had run, this loop is what
        // ran it.
        if (record.content.has_value() || !record.image.Valid()) {
            continue;
        }

        if (dest.recording.IsOpen()) {
            const VkClearColorValue clear {
                .float32 = {kClearColorScene.r, kClearColorScene.g, kClearColorScene.b, kClearColorScene.a},
            };
            Vk::ClearColorImage(dest.recording.Command(), record.image.handle, clear);
            record.trackedLayout = Vk::AttachmentLayout::ColorAttachment;
            // The frame is the writer here, and says so: a capture or a test
            // metric reading this image would see the background colour and be
            // right to call the scene black -- except the scene was never in
            // it, and only the receipt can tell the two apart.
            record.content = DestinationRegistry::Rendered {.by = DestinationRegistry::Rendered::By::FrameFill};
        } else {
            // No pass wrote it and the stream that would carry the clear is
            // gone, so this image holds nothing defined. The receipt stays
            // empty, which is what tells a read-back exactly that -- the state
            // a boolean pair could not name.
            record.trackedLayout = Vk::AttachmentLayout::Undefined;
        }

        if (!destinations.UnwrittenWarned()) {
            ZHLN::Log(
                "[Render] Destination 0x{:016X} (extent {}x{}) was vended but no pass wrote it this frame; filled with the background colour.",
                record.handle.Raw(), record.image.extent.width, record.image.extent.height
            );
            destinations.NoteUnwrittenWarned();
        }
    }
}

auto RenderContext::Impl::PresentUsedWindows() noexcept -> FrameOutcome<PresentSuboptimal> {
    // The frame's own non-failure: if any window's present did not go through as
    // asked, the frame is still this -- drawn, not shown as asked, already
    // rebuilt for. Nullopt means every present went through.
    std::optional<PresentSuboptimal> result {};

    for (auto& dest: destinations.Windows()) {
        if (!dest.imageAcquired) {
            continue;
        }

        Vk::SwapchainPresenter& destPresenter = dest.Presenter();
        const bool              presents      = destPresenter.HasSwapchain();
        const uint32_t          slot          = destPresenter.frameIndex;

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
        const ZHLN_FrameSync& sync         = destPresenter.sync[slot];
        const uint64_t        computeValue = destPresenter.sync.GetTimelineValue(slot);
        if (sync.compute_timeline != VK_NULL_HANDLE && computeValue > 0 && computeSubmittedThisFrame) {
            waits[waitCount++] = Vk::MakeSemaphoreSubmitInfo(sync.compute_timeline, computeValue, Vk::kAsyncComputeConsumerStages);
        }

        // The source layout of the present transition: whatever the last writer
        // left, mapped from the vocabulary a pass speaks (AttachmentLayout).
        // From Undefined -- a vended image no pass wrote -- that is exactly
        // VK_IMAGE_LAYOUT_UNDEFINED, which is always a legal oldLayout because
        // the contents are don't-care.
        VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (dest.imageIndex < dest.recordHandles.size()) {
            const DestinationRegistry::Handle handle = dest.recordHandles[dest.imageIndex];
            if (handle.Valid() && handle.Index() < destinations.Records().size()) {
                currentLayout = Vk::ToVkImageLayout(destinations.Records()[handle.Index()].trackedLayout);
            }
        }

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
            // carry on past -- and it has a name, so nothing has to be
            // translated to ask for it.
            if (presented.error().Is(FrameResult::DeviceLost)) {
                Vk::Instance::NotifyDeviceLost();
            }
            // Every present error fails the frame. The one outcome that does not
            // arrive here is "the swapchain and the surface disagreed", which is
            // PresentSuboptimal in the value slot, not an error; it is reported
            // below, after this window's bookkeeping is done, so the other
            // windows still present.
            return std::unexpected(presented.error());
        }

        // Host presentation (macOS). A destination with no swapchain has no
        // vkQueuePresent to go through: in HostBlit mode the frame lives in the
        // offscreen headless target and the plugin copies it out on its own
        // fence -- which waits on the submit above -- and blits it through its
        // own OpenGL window. Closing that window ends the session, exactly like
        // closing any other engine window.
        if constexpr (isMac) {
            if (!presents && dest.IsPrimary() && presentationMode == PresentationMode::HostBlit && dest.window != nullptr) {
                auto& target = destPresenter.headlessColorTarget;
                if (target.Valid()) {
                    auto* win = static_cast<GLFWwindow*>(dest.window->GetNativeHandle());
                    if (!HostBlit::Present(
                            target.image, win, target.extent.width, target.extent.height, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                        )) {
                        dest.window->Close();
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
            const Extent2D size = dest.window != nullptr ? dest.window->GetSize() : Extent2D {};
            if (size.width != 0 && size.height != 0) {
                if (!destPresenter.Rebuild(size.width, size.height)) {
                    ZHLN::Log("[Render] Destination rebuild after present failed; retrying next frame.");
                }
            }
            destinations.Retire(dest.window);
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

    return result;
}

} // namespace ZHLN
