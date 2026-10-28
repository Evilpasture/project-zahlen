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
    image.image.DiscardTo<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd);
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

auto RenderContext::Impl::PresentUsedWindows() noexcept -> PresentBatch {
    PresentBatch presented;

    // One window, one entry. A window that cannot present does not take the
    // others with it: its failure goes in the batch and the loop carries on, so
    // a session with three windows and one broken swapchain still shows the
    // other two. The batch is also why nothing has to be "remembered": the old
    // shape kept `std::optional<ErrorCode> firstError` alongside the frame
    // result, which silently dropped every failure after the first.
    for (auto& dest: destinations.Windows()) {
        if (!dest.acquired) {
            continue;
        }
        const uint64_t          windowId      = dest.id;
        Vk::SwapchainPresenter& destPresenter = dest.Presenter();
        auto                    recording     = destinations.FindRecording(windowId);
        if (!recording || !recording->recorder.IsRecording()) {
            ZHLN::Log("[Render] Window target has no command stream to present.");
            dest.acquired.reset();
            destPresenter.AdvanceFrame();
            presented.EmplaceSuccess(PresentOutcome {.windowId = windowId, .disposition = PresentDisposition::NotRecorded});
            continue;
        }
        geometry.RecordPendingUpdates(recording->recorder.Handle());
        const auto reconciled = ReconcileDestination(dest, recording->recorder);
        if (!reconciled) {
            ZHLN::Log("[Render] Window target cannot be presented: {}.", reconciled.error());
            dest.acquired.reset();
            destPresenter.AdvanceFrame();
            presented.EmplaceSuccess(PresentOutcome {.windowId = windowId, .disposition = PresentDisposition::NotReconcilable});
            continue;
        }

        const bool presents = destPresenter.HasSwapchain();

        std::array<Vk::SemaphoreSubmit, 4> waits {};
        uint32_t waitCount = 0;
        const uint64_t stagingValue = transferRingBuffer.GetCurrentValue();
        if (transferRingBuffer.GetSemaphore() != VK_NULL_HANDLE && stagingValue > 0) {
            waits[waitCount++] = Vk::SemaphoreSubmit::Timeline(transferRingBuffer.GetSemaphore(), stagingValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        }

        // Overlap CPU frame N with GPU frame N-1 without GPU render-target hazards:
        // order this window's graphics submission behind the previous frame slot's
        // graphics timeline on the GPU rather than stalling the CPU in BeginFrame.
        if (auto prevGfxDep = destPresenter.sync.GetGraphicsWaitDependency(
                Vk::PreviousFrameSlot(destPresenter.frameIndex), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
            )) {
            waits[waitCount++] = *prevGfxDep;
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
            const bool lost = Vk::IsDeviceLost(executable.error());
            if (lost) {
                NoteDeviceLost("PresentUsedWindows::End");
            }
            presented.EmplaceError(PresentFailure {.windowId = windowId, .error = AsFrameError(executable.error())});
            dest.acquired.reset();
            destPresenter.AdvanceFrame();
            if (lost) {
                // Every remaining window would fail the same way on the same
                // device, so the loop ends here; the batch keeps the failure.
                break;
            }
            continue;
        }
        auto result = destPresenter.Present(
            ctx.GraphicsQueue(), ctx.PresentQueue(), std::move(*executable), dest.acquired->imageIndex,
            std::span<const Vk::SemaphoreSubmit> {waits.data(), waitCount}
        );
        if (!result) {
            const bool lost = Vk::IsDeviceLost(result.error());
            if (lost) {
                NoteDeviceLost("PresentUsedWindows::Present");
            }
            ZHLN::Log("[Render] Present for window {:p} failed ({}); presenting other windows.", static_cast<const void*>(dest.target), result.error());
            presented.EmplaceError(PresentFailure {.windowId = windowId, .error = AsFrameError(result.error())});
            dest.acquired.reset();
            destPresenter.AdvanceFrame();
            if (lost) {
                break;
            }
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

        // An engaged optional from Present() is the swapchain saying "that went
        // out, but rebuild me" -- as opposed to a plain empty one, which is the
        // same present with nothing to ask for.
        const bool suboptimal = result->has_value();
        if (suboptimal) {
            const Extent2D size = dest.target != nullptr ? dest.target->GetFramebufferExtent() : Extent2D {};
            if (size.width != 0 && size.height != 0) {
                if (!destPresenter.Rebuild(size.width, size.height)) {
                    ZHLN::Log("[Render] Window rebuild after present failed; retrying next frame.");
                }
            }

            dest.acquired.reset();
            dest.cachedGeneration = destPresenter.resourceGeneration;
        }

        presented.EmplaceSuccess(
            PresentOutcome {.windowId = windowId, .disposition = suboptimal ? PresentDisposition::Suboptimal : PresentDisposition::Presented}
        );
        destPresenter.AdvanceFrame();
    }

    return presented;
}

} // namespace ZHLN
