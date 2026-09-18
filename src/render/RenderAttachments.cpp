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
#include "OpenGLHacks/HostBlit.hpp"
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

auto RenderContext::Impl::FindOrCreateDestination(Window& aux, bool primary) noexcept -> DestinationRegistry::WindowEntry* {
    if (auto* existing = destinations.Find(aux); existing != nullptr) {
        return existing;
    }
    if (destinations.Full()) {
        ZHLN::Log("[Render] Destination registry full; ignoring window.");
        return nullptr;
    }

    DestinationRegistry::WindowEntry dest {};
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

    destinations.Attach(std::move(dest));
    return destinations.Find(aux);
}

// ============================================================================
// Image acquisition and command-buffer ownership
// ============================================================================

auto RenderContext::Impl::AcquireDestinationImage(DestinationRegistry::WindowEntry& dest) noexcept -> uint32_t {
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
            destinations.Retire(dest.window);
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
            destinations.Retire(dest.window);
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
            const auto handle = destinations.Register(DestinationRegistry::Record {
                .bindlessIndex = 0,
                .image       = sc.images[imageIndex],
                .view        = sc.views[imageIndex],
                .extent      = {.width = sc.extent.width, .height = sc.extent.height, .depth = 1},
                .format      = sc.format,
                .presentable = true,
                .generation  = sess.presentation.resourceGeneration,
                .window      = dest.window,
            });
            dest.recordSlots[imageIndex] = handle.Index() + 1;
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
            const auto handle = destinations.Register(DestinationRegistry::Record {
                .bindlessIndex = 0,
                .image         = target.image.Handle(),
                .view          = target.view.Get(),
                .extent        = {.width = target.extent.width, .height = target.extent.height, .depth = 1},
                .format        = VK_FORMAT_R8G8B8A8_UNORM,
                .presentable   = false,
                .generation    = sess.presentation.resourceGeneration,
                .window        = dest.window,
            });
            dest.recordSlots[0] = handle.Index() + 1;
        }
    }

    // Freshly acquired swapchain contents are undefined; a record's tracked
    // layout starts over so the first pass this frame knows it may discard.
    DestinationRegistry::Record& record = destinations.Records()[dest.recordSlots[dest.imageIndex] - 1];
    record.writtenThisFrame = false;
    record.backgroundFilled = false;
    record.trackedLayout    = Vk::AttachmentLayout::Undefined;

    dest.imageAcquired = true;
    dest.openCmd       = sess.pools.Cmd(slot);
    dest.commandOpen   = true;
    ZHLN_BeginCommandBuffer(dest.openCmd);
    return dest.recordSlots[dest.imageIndex];
}

void RenderContext::Impl::FillUnwrittenDestinations() noexcept {
    for (DestinationRegistry::WindowEntry& dest: destinations.Windows()) {
        if (!dest.imageAcquired || dest.recordSlots.empty()) {
            continue;
        }
        const uint32_t slot = dest.recordSlots[dest.imageIndex];
        if (slot == 0 || slot - 1 >= destinations.Records().size()) {
            continue;
        }
        DestinationRegistry::Record& record = destinations.Records()[slot - 1];
        if (record.writtenThisFrame || record.image == VK_NULL_HANDLE || record.view == VK_NULL_HANDLE) {
            continue;
        }

        if (dest.commandOpen && dest.openCmd != VK_NULL_HANDLE) {
            const VkClearColorValue clear {
                .float32 = {kClearColorScene.r, kClearColorScene.g, kClearColorScene.b, kClearColorScene.a},
            };
            Vk::ClearColorImage(dest.openCmd, record.image, clear);
            record.trackedLayout = Vk::AttachmentLayout::ColorAttachment;
        } else {
            record.trackedLayout = Vk::AttachmentLayout::Undefined;
        }
        record.writtenThisFrame = true;
        // Mark the record as *filled*, not drawn: a capture or a test metric
        // reading this image would see the background colour and be right to
        // call the scene black -- except the scene was never in it.
        record.backgroundFilled = true;

        if (!destinations.UnwrittenWarned()) {
            ZHLN::Log(
                "[Render] Destination 0x{:016X} (extent {}x{}) was vended but no pass wrote it this frame; filled with the background colour.",
                record.handle.Raw(), record.extent.width, record.extent.height
            );
            destinations.NoteUnwrittenWarned();
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

    DestinationRegistry::WindowEntry* dest = FindOrCreateDestination(const_cast<Window&>(aux), &aux == &window);
    if (dest == nullptr) {
        return {};
    }
    destinations.SetActive(dest->window);

    const uint32_t slot = AcquireDestinationImage(*dest);
    if (slot == 0) {
        return {};
    }
    current_cmd = dest->openCmd;
    current_image_index = dest->imageIndex;
    return RenderAttachment {.texture = destinations.Records()[slot - 1].handle.AsTexture(), .mipLevel = 0, .arrayLayer = 0};
}

void RenderContext::Impl::ReleaseWindow(const Window& aux) noexcept {
    DestinationRegistry::WindowEntry* entry = destinations.Find(aux);
    if (entry == nullptr || entry->IsPrimary()) {
        // No destination at all, or the primary window's session, which
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

    const auto handle = destinations.Register(DestinationRegistry::Record {
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
    return handle.AsTexture();
}

void RenderContext::Impl::DestroyRenderTexture(TextureHandle handle) noexcept {
    const auto decoded = DestinationRegistry::Handle::FromTexture(handle);
    if (!decoded.has_value() || decoded->Index() >= destinations.Records().size()) {
        return;
    }
    DestinationRegistry::Record& record = destinations.Records()[decoded->Index()];
    if (record.serial != decoded->Serial()) {
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
    record.handle           = {};
    record.serial           = 0;
    record.image            = VK_NULL_HANDLE;
    record.view             = VK_NULL_HANDLE;
    record.bindlessIndex    = 0;
    record.trackedLayout    = Vk::AttachmentLayout::Undefined;
    record.writtenThisFrame = false;
    record.backgroundFilled = false;
}

// ============================================================================
// Presentation
// ============================================================================

auto RenderContext::Impl::PresentUsedWindows() noexcept -> std::expected<void, ErrorCode> {
    using enum RenderFrameResult;

    std::expected<void, ErrorCode> result {};

    for (auto& dest: destinations.Windows()) {
        if (!dest.imageAcquired) {
            continue;
        }
        Vk::SwapchainSession& sess = dest.Session();
        const uint32_t        slot = sess.frameIndex;

        const bool presents = sess.presentation.swapchain.Valid();

        // 1. Move the destination into the layout presentation requires. This
        //    is the presenter's call to make and nobody else's: it is the only
        //    code that knows the image belongs to a swapchain at all, and the
        //    only code that submits the stream the transition has to be part
        //    of. It therefore also has to happen *while the frame's command
        //    buffer is still recording* -- the barrier recorded below goes into
        //    the same buffer that is about to be ended and submitted.
        //
        //    The source layout is whatever the last writer left, mapped from the
        //    vocabulary a pass speaks (AttachmentLayout): from Undefined -- a
        //    vended image no pass wrote -- that is exactly VK_IMAGE_LAYOUT_
        //    UNDEFINED, which is always a legal oldLayout because the contents
        //    are don't-care.
        if (presents && dest.openCmd != VK_NULL_HANDLE && dest.imageIndex < dest.recordSlots.size() && dest.recordSlots[dest.imageIndex] != 0) {
            DestinationRegistry::Record& record = destinations.Records()[dest.recordSlots[dest.imageIndex] - 1];
            if (record.image != VK_NULL_HANDLE) {
                const VkImageMemoryBarrier2 barrier = Vk::MakeImageBarrier({
                    .image      = record.image,
                    .src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .dst_access = 0,
                    .src_layout = Vk::ToVkImageLayout(record.trackedLayout),
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
                // The image is presentable now, and this is the only writer of
                // that fact: it is not an AttachmentLayout, so no pass can
                // reach it, and the record's own field stays in the pass
                // vocabulary. A later frame re-vends from Undefined.
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
        const bool waitsOnCompute = sync.compute_timeline != VK_NULL_HANDLE && computeValue > 0 && computeSubmittedThisFrame;
        if (waitsOnCompute) {
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

        // 4. Host presentation (macOS). A destination with no swapchain has no
        //    vkQueuePresent to go through: in HostBlit mode the frame lives in
        //    the offscreen headless target and the plugin copies it out on its
        //    own fence -- which waits on the submit above -- and blits it
        //    through its own OpenGL window. Closing that window ends the
        //    session, exactly like closing any other engine window.
        if constexpr (isMac) {
            if (!presents && dest.IsPrimary() && presentationMode == PresentationMode::HostBlit && dest.window != nullptr) {
                auto& target = sess.presentation.headlessColorTarget;
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

        // 5. Present.
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
                destinations.Retire(dest.window);
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

        // 6. Retire the acquisition and advance this window's own parity. The
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
