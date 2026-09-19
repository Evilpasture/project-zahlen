// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/RenderDestinations.cpp
//
// Destinations: which windows a frame can render into, and how one of them
// becomes the attachment a caller draws through.
//
// This is the adaptation layer between the engine's `Window` and the RHI's
// `Vk::SwapchainPresenter`, and nothing else. It asks a window for a surface,
// hands it to a presenter, and -- once per frame -- acquires the destination's
// image and makes sure a registry record points at it. The Vulkan WSI mechanics
// (acquire, transition, submit, present) live in the presenter; the bookkeeping
// (records, generations, handles) lives in the registry; what is left here is
// the three decisions that need both: who owns a window's presenter, when a
// cached record has gone stale, and when a frame's command buffer opens.
//
// A window is a destination, not a mode. There is no viewport kind to dispatch
// on: the caller asks for a window's attachment, the image is acquired and its
// handle vended, and the caller decides what to render into it. Offscreen render
// textures register in the same table (see RenderTexture.cpp), which is what
// makes a render-to-texture target and a swapchain image interchangeable.

#include "RenderInternal.hpp"
#include <Zahlen/Log.hpp>
#include <cstdint>
#include <memory>
#include <utility>

namespace ZHLN {

// ============================================================================
// Window -> surface -> presenter
// ============================================================================

auto RenderContext::Impl::FindOrCreateDestination(Window& aux, bool primary) noexcept
    -> std::expected<DestinationRegistry::WindowEntry*, ErrorCode> {
    if (auto* existing = destinations.Find(aux); existing != nullptr) {
        return existing;
    }
    if (destinations.Full()) {
        ZHLN::Log("[Render] Destination registry full; ignoring window.");
        return std::unexpected(Vk::PresentationError::WindowNotPresented);
    }

    DestinationRegistry::WindowEntry dest {};
    dest.window = &aux;

    if (!primary) {
        if (presentationMode != PresentationMode::NativeSwapchain) {
            return std::unexpected(Vk::PresentationError::NativeSwapchainRequired);
        }
        if (ctx.Device() == VK_NULL_HANDLE) {
            return std::unexpected(Vk::PresentationError::ContextInvalid);
        }

        // The window's own surface, then its own presenter on top of it: an
        // extra window is not a second viewport of the primary's swapchain.
        int  width      = 0;
        int  height     = 0;
        auto surfaceRes = aux.CreateVulkanSurface(ctx.Instance(), ctx.Physical(), width, height);
        if (!surfaceRes) {
            ZHLN::Log("[Render] Destination surface creation failed ({})", surfaceRes.error());
            return std::unexpected(ErrorCode {surfaceRes.error()});
        }

        auto owned    = std::make_unique<Vk::SwapchainPresenter>();
        owned->surface = Vk::Surface(ctx.Instance(), static_cast<VkSurfaceKHR>(*surfaceRes));
        if (owned->surface.Get() == VK_NULL_HANDLE || width <= 0 || height <= 0) {
            return std::unexpected(Vk::PresentationError::WindowNotPresented);
        }
        if (auto initRes = owned->Init(ctx, allocator, static_cast<uint32_t>(width), static_cast<uint32_t>(height), ctx.PhysicalInfo().graphics_family, true);
            !initRes) {
            ZHLN::Log("[Render] Destination swapchain init failed ({})", initRes.error());
            return std::unexpected(initRes.error());
        }
        // Blitting between destinations requires one present format; a window
        // whose surface disagrees cannot be a destination.
        if (owned->GetPresentFormat() != presenter.GetPresentFormat()) {
            ZHLN::Log("[Render] Destination refused: present format differs from the primary swapchain.");
            return std::unexpected(Vk::PresentationError::PresentFormatMismatch);
        }
        dest.presenter      = owned.get();
        dest.ownedPresenter = std::move(owned);
    } else {
        // The primary window's presenter belongs to the render context; the
        // entry only borrows it.
        dest.presenter = &presenter;
    }

    destinations.Attach(std::move(dest));
    return destinations.Find(aux);
}

// ============================================================================
// Acquiring the frame's image
// ============================================================================

auto RenderContext::Impl::AcquireDestinationImage(DestinationRegistry::WindowEntry& dest) noexcept -> uint32_t {
    if (dest.imageAcquired) {
        const uint32_t slot = dest.imageIndex < dest.recordSlots.size() ? dest.recordSlots[dest.imageIndex] : 0;
        return slot;
    }
    if (dest.window == nullptr) {
        return 0;
    }

    Vk::SwapchainPresenter& destPresenter = dest.Presenter();

    // The presenter acquires, and -- for a caller-owned window whose size
    // drifted -- rebuilds first. The primary window is not rebuilt here:
    // BeginFrame's RecreateTargets does that, because its resize also recreates
    // every internal target the renderer owns.
    const Extent2D size   = dest.window->GetSize();
    auto           target = destPresenter.AcquireNext(VkExtent2D {.width = size.width, .height = size.height}, /*allowRebuild=*/!dest.IsPrimary());
    if (!target) {
        const ErrorCode error = target.error();
        if (error.Is(Vk::PresentationError::DeviceLost)) {
            Vk::Instance::NotifyDeviceLost();
        }
        if (error.Is(Vk::PresentationError::SwapchainOutOfDate) || error.Is(Vk::PresentationError::DeviceLost)) {
            // The presenter rebuilt what it could; the handles this
            // destination's records were built from are gone either way.
            destinations.Retire(dest.window);
            dest.recordSlots.clear();
            dest.cachedGeneration = destPresenter.resourceGeneration;
        }
        return 0;
    }

    // Any rebuild -- BeginFrame's RecreateTargets on a resize, a present-time
    // Suboptimal/OutOfDate, a caller's own Rebuild, or the one AcquireNext
    // just did -- replaces the images this destination's records were built
    // from. The generation counter catches all of them, including the headless
    // case where the swapchain handle stays null and the offscreen target is
    // quietly swapped underneath us.
    if (dest.cachedGeneration != target->generation) {
        if (dest.cachedGeneration != 0) {
            // Say so: a record retired out from under a caller is exactly the
            // class of bug that otherwise shows up as a driver complaint about
            // an invalid image view, or as a blank capture, with nothing in the
            // log tying the two together.
            ZHLN::Log(
                "[Render] Destination resources rebuilt (generation {} -> {}); re-vending the window's image.", dest.cachedGeneration,
                target->generation
            );
            destinations.Retire(dest.window);
            dest.recordSlots.clear();
        }
        dest.cachedGeneration = target->generation;
    }

    dest.imageIndex = target->imageIndex;
    if (dest.recordSlots.size() <= target->imageIndex) {
        dest.recordSlots.resize(target->imageIndex + 1, 0);
    }
    if (dest.recordSlots[target->imageIndex] == 0) {
        const auto handle = destinations.Register(DestinationRegistry::Record {
            .bindlessIndex = 0,
            .image         = target->image,
            .view          = target->view,
            .extent        = {.width = target->extent.width, .height = target->extent.height, .depth = 1},
            .format        = target->format,
            .presentable   = target->presentable,
            .generation    = target->generation,
            .window        = dest.window,
        });
        dest.recordSlots[target->imageIndex] = handle.Index() + 1;
    }

    // Freshly acquired swapchain contents are undefined; a record's tracked
    // layout starts over so the first pass this frame knows it may discard.
    DestinationRegistry::Record& record = destinations.Records()[dest.recordSlots[target->imageIndex] - 1];
    record.writtenThisFrame = false;
    record.backgroundFilled = false;
    record.trackedLayout    = Vk::AttachmentLayout::Undefined;

    dest.imageAcquired = true;
    return dest.recordSlots[target->imageIndex];
}

// ============================================================================
// Vending
// ============================================================================

auto RenderContext::Impl::VendedWindowAttachment(const Window& aux) noexcept -> RenderAttachment {
    // Vending an attachment acquires an image and opens the frame's command
    // buffer; both belong to a frame. Outside BeginFrame/EndFrame there is no
    // frame to own them, so hand back an empty attachment instead of recording
    // into a pool nobody reset.
    if (!activeQueueGuard.has_value()) {
        ZHLN::Log("[Render] GetWindowAttachment outside BeginFrame/EndFrame; attachment refused.");
        return {};
    }

    auto found = FindOrCreateDestination(const_cast<Window&>(aux), &aux == &window);
    if (!found) {
        return {};
    }
    DestinationRegistry::WindowEntry* dest = *found;
    destinations.SetActive(dest->window);

    const uint32_t slot = AcquireDestinationImage(*dest);
    if (slot == 0) {
        return {};
    }

    // The acquire deliberately stops at the image: it does not touch the
    // frame's command buffer. Opening it is the vend's job -- this is the call
    // that hands the attachment to a caller, and a caller that never gets one
    // has nothing to record into. The second vend of the same destination in
    // one frame reuses the buffer the first one opened.
    if (!dest->commandOpen) {
        Vk::SwapchainPresenter& destPresenter = dest->Presenter();
        dest->openCmd                         = destPresenter.SlotCommand(destPresenter.frameIndex);
        ZHLN_BeginCommandBuffer(dest->openCmd);
        dest->commandOpen = true;
    }

    current_cmd         = dest->openCmd;
    current_image_index = dest->imageIndex;
    return RenderAttachment {.texture = destinations.Records()[slot - 1].handle.AsTexture(), .mipLevel = 0, .arrayLayer = 0};
}

// ============================================================================
// Teardown
// ============================================================================

void RenderContext::Impl::ReleaseWindow(const Window& aux) noexcept {
    DestinationRegistry::WindowEntry* entry = destinations.Find(aux);
    if (entry == nullptr || entry->IsPrimary()) {
        // No destination at all, or the primary window's presenter, which
        // outlives every caller: releasing it is a no-op rather than a teardown
        // of the renderer's own presentation.
        return;
    }

    const Window* released = entry->window;
    if (ctx.Device() != VK_NULL_HANDLE) {
        // The released window's swapchain and records are about to die; the
        // device must be idle first. A lost device has to be *captured* here,
        // not discarded: the next frame's BeginFrame wait only reports what the
        // instance's lost-device state already says, so a wait failure nobody
        // notes is a wait failure nobody reports. Non-fatal wait failures (a
        // driver hiccup) leave the instance state alone and stay unreported by
        // design -- the teardown below is safe either way.
        if (const auto waited = Vk::WaitIdle(ctx.Device()); !waited && waited.error().Is(Vk::VulkanCallError::DeviceLost)) {
            Vk::Instance::NotifyDeviceLost();
        }
    }
    destinations.Detach(aux);
    destinations.Retire(released);
}

void RenderContext::Impl::DestroyDestinations() noexcept {
    if (ctx.Device() != VK_NULL_HANDLE) {
        // Same rule as ReleaseWindow: consume the wait, don't drop it, and
        // hand a lost device to the instance state the next frame reads.
        if (const auto waited = Vk::WaitIdle(ctx.Device()); !waited && waited.error().Is(Vk::VulkanCallError::DeviceLost)) {
            Vk::Instance::NotifyDeviceLost();
        }
    }
    destinations.Clear();
}

} // namespace ZHLN
