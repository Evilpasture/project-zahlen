// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/RenderAttachments.cpp
//
// Destinations: the registry that turns a `RenderAttachment` (a subresource
// handle) back into a concrete VkImage, and the presentation path that ends a
// frame for every window that was drawn into.
//
// A window is a destination, not a mode. There is no viewport kind to dispatch
// on: `GetWindowAttachment` acquires the window's next swapchain image (or the
// headless offscreen target) and vends its handle, the caller decides what to
// render into it, and EndFrame presents the windows that were actually used.
// Offscreen render textures register in the same table, which is what makes a
// render-to-texture target and a swapchain image interchangeable to a caller.

#include "RenderInternal.hpp"
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>

namespace ZHLN {

namespace {

[[nodiscard]] constexpr auto MakeCmdSubmit(VkCommandBuffer cmd) noexcept -> VkCommandBufferSubmitInfo {
    return VkCommandBufferSubmitInfo {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext         = nullptr,
        .commandBuffer = cmd,
        .deviceMask    = 0,
    };
}

[[nodiscard]] constexpr auto MakeSemSubmit(VkSemaphore sem, uint64_t value, VkPipelineStageFlags2 stage) noexcept -> VkSemaphoreSubmitInfo {
    return VkSemaphoreSubmitInfo {
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext     = nullptr,
        .semaphore = sem,
        .value     = value,
        .stageMask = stage,
    };
}

} // namespace

// ============================================================================
// Registry
// ============================================================================

auto RenderContext::Impl::FindDestination(const Window& aux) noexcept -> DestinationWindow* {
    const auto it = std::find_if(destinationWindows.begin(), destinationWindows.end(), [&](const DestinationWindow& dest) { return dest.window == &aux; });
    return it != destinationWindows.end() ? &*it : nullptr;
}

auto RenderContext::Impl::RegisterRenderTarget(RenderTargetRecord record) noexcept -> uint32_t {
    // A retired slot (no handle, no view) is free again. Reusing it instead of
    // appending keeps the registry bounded by live records rather than by every
    // registration ever made; the handle carries the index explicitly, so a
    // recycled slot is still addressable by the callers that hold its handle.
    size_t index = renderTargets.size();
    for (size_t i = 0; i < renderTargets.size(); ++i) {
        const RenderTargetRecord& slot = renderTargets[i];
        if (slot.handle == TextureHandle::Invalid && slot.view == VK_NULL_HANDLE && slot.image == VK_NULL_HANDLE) {
            index = i;
            break;
        }
    }

    // The serial is minted per registration, so a handle vended before this
    // record existed cannot resolve to it even though the slot index is reused.
    // 0 is the retired marker, so the counter steps over it.
    uint32_t serial = static_cast<uint32_t>(nextRenderTargetSerial++) & 0x00FF'FFFFu;
    if (serial == 0) {
        serial = static_cast<uint32_t>(nextRenderTargetSerial++) & 0x00FF'FFFFu;
    }
    record.serial = serial;
    record.handle = static_cast<TextureHandle>(
        kRenderTargetHandleTag | (static_cast<uint64_t>(serial) << kRenderTargetSerialShift) | (static_cast<uint64_t>(index) & kRenderTargetIndexMask)
    );

    if (index == renderTargets.size()) {
        renderTargets.push_back(record);
    } else {
        renderTargets[index] = record;
    }
    return static_cast<uint32_t>(index);
}

void RenderContext::Impl::RetireDestinationRecords(const Window* owner) noexcept {
    if (owner == nullptr) {
        // A null owner is the render-to-texture family (not owned by a window);
        // retiring "everything without a window" is never what a caller means.
        return;
    }
    for (RenderTargetRecord& record: renderTargets) {
        if (record.window != owner) {
            continue;
        }
        // Neutralize in place: the slot index stays allocated so no other
        // destination's recordSlots entry shifts, but every handle and image
        // it named is gone. ResolveAttachment already rejects a mismatch.
        record.handle        = TextureHandle::Invalid;
        record.serial        = 0;
        record.image         = VK_NULL_HANDLE;
        record.view          = VK_NULL_HANDLE;
        record.bindlessIndex = 0;
        record.generation    = 0;
        record.trackedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        record.writtenThisFrame = false;
    }
}

auto RenderContext::Impl::LiveGenerationFor(const Window& aux) noexcept -> uint64_t {
    if (auto* dest = FindDestination(aux); dest != nullptr) {
        return dest->Session().presentation.resourceGeneration;
    }
    return 0;
}

auto RenderContext::Impl::FindOrCreateDestination(Window& aux, bool primary) noexcept -> DestinationWindow* {
    if (auto* existing = FindDestination(aux); existing != nullptr) {
        return existing;
    }
    if (destinationWindows.size() >= kMaxDestinationWindows) {
        ZHLN::Log("[Render] Destination registry full; ignoring window.");
        return nullptr;
    }

    DestinationWindow dest {};
    dest.window = &aux;

    if (primary) {
        // The primary window's session belongs to the renderer; the entry only
        // borrows it.
        dest.session = &session;
    } else {
        using Vk::SurfaceCreationError;

        if (presentationMode != PresentationMode::NativeSwapchain) {
            return nullptr;
        }
        if (ctx.Device() == VK_NULL_HANDLE) {
            return nullptr;
        }

        int  width      = 0;
        int  height     = 0;
        auto surfaceRes = aux.CreateVulkanSurface(ctx.Instance(), ctx.Physical(), width, height);
        if (!surfaceRes) {
            ZHLN::Log("[Render] Destination surface creation failed ({})", surfaceRes.error());
            return nullptr;
        }

        auto owned = std::make_unique<Vk::SwapchainSession>();
        owned->surface = Vk::Surface(ctx.Instance(), static_cast<VkSurfaceKHR>(*surfaceRes));
        if (owned->surface.Get() == VK_NULL_HANDLE || width <= 0 || height <= 0) {
            return nullptr;
        }
        if (auto initRes = owned->Init(ctx, allocator, static_cast<uint32_t>(width), static_cast<uint32_t>(height), ctx.PhysicalInfo().graphics_family, true);
            !initRes) {
            ZHLN::Log("[Render] Destination swapchain init failed ({})", initRes.error());
            return nullptr;
        }
        // Blitting between destinations requires one present format; a window
        // whose surface disagrees cannot be a destination.
        if (owned->presentation.GetPresentFormat() != session.presentation.GetPresentFormat()) {
            // Present format is part of the swapchain, not of this API: a
            // window whose surface disagrees simply cannot be a destination.
            ZHLN::Log("[Render] Destination refused: present format differs from the primary swapchain.");
            return nullptr;
        }
        dest.session      = owned.get();
        dest.ownedSession = std::move(owned);
    }

    destinationWindows.push_back(std::move(dest));
    // Say which session the new destination uses. A window that is not the
    // renderer's primary one owns its session, and a frame that renders into it
    // is not the frame the primary session presents -- which is worth a line,
    // because from the outside that is a black window with no other symptom.
    ZHLN::Log(
        "[Render] Destination created for window {:p} (primary={}); {}", static_cast<const void*>(destinationWindows.back().window), primary ? 1 : 0,
        primary ? "borrowing the renderer's session" : "owning its own session"
    );
    return &destinationWindows.back();
}

// ============================================================================
// Image acquisition and command-buffer ownership
// ============================================================================

auto RenderContext::Impl::AcquireDestinationImage(DestinationWindow& dest) noexcept -> uint32_t {
    if (dest.imageAcquired) {
        const uint32_t slot = dest.imageIndex < dest.recordSlots.size() ? dest.recordSlots[dest.imageIndex] : 0;
        return slot;
    }
    if (dest.session == nullptr || dest.window == nullptr) {
        return 0;
    }

    Vk::SwapchainSession& sess = dest.Session();
    const uint32_t        slot = sess.frameIndex;

    // A caller-owned window can be resized between frames; the primary window
    // is rebuilt by BeginFrame (RecreateTargets) because its resize also
    // recreates every internal target.
    if (!dest.IsPrimary() && sess.presentation.swapchain.Valid()) {
        const Extent2D size = dest.window->GetSize();
        const VkExtent2D scExtent = sess.presentation.swapchain.Get().extent;
        if (size.width != 0 && size.height != 0 && (size.width != scExtent.width || size.height != scExtent.height)) {
            if (!sess.presentation.Rebuild(size.width, size.height)) {
                return 0;
            }
            // The rebuild bumped the generation; the check below retires this
            // destination's records and drops their slots.
        }
    }

    // Any rebuild -- BeginFrame's RecreateTargets on a resize, a present-time
    // Suboptimal/OutOfDate, a caller's own Rebuild -- replaces the images this
    // destination's records were built from. The generation counter catches all
    // of them, including the headless case where the swapchain handle stays
    // null and the offscreen target is quietly swapped underneath us.
    const uint64_t liveGeneration = sess.presentation.resourceGeneration;
    if (dest.cachedGeneration != liveGeneration) {
        if (dest.cachedGeneration != 0) {
            // Say so: a record retired out from under a caller is exactly the
            // class of bug that otherwise shows up as a driver complaint about
            // an invalid image view, or as a blank capture, with nothing in the
            // log tying the two together.
            ZHLN::Log(
                "[Render] Destination resources rebuilt (generation {} -> {}); re-vending the window's image.", dest.cachedGeneration,
                liveGeneration
            );
            RetireDestinationRecords(dest.window);
            dest.recordSlots.clear();
        }
        dest.cachedGeneration = liveGeneration;
    }

    if (sess.presentation.swapchain.Valid()) {
        if (sess.sync.Wait(slot) == VK_ERROR_DEVICE_LOST) {
            Vk::Instance::NotifyDeviceLost();
            return 0;
        }
        sess.sync.ResetFence(slot);
        sess.pools[slot].Reset();

        const auto&   sc   = sess.presentation.swapchain.Get();
        uint32_t      imageIndex = 0;
        ZHLN_AcquireDesc acquire {
            .swapchain       = sc.handle,
            .image_available = sess.sync[slot].image_available,
            .timeout_ns      = UINT64_MAX,
        };
        const ZHLN_FrameResult res = ZHLN_AcquireImage(ctx.Device(), &acquire, &imageIndex);
        if (res == ZHLN_FrameResult_OutOfDate) {
            const Extent2D size = dest.window->GetSize();
            if (size.width != 0 && size.height != 0) {
                if (!sess.presentation.Rebuild(size.width, size.height)) {
                    ZHLN::Log("[Render] Destination rebuild failed; the frame skips this window.");
                }
            }
            RetireDestinationRecords(dest.window);
            dest.recordSlots.clear();
            dest.cachedGeneration = sess.presentation.resourceGeneration;
            return 0;
        }
        if (res != ZHLN_FrameResult_Ok && res != ZHLN_FrameResult_Suboptimal) {
            return 0;
        }
        dest.imageIndex = imageIndex;

        if (dest.recordSlots.size() < sc.image_count) {
            dest.recordSlots.resize(sc.image_count, 0);
        }
        if (dest.recordSlots[imageIndex] == 0) {
            const uint32_t recordIndex = RegisterRenderTarget(RenderTargetRecord {
                .handle      = TextureHandle::Invalid,
                .bindlessIndex = 0,
                .image       = sc.images[imageIndex],
                .view        = sc.views[imageIndex],
                .extent      = {.width = sc.extent.width, .height = sc.extent.height, .depth = 1},
                .format      = sc.format,
                .presentable = true,
                .generation  = sess.presentation.resourceGeneration,
                .window      = dest.window,
            });
            dest.recordSlots[imageIndex] = recordIndex + 1;
        }
    } else {
        // Headless: no surface, no acquire. The frame renders into the
        // presentation context's offscreen color target and nothing is
        // presented, so the same call site works with no window system.
        if (sess.sync.Wait(slot) == VK_ERROR_DEVICE_LOST) {
            Vk::Instance::NotifyDeviceLost();
            return 0;
        }
        sess.sync.ResetFence(slot);
        sess.pools[slot].Reset();
        dest.imageIndex = 0;

        if (dest.recordSlots.empty()) {
            dest.recordSlots.resize(1, 0);
        }
        if (dest.recordSlots[0] == 0) {
            auto& target = sess.presentation.headlessColorTarget;
            if (!target.Valid()) {
                return 0;
            }
            const uint32_t recordIndex = RegisterRenderTarget(RenderTargetRecord {
                .handle        = TextureHandle::Invalid,
                .bindlessIndex = 0,
                .image         = target.image.Handle(),
                .view          = target.view.Get(),
                .extent        = {.width = target.extent.width, .height = target.extent.height, .depth = 1},
                .format        = VK_FORMAT_R8G8B8A8_UNORM,
                .presentable   = false,
                .generation    = sess.presentation.resourceGeneration,
                .window        = dest.window,
            });
            dest.recordSlots[0] = recordIndex + 1;
        }
    }

    // Freshly acquired swapchain contents are undefined; a record's tracked
    // layout starts over so the first pass this frame knows it may discard.
    const uint32_t recordIndex = dest.recordSlots[dest.imageIndex] - 1;
    renderTargets[recordIndex].writtenThisFrame = false;
    renderTargets[recordIndex].trackedLayout    = VK_IMAGE_LAYOUT_UNDEFINED;

    dest.imageAcquired = true;
    dest.openCmd       = sess.pools.Cmd(slot);
    dest.commandOpen   = true;
    ZHLN_BeginCommandBuffer(dest.openCmd);
    return dest.recordSlots[dest.imageIndex];
}

auto RenderContext::Impl::ResolveAttachment(const RenderAttachment& attachment) noexcept -> std::optional<RenderTargetRecord> {
    if (!attachment.Valid()) {
        return std::nullopt;
    }
    const auto decoded = DecodeRenderHandle(static_cast<uint64_t>(attachment.texture));
    if (!decoded.has_value() || decoded->index >= renderTargets.size()) {
        return std::nullopt;
    }
    const RenderTargetRecord& record = renderTargets[decoded->index];
    // Slot identity, not just slot number: a record retired since this handle
    // was vended has serial 0, and a different image living in the same slot
    // has a different one.
    if (record.serial != decoded->serial) {
        return std::nullopt;
    }
    // A window-backed record is only valid while the presentation resources it
    // was built from are still the live ones. Falling through here after a
    // rebuild would bind a destroyed VkImage/VkImageView, which is a
    // use-after-free the driver reports as an invalid handle at best and
    // segfaults on at worst -- so refuse, loudly, and let the caller draw
    // nothing this frame.
    if (record.window != nullptr && record.generation != LiveGenerationFor(*record.window)) {
        ZHLN::Log("[Render] Attachment from a retired presentation generation; pass skipped.");
        return std::nullopt;
    }
    return record;
}

void RenderContext::Impl::NoteAttachmentWritten(const RenderAttachment& attachment, VkImageLayout layout) noexcept {
    if (!attachment.Valid()) {
        return;
    }
    const auto decoded = DecodeRenderHandle(static_cast<uint64_t>(attachment.texture));
    if (!decoded.has_value() || decoded->index >= renderTargets.size()) {
        return;
    }
    RenderTargetRecord& record = renderTargets[decoded->index];
    if (record.serial != decoded->serial) {
        return;
    }
    record.writtenThisFrame = true;
    record.trackedLayout    = layout;
    // A frame that writes its destination again re-arms the unwritten warning,
    // so the next episode is reported too.
    warnedUnwrittenDestination = false;
}

auto RenderContext::Impl::ActiveDestinationRecord() noexcept -> std::optional<RenderTargetRecord> {
    if (activeDestinationWindow == nullptr) {
        return std::nullopt;
    }
    DestinationWindow* dest = FindDestination(*activeDestinationWindow);
    if (dest == nullptr || !dest->imageAcquired || dest->recordSlots.empty()) {
        return std::nullopt;
    }
    const uint32_t slot = dest->recordSlots[dest->imageIndex];
    if (slot == 0 || slot - 1 >= renderTargets.size()) {
        return std::nullopt;
    }
    const RenderTargetRecord& record = renderTargets[slot - 1];
    // A retired slot keeps its index but loses its image, view and serial.
    if (record.serial == 0 || record.image == VK_NULL_HANDLE) {
        return std::nullopt;
    }
    return record;
}

void RenderContext::Impl::FillUnwrittenDestinations() noexcept {
    for (DestinationWindow& dest: destinationWindows) {
        if (!dest.imageAcquired || dest.recordSlots.empty()) {
            continue;
        }
        const uint32_t slot = dest.recordSlots[dest.imageIndex];
        if (slot == 0 || slot - 1 >= renderTargets.size()) {
            continue;
        }
        RenderTargetRecord& record = renderTargets[slot - 1];
        if (record.writtenThisFrame || record.image == VK_NULL_HANDLE || record.view == VK_NULL_HANDLE) {
            continue;
        }

        if (dest.commandOpen && dest.openCmd != VK_NULL_HANDLE) {
            const VkClearColorValue clear {
                .float32 = {kClearColorScene.r, kClearColorScene.g, kClearColorScene.b, kClearColorScene.a},
            };
            Vk::ClearColorImage(dest.openCmd, record.image, clear);
            record.trackedLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        } else {
            record.trackedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        record.writtenThisFrame = true;

        if (!warnedUnwrittenDestination) {
            ZHLN::Log(
                "[Render] Destination 0x{:016X} was vended but no pass wrote it this frame; filled with the background colour.",
                static_cast<uint64_t>(record.handle)
            );
            warnedUnwrittenDestination = true;
        }
    }
}

auto RenderContext::Impl::VendedWindowAttachment(const Window& aux) noexcept -> RenderAttachment {
    // Vending an attachment acquires an image and opens the frame's command
    // buffer; both belong to a frame. Outside BeginFrame/EndFrame there is no
    // frame to own them, so hand back an empty attachment instead of recording
    // into a pool nobody reset.
    if (!activeQueueGuard.has_value()) {
        ZHLN::Log("[Render] GetWindowAttachment outside BeginFrame/EndFrame; attachment refused.");
        return {};
    }

    DestinationWindow* dest = FindOrCreateDestination(const_cast<Window&>(aux), &aux == &window);
    if (dest == nullptr) {
        return {};
    }
    activeDestinationWindow = dest->window;

    const uint32_t slot = AcquireDestinationImage(*dest);
    if (slot == 0) {
        return {};
    }
    current_cmd = dest->openCmd;
    current_image_index = dest->imageIndex;
    return RenderAttachment {.texture = renderTargets[slot - 1].handle, .mipLevel = 0, .arrayLayer = 0};
}

void RenderContext::Impl::ReleaseWindow(const Window& aux) noexcept {
    const auto it = std::find_if(destinationWindows.begin(), destinationWindows.end(), [&](const DestinationWindow& dest) { return dest.window == &aux; });
    if (it == destinationWindows.end()) {
        return;
    }
    if (it->IsPrimary()) {
        // The primary window's session outlives every caller; releasing it is a
        // no-op rather than a teardown of the renderer's own presentation.
        return;
    }

    const Window* released = it->window;
    if (ctx.Device() != VK_NULL_HANDLE) {
        // The released window's swapchain and records are about to die; the
        // device must be idle first. A lost device reports itself through the
        // next frame, so the result is deliberately dropped here.
        static_cast<void>(Vk::WaitIdle(ctx.Device()));
    }
    destinationWindows.erase(it);

    RetireDestinationRecords(released);
    if (activeDestinationWindow == released) {
        activeDestinationWindow = nullptr;
    }
}

void RenderContext::Impl::DestroyDestinations() noexcept {
    if (ctx.Device() != VK_NULL_HANDLE) {
        static_cast<void>(Vk::WaitIdle(ctx.Device()));
    }
    destinationWindows.clear();
    renderTargets.clear();
    activeDestinationWindow = nullptr;
}

// ============================================================================
// Render-to-texture
// ============================================================================

auto RenderContext::Impl::CreateRenderTexture(uint32_t width, uint32_t height, bool hdr) noexcept -> std::expected<TextureHandle, ErrorCode> {
    if (width == 0 || height == 0 || ctx.Device() == VK_NULL_HANDLE) {
        return std::unexpected(Vk::DescriptorHeapError::AllocationFailed);
    }

    const VkFormat format = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    const Vk::ImageUsage usage = Vk::ImageUsage::ColorAttachment | Vk::ImageUsage::Sampled | Vk::ImageUsage::TransferSrc;

    auto imageRes = Vk::ImageBuilder {}.Texture2D(width, height, format, usage, 1).Build(allocator.Get());
    if (!imageRes) {
        return std::unexpected(imageRes.error());
    }
    auto image = std::move(*imageRes);

    auto viewRes = Vk::CreateView(ctx.Device(), image.Handle(), format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    if (!viewRes) {
        return std::unexpected(viewRes.error());
    }
    auto view = std::move(*viewRes);

    const VkImage     rawImage = image.Handle();
    const VkImageView rawView  = view.Get();

    // The render texture is published in the bindless texture array, so a
    // material may sample what a previous pass rendered into it. That is the
    // whole point of the RTT API: the handle addresses a subresource *and*
    // resolves to a descriptor.
    auto bindless = AdoptBindlessTexture(std::move(image), std::move(view), format, 1, false);
    if (!bindless) {
        return std::unexpected(bindless.error());
    }

    const uint32_t recordIndex = RegisterRenderTarget(RenderTargetRecord {
        .handle        = TextureHandle::Invalid,
        .bindlessIndex = *bindless,
        .image         = rawImage,
        .view          = rawView,
        .extent        = {.width = width, .height = height, .depth = 1},
        .format        = format,
        .presentable   = false,
        .window        = nullptr,
    });

    // `image`/`view` are owned by the bindless arrays from here on; the record
    // only references them. Stamp the record's handle so the caller can address
    // it and release it later.
    return renderTargets[recordIndex].handle;
}

void RenderContext::Impl::DestroyRenderTexture(TextureHandle handle) noexcept {
    const auto decoded = DecodeRenderHandle(static_cast<uint64_t>(handle));
    if (!decoded.has_value() || decoded->index >= renderTargets.size()) {
        return;
    }
    RenderTargetRecord& record = renderTargets[decoded->index];
    if (record.serial != decoded->serial) {
        return;
    }

    // The texture may still be sampled by an in-flight frame, so hand the
    // bindless slot back to the deferred-release path instead of destroying it
    // here; ReclaimTextureSlots neutralizes the descriptor at the next frame
    // boundary for this parity.
    const uint32_t bindlessIndex = record.bindlessIndex;
    if (bindlessIndex > kFallbackNormalTextureIndex) {
        ReleaseBindlessTexture(bindlessIndex);
    }
    // Retire the slot rather than erasing it: every later record keeps its
    // index, so handles already handed to callers stay valid -- and stay
    // rejected, because the serial no longer matches.
    record.handle           = TextureHandle::Invalid;
    record.serial           = 0;
    record.image            = VK_NULL_HANDLE;
    record.view             = VK_NULL_HANDLE;
    record.bindlessIndex    = 0;
    record.trackedLayout    = VK_IMAGE_LAYOUT_UNDEFINED;
    record.writtenThisFrame = false;
}

// ============================================================================
// Presentation
// ============================================================================

auto RenderContext::Impl::PresentUsedWindows() noexcept -> std::expected<void, ErrorCode> {
    using enum RenderFrameResult;

    std::expected<void, ErrorCode> result {};

    for (auto& dest: destinationWindows) {
        if (!dest.imageAcquired) {
            continue;
        }
        Vk::SwapchainSession& sess = dest.Session();
        const uint32_t        slot = sess.frameIndex;

        const bool presents = sess.presentation.swapchain.Valid();

        // 1. Move the destination into the layout presentation requires. This
        //    has to happen *while the frame's command buffer is still
        //    recording* and before it is submitted: vkQueuePresentKHR requires
        //    the image in PRESENT_SRC_KHR, and this barrier is the only one
        //    that puts it there -- no pass transitions into a present layout,
        //    because a pass does not know whether its target is a swapchain
        //    image or a render texture. The tracked layout is what the last
        //    pass left behind; a vended image nothing wrote is UNDEFINED, and
        //    UNDEFINED as the oldLayout is always legal because the contents
        //    are don't-care.
        if (presents && dest.openCmd != VK_NULL_HANDLE && dest.imageIndex < dest.recordSlots.size() && dest.recordSlots[dest.imageIndex] != 0) {
            const uint32_t recordIndex = dest.recordSlots[dest.imageIndex] - 1;
            RenderTargetRecord& record = renderTargets[recordIndex];
            if (record.image != VK_NULL_HANDLE && record.trackedLayout != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) {
                const VkImageMemoryBarrier2 barrier = Vk::MakeImageBarrier({
                    .image      = record.image,
                    .src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .dst_access = 0,
                    .src_layout = record.trackedLayout,
                    .dst_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    .src_stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .dst_stage  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                    .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .base_mip   = 0,
                    .mip_count  = VK_REMAINING_MIP_LEVELS,
                });
                Vk::PipelineBarrier(
                    dest.openCmd, std::span<const VkBufferMemoryBarrier2> {}, std::span<const VkImageMemoryBarrier2> {&barrier, 1}
                );
                record.trackedLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            }
        }

        // 2. Close the window's command buffer. Everything the frame records --
        //    the scene, the overlay, the present transition above -- is now in
        //    it, so this is the last chance to touch it. Recording a barrier
        //    after this point leaves the transition out of the submitted
        //    stream, and the validation layer's complaint about it is a crash
        //    inside the layer rather than a readable error.
        if (dest.commandOpen) {
            ZHLN_EndCommandBuffer(dest.openCmd);
            dest.commandOpen = false;
        }

        // 3. Submit: wait the image-available semaphore, the transfer ring, and
        //    the frame's compute timeline; signal the per-image present
        //    semaphore and this slot's fence.
        const ZHLN_FrameSync& sync = sess.sync[slot];
        const uint64_t        computeValue = sess.sync.GetTimelineValue(slot);
        const uint64_t        stagingValue = transferRingBuffer.GetCurrentValue();

        std::array<VkSemaphoreSubmitInfo, 3> waits {};
        uint32_t                             waitCount = 0;
        if (presents) {
            waits[waitCount++] = MakeSemSubmit(sync.image_available, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        if (transferRingBuffer.GetSemaphore() != VK_NULL_HANDLE && stagingValue > 0) {
            waits[waitCount++] = MakeSemSubmit(transferRingBuffer.GetSemaphore(), stagingValue, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        }
        if (sync.compute_timeline != VK_NULL_HANDLE && computeValue > 0 && computeSubmittedThisFrame) {
            waits[waitCount++] = MakeSemSubmit(sync.compute_timeline, computeValue, Vk::kAsyncComputeConsumerStages);
        }

        const VkSemaphore presentSem = presents ? sess.presentation.presentSemaphores[dest.imageIndex] : VK_NULL_HANDLE;
        const VkSemaphoreSubmitInfo signal = MakeSemSubmit(presentSem, 0, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);

        const VkCommandBufferSubmitInfo cmdInfo = MakeCmdSubmit(dest.openCmd);
        auto submitRes = Vk::QueueSubmit(
            ctx.GraphicsQueue(), std::span<const VkCommandBufferSubmitInfo> {&cmdInfo, 1},
            std::span<const VkSemaphoreSubmitInfo> {waits.data(), waitCount}, std::span<const VkSemaphoreSubmitInfo> {&signal, presents ? 1u : 0u},
            sync.in_flight
        );
        if (!submitRes) [[unlikely]] {
            if (submitRes.error().Is(Vk::VulkanCallError::DeviceLost)) {
                Vk::Instance::NotifyDeviceLost();
                return std::unexpected(RenderFrameResult::DeviceLost);
            }
            return std::unexpected(submitRes.error());
        }

        // 4. Present.
        if (presents) {
            const ZHLN_PresentDesc present {
                .present_queue   = ctx.PresentQueue(),
                .swapchain       = sess.presentation.swapchain.Get().handle,
                .render_finished = presentSem,
                .image_index     = dest.imageIndex,
            };
            const ZHLN_FrameResult presented = Vk::PresentFrame(present);
            if (presented == ZHLN_FrameResult_OutOfDate || presented == ZHLN_FrameResult_Suboptimal) {
                const Extent2D size = dest.window != nullptr ? dest.window->GetSize() : Extent2D {};
                if (size.width != 0 && size.height != 0) {
                    if (!sess.presentation.Rebuild(size.width, size.height)) {
                        ZHLN::Log("[Render] Destination rebuild after present failed; retrying next frame.");
                    }
                }
                RetireDestinationRecords(dest.window);
                dest.recordSlots.clear();
                dest.cachedGeneration = sess.presentation.resourceGeneration;
                result = std::unexpected(RenderFrameResult::Suboptimal);
            } else if (presented == ZHLN_FrameResult_DeviceLost) {
                Vk::Instance::NotifyDeviceLost();
                return std::unexpected(RenderFrameResult::DeviceLost);
            } else if (presented != ZHLN_FrameResult_Ok) {
                return std::unexpected(RenderFrameResult::Error);
            }
        }

        // 5. Retire the acquisition and advance this window's own parity. The
        //    timeline value is deliberately *not* stepped here: BeginFrame does
        //    that for the primary schedule, and an extra window never records
        //    compute, so its timeline stays 0 and adds no wait.
        dest.imageAcquired = false;
        dest.openCmd       = VK_NULL_HANDLE;
        sess.frameIndex    = (slot + 1) & 1u;
    }

    return result;
}

} // namespace ZHLN
