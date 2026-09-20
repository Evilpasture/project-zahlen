// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/RenderDestinations.cpp
//
// Destinations: which windows a frame can render into, and how one becomes the attachment a
// caller draws through.
//
// This is the adaptation layer between the engine's `Window` and the RHI's
// `Vk::SwapchainPresenter`, and nothing else: it asks a window for a surface, hands it to a
// presenter, and once per frame acquires the image and makes sure a registry record points at
// it. WSI mechanics live in the presenter and bookkeeping in the registry; what is left here
// is the three decisions that need both -- who owns a window's presenter, when a cached
// record has gone stale, and when a frame's command buffer opens.
//
// A window is a destination, not a mode: the caller asks for its attachment and decides what
// to render into it. Offscreen render textures register in the same table (RenderTexture.cpp),
// which is what makes an RTT target and a swapchain image interchangeable.

#include "RenderInternal.hpp"
#include <Zahlen/Log.hpp>
#include <cstdint>
#include <memory>
#include <utility>

namespace ZHLN {

// The frame's stream for a destination

// DestinationRecording is declared beside the entry that owns it (DestinationRegistry.hpp)
// and implemented here, where the acquisition that opens it lives: opening a destination's
// stream is part of acquiring its image.
//
// `Open` is idempotent on purpose -- a second pass into the same destination in one frame
// records into the buffer the first opened, which is what makes "RenderScene then RenderUI,
// same window" one stream rather than two. The two ends are asymmetric: the frame's guard
// closes whatever is still open, while `Discard` is for a buffer whose pool is gone (a rebuilt
// or released presenter), where calling end would be worse than forgetting it. Present retires
// its own stream through Discard, because it ends the buffer itself after recording the
// present transition into it.

DestinationRegistry::DestinationRecording::~DestinationRecording() noexcept {
    // Last resort: the frame's own boundary closes recordings, and the
    // presentation path retires the one it submitted. Reaching here with
    // something open means a caller skipped that, and an open command buffer
    // whose frame is over is a stream nobody finished -- end it.
    Close();
}

DestinationRegistry::DestinationRecording::DestinationRecording(DestinationRecording&& other) noexcept: cmd(other.cmd), open(other.open) {
    // One recording, one owner: the moved-from object has nothing left to end.
    other.cmd  = VK_NULL_HANDLE;
    other.open = false;
}

auto DestinationRegistry::DestinationRecording::operator=(DestinationRecording&& other) noexcept -> DestinationRecording& {
    if (this != &other) {
        Close();
        cmd       = other.cmd;
        open      = other.open;
        other.cmd  = VK_NULL_HANDLE;
        other.open = false;
    }
    return *this;
}

auto DestinationRegistry::DestinationRecording::Open(VkCommandBuffer slot) noexcept -> VkCommandBuffer {
    if (!open) {
        cmd  = slot;
        open = cmd != VK_NULL_HANDLE;
        if (open) {
            ZHLN_BeginCommandBuffer(cmd);
        }
    }
    return cmd;
}

void DestinationRegistry::DestinationRecording::Close() noexcept {
    if (open) {
        if (cmd != VK_NULL_HANDLE) {
            ZHLN_EndCommandBuffer(cmd);
        }
        open = false;
    }
}

void DestinationRegistry::DestinationRecording::Discard() noexcept {
    cmd  = VK_NULL_HANDLE;
    open = false;
}

// Window -> surface -> presenter

auto RenderContext::Impl::FindOrCreateDestination(Window& aux, bool primary) noexcept -> std::expected<DestinationVend, ErrorCode> {
    if (auto* existing = destinations.Find(aux); existing != nullptr) {
        return DestinationVend {.entry = existing, .created = false};
    }
    if (destinations.Full()) {
        // The registry has a backstop with the same condition; this is the check
        // that comes first, before a surface and a presenter are built for a
        // window that could not be recorded anyway.
        return std::unexpected(DestinationError::RegistryFull);
    }

    DestinationRegistry::WindowEntry dest {};
    dest.window = &aux;

    if (!primary) {
        if (presentationMode != PresentationMode::NativeSwapchain) {
            return std::unexpected(DestinationError::NativeSwapchainRequired);
        }
        if (ctx.Device() == VK_NULL_HANDLE) {
            return std::unexpected(DestinationError::DeviceUnavailable);
        }

        // The window's own surface, then its own presenter on top of it: an
        // extra window is not a second viewport of the primary's swapchain.
        int  width      = 0;
        int  height     = 0;
        auto surfaceRes = aux.CreateVulkanSurface(ctx.Instance(), ctx.Physical(), width, height);
        if (!surfaceRes) {
            // The surface layer knows more about why than this one could say in
            // its own vocabulary, so its code travels untranslated.
            return std::unexpected(ErrorCode {surfaceRes.error()});
        }

        auto owned     = std::make_unique<Vk::SwapchainPresenter>();
        owned->surface = Vk::Surface(ctx.Instance(), static_cast<VkSurfaceKHR>(*surfaceRes));
        if (owned->surface.Get() == VK_NULL_HANDLE || width <= 0 || height <= 0) {
            return std::unexpected(DestinationError::SurfaceUnusable);
        }
        if (auto initRes = owned->Init(ctx, allocator, static_cast<uint32_t>(width), static_cast<uint32_t>(height), ctx.PhysicalInfo().graphics_family, true);
            !initRes) {
            // Bring-up failure, in the presenter's words: which call gave up is
            // more than "the swapchain could not be created" would have said.
            return std::unexpected(initRes.error());
        }
        // Blitting between destinations requires one present format; a window
        // whose surface disagrees cannot be a destination.
        if (owned->GetPresentFormat() != presenter.GetPresentFormat()) {
            return std::unexpected(DestinationError::PresentFormatMismatch);
        }
        dest.presenter      = owned.get();
        dest.ownedPresenter = std::move(owned);
    } else {
        // The primary window's presenter belongs to the render context; the
        // entry only borrows it.
        dest.presenter = &presenter;
    }

    if (auto* entry = destinations.Attach(std::move(dest)); entry != nullptr) {
        return DestinationVend {.entry = entry, .created = true};
    }
    // Unreachable while the Full() check above stands, and honest if it does
    // not: the table being full is Attach's only way to refuse.
    return std::unexpected(DestinationError::RegistryFull);
}

// Acquiring the frame's image

auto RenderContext::Impl::AcquireDestinationImage(DestinationRegistry::WindowEntry& dest) noexcept
    -> std::expected<std::optional<DestinationRegistry::Handle>, ErrorCode> {
    if (dest.imageAcquired) {
        // Vended this frame already: hand back the same handle, or nothing if
        // the generation moved under it (which clears the array).
        if (dest.imageIndex >= dest.recordHandles.size() || !dest.recordHandles[dest.imageIndex].Valid()) {
            return std::nullopt;
        }
        return dest.recordHandles[dest.imageIndex];
    }
    if (dest.window == nullptr) {
        return std::nullopt;
    }

    Vk::SwapchainPresenter& destPresenter = dest.Presenter();

    // The presenter acquires, and -- for a caller-owned window whose size
    // drifted -- rebuilds first. The primary window is not rebuilt here:
    // BeginFrame's RecreateTargets does that, because its resize also recreates
    // every internal target the renderer owns.
    const Extent2D size     = dest.window->GetSize();
    auto           acquired = destPresenter.AcquireNext(VkExtent2D {.width = size.width, .height = size.height}, /*allowRebuild=*/!dest.IsPrimary());
    if (!acquired) {
        // A real error leaves through the error slot: what a failed acquire means for the
        // frame is the caller's to decide. DeviceLost also invalidates this destination's
        // records -- device and swapchain are gone; any other code leaves the swapchain, and
        // so the records, as they were.
        const ErrorCode error = acquired.error();
        if (!error.Is(FrameResult::DeviceLost)) {
            return std::unexpected(error);
        }
        Vk::Instance::NotifyDeviceLost();
        destinations.Retire(dest.window);
        // The rebuild took the pool this destination's stream came from with
        // it, so the handle is forgotten rather than closed -- and the same
        // goes for every path below that retires a destination mid-frame.
        dest.recording.Discard();
        dest.recordHandles.clear();
        dest.cachedGeneration = destPresenter.resourceGeneration;
        return std::unexpected(error);
    }
    if (!acquired->has_value()) {
        // Nothing was vended: the swapchain no longer matched the surface and
        // the presenter has already rebuilt what it could. Either way the
        // handles these records were built from are gone with it.
        destinations.Retire(dest.window);
        dest.recording.Discard();
        dest.recordHandles.clear();
        dest.cachedGeneration = destPresenter.resourceGeneration;
        return std::nullopt;
    }

    // The value slot is engaged, so there is an image in hand -- and everything
    // below is about that image and the records built from it.
    const Vk::SwapchainTarget& target = **acquired;

    // Any rebuild -- BeginFrame's RecreateTargets on a resize, one triggered by a present
    // that did not go through, a caller's own Rebuild, or the one AcquireNext just did --
    // replaces the images this destination's records were built from. The generation counter
    // catches all of them, including the headless case where the swapchain stays null and the
    // offscreen target is swapped underneath us.
    if (dest.cachedGeneration != target.generation) {
        if (dest.cachedGeneration != 0) {
            // Say so: a record retired out from under a caller is exactly the
            // class of bug that otherwise shows up as a driver complaint about
            // an invalid image view, or as a blank capture, with nothing in the
            // log tying the two together.
            ZHLN::Log(
                "[Render] Destination resources rebuilt (generation {} -> {}); re-vending the window's image.", dest.cachedGeneration,
                target.generation
            );
            destinations.Retire(dest.window);
            dest.recording.Discard();
            dest.recordHandles.clear();
        }
        dest.cachedGeneration = target.generation;
    }

    dest.imageIndex = target.imageIndex;
    if (dest.recordHandles.size() <= target.imageIndex) {
        dest.recordHandles.resize(target.imageIndex + 1);
    }
    if (!dest.recordHandles[target.imageIndex].Valid()) {
        dest.recordHandles[target.imageIndex] = destinations.Register(DestinationRegistry::Record {
            .bindlessIndex = 0,
            // The image the presenter acquired, moved in whole: the record and
            // the target describe one image in the one vocabulary they share.
            .image         = target.image,
            .presentable   = target.presentable,
            .generation    = target.generation,
            .window        = dest.window,
        });
    }

    // Freshly acquired swapchain contents are undefined; a record's tracked
    // layout starts over so the first pass this frame knows it may discard, and
    // the frame's receipt for it starts empty because nothing has written this
    // incarnation of the image yet.
    const DestinationRegistry::Handle handle = dest.recordHandles[target.imageIndex];
    DestinationRegistry::Record&      record = destinations.Records()[handle.Index()];
    record.trackedLayout = Vk::AttachmentLayout::Undefined;
    record.content.reset();

    dest.imageAcquired = true;
    return handle;
}

// Acquisition and the query that does nothing

namespace {

/// The descriptor a destination's image is vended as. One definition, because
/// the query and the acquisition have to agree on what an attachment is.
[[nodiscard]] constexpr auto AttachmentFor(DestinationRegistry::Handle handle) noexcept -> RenderAttachment {
    return RenderAttachment {.texture = handle.AsTexture(), .mipLevel = 0, .arrayLayer = 0};
}

/// The descriptor a destination's acquired image is vended as, or nothing when
/// it has no image in hand this frame. Reads the entry's frame state and mints
/// the same value every time: this is the whole of the pure query.
[[nodiscard]] auto VendedAttachmentOf(const DestinationRegistry::WindowEntry& dest) noexcept -> std::optional<RenderAttachment> {
    if (!dest.imageAcquired || dest.imageIndex >= dest.recordHandles.size()) {
        return std::nullopt;
    }
    const DestinationRegistry::Handle handle = dest.recordHandles[dest.imageIndex];
    if (!handle.Valid()) {
        return std::nullopt;
    }
    return AttachmentFor(handle);
}

} // namespace

auto RenderContext::Impl::WindowAttachment(const Window& aux) noexcept -> std::optional<RenderAttachment> {
    // The answer is the destination's, and asking for it changes nothing:
    // it does not acquire, does not wait, does not open a command buffer, and
    // cannot be told apart from not having asked. A window the frame has not
    // acquired yet -- or one that is not a destination at all -- has no
    // attachment, which is the whole of what this can say.
    const DestinationRegistry::WindowEntry* dest = destinations.Find(aux);
    if (dest == nullptr) {
        return std::nullopt;
    }
    return VendedAttachmentOf(*dest);
}

auto RenderContext::Impl::AcquireTarget(const Window& aux) noexcept -> FrameOutcome<RenderAttachment> {
    // Acquiring an image and opening the frame's command buffer both belong to a
    // frame. Outside BeginFrame/EndFrame there is no frame to own them, so that
    // is what the caller is told: an attachment that recorded into a pool nobody
    // reset would be worse than none at all.
    if (!activeQueueGuard.has_value()) {
        return std::unexpected(DestinationError::NoActiveFrame);
    }

    auto found = FindOrCreateDestination(const_cast<Window&>(aux), &aux == &window);
    if (!found) {
        return std::unexpected(found.error());
    }
    if (found->created) {
        // This call just built the destination and is the boundary that hands it out, so it
        // says which presenter it uses: a window that is not the renderer's primary owns its
        // own, and a frame rendered into it is not the frame the primary presents -- from the
        // outside, a black window with no other symptom. Attach hands the entry back, so this
        // is not a second lookup.
        const DestinationRegistry::WindowEntry* entry = found->entry;
        ZHLN::Log(
            "[Render] Destination created for window {:p} (primary={}); {}", static_cast<const void*>(entry->window), entry->IsPrimary() ? 1 : 0,
            entry->IsPrimary() ? "borrowing the renderer's presenter" : "owning its own presenter"
        );
    }
    DestinationRegistry::WindowEntry* dest = found->entry;
    destinations.SetActive(dest->window);

    const auto acquired = AcquireDestinationImage(*dest);
    if (!acquired) {
        return std::unexpected(acquired.error());
    }
    if (!acquired->has_value()) {
        // Nothing was acquired this frame -- the destination was retired, or it
        // has no image to hand out. Not an error: the caller has nothing to draw
        // into, and drawing nothing is what it already does with an empty
        // attachment.
        return std::nullopt;
    }

    // The frame's stream for this destination. Acquisition deliberately stops at the image;
    // opening the buffer is this call's, because this is what makes a destination drawable --
    // and a pass that never gets an attachment has nothing to record into. A second
    // acquisition in one frame opens nothing: the recording is idempotent.
    Vk::SwapchainPresenter& destPresenter = dest->Presenter();
    dest->recording.Open(destPresenter.SlotCommand(destPresenter.frameIndex));

    return AttachmentFor(**acquired);
}

auto RenderContext::Impl::RecordingFor(const DestinationRegistry::Record& record) const noexcept -> VkCommandBuffer {
    const DestinationRegistry::WindowEntry* dest = destinations.DestinationOf(record);
    return dest != nullptr ? dest->recording.Command() : VK_NULL_HANDLE;
}

auto RenderContext::Impl::FrameCommand() const noexcept -> VkCommandBuffer {
    const DestinationRegistry::WindowEntry* active = destinations.ActiveDestination();
    return active != nullptr ? active->recording.Command() : VK_NULL_HANDLE;
}

// Teardown

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
        // The released window's swapchain and records are about to die, so the device must be
        // idle first. A lost device has to be *captured* here, not discarded: the next frame's
        // BeginFrame wait only reports what the instance's lost-device state already says.
        // Non-fatal wait failures (a driver hiccup) stay unreported by design -- the teardown
        // below is safe either way.
        if (const auto waited = Vk::WaitIdle(ctx.Device()); !waited && waited.error().Is(FrameResult::DeviceLost)) {
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
        if (const auto waited = Vk::WaitIdle(ctx.Device()); !waited && waited.error().Is(FrameResult::DeviceLost)) {
            Vk::Instance::NotifyDeviceLost();
        }
    }
    destinations.Clear();
}

} // namespace ZHLN
