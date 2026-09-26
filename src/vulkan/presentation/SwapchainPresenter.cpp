// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/presentation/SwapchainPresenter.cpp
//
// The three verbs in SwapchainPresenter.hpp, plus the bring-up they need. The
// swapchain construction is the code that used to be PresentationContext, moved
// here rather than rewritten: it was already correct, and the point of the move
// is who owns it, not what it does.

#include "Rendering.hpp" // the umbrella this header is written against (see GPUDiagnostics.cpp's order)
#include "SwapchainPresenter.hpp"

#include <array>
#include <cstdint>

namespace ZHLN::Vk {

// Bring-up

auto SwapchainPresenter::Init(const Context& ctx, Allocator& alloc, uint32_t width, uint32_t height, uint32_t graphicsFamily, bool vsync)
    -> std::expected<void, ErrorCode> {
    _ctx   = &ctx;
    _alloc = &alloc;
    _vsync = vsync;

    // The pacing policy resolves before the first swapchain exists: it decides
    // the present mode and the TIMING_BIT the description below carries. The
    // surface was assigned by the caller before Init (VK_NULL_HANDLE headless).
    _pacer.Resolve(ctx, surface.Get(), vsync);

    sync  = FrameSync<2>::Create(ctx.Device());
    pools = CommandPools<2, QueueType::Graphics>::Create(ctx.Device(), {.queueFamily = graphicsFamily, .buffersPerPool = 1});
    frameIndex = 0;
    if (!sync.Valid() || !pools.Valid()) {
        return std::unexpected(PresentationError::SyncCreationFailed);
    }

    return Rebuild(width, height);
}

auto SwapchainPresenter::Rebuild(uint32_t width, uint32_t height) -> std::expected<void, ErrorCode> {
    if ((_ctx == nullptr) || (_alloc == nullptr)) {
        return std::unexpected(PresentationError::ContextInvalid);
    }

    auto idle_res = Vk::WaitIdle(_ctx->Device());
    if (!idle_res) {
        return std::unexpected(idle_res.error());
    }

    // In headless mode (no surface), skip swapchain construction entirely.
    // Allocate a depth target and a color target using the requested render
    // extent for offscreen rendering.
    if (surface.Get() == VK_NULL_HANDLE) {
        const VkExtent2D renderExtent = {.width = width, .height = height};
        {
            auto dt_res = RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>::Create(
                *_alloc, *_ctx, renderExtent, {.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled}
            );
            if (!dt_res) {
                return std::unexpected(dt_res.error());
            }
            depthTarget = std::move(*dt_res);
        }

        // Headless offscreen color target for the Blit pass output
        {
            auto hct_res = RenderTarget<kHeadlessColorFormat>::Create(
                *_alloc, *_ctx, renderExtent, {.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled | ImageUsage::TransferSrc}
            );
            if (!hct_res) {
                return std::unexpected(hct_res.error());
            }
            headlessColorTarget = std::move(*hct_res);
        }

        // Both offscreen targets above are new handles for everyone caching one.
        ++resourceGeneration;
        // Headless: no swapchain to arm the closed loop against, so this only
        // seals the policy Resolve already picked (Decoupled or LegacyVBlank).
        _pacer.OnSwapchainRebuilt(VK_NULL_HANDLE, VK_NULL_HANDLE, 0, VK_PRESENT_MODE_MAX_ENUM_KHR);
        return {};
    }

    const ZHLN_Device raw_dev = {
        .handle         = _ctx->Device(),
        .graphics_queue = _ctx->GraphicsQueue(),
        .present_queue  = _ctx->PresentQueue(),
        .transfer_queue = _ctx->TransferQueue(),
        .compute_queue  = _ctx->ComputeQueue()
    };
    const ZHLN_PhysicalDeviceInfo raw_phys = _ctx->PhysicalInfo();
    ZHLN_SwapchainDesc            s_desc   = {
        .device                = &raw_dev,
        .physical              = &raw_phys,
        .surface               = surface.Get(),
        .width                 = width,
        .height                = height,
        .vsync                 = _vsync,
        .present_mode          = _pacer.RequestedPresentMode(),
        .enable_present_timing = _pacer.WantsPresentTiming(),
        .old_swapchain         = swapchain.Get().handle,
    };

    if (!swapchain.Rebuild(s_desc)) {
        return std::unexpected(PresentationError::SwapchainCreationFailed);
    }
    // The closed loop re-arms against the new swapchain -- queue size, time
    // domain, timing properties, restarted ids and baseline -- or, on the
    // first build, confirms the provisional policy against the actual present
    // mode (a fallback away from the FIFO family downgrades it to adaptive).
    _pacer.OnSwapchainRebuilt(_ctx->Device(), swapchain.Get().handle, swapchain.Get().image_count, swapchain.Get().present_mode);
    presentSemaphores.Rebuild(_ctx->Device(), swapchain.Get().image_count);

    // Automatically recreate the depth buffer to match the new swapchain extent
    {
        auto dt_res = RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>::Create(
            *_alloc, *_ctx, swapchain.Get().extent, {.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled}
        );
        if (!dt_res) {
            return std::unexpected(dt_res.error());
        }
        depthTarget = std::move(*dt_res);
    }

    // New swapchain images, new present semaphores, new depth target: every
    // handle a caller or a record may hold is now stale.
    ++resourceGeneration;

    return {};
}

// Acquire

auto SwapchainPresenter::AcquireNext(VkExtent2D desiredExtent, bool allowRebuild) noexcept -> FrameOutcome<SwapchainTarget> {
    if (_ctx == nullptr) {
        return std::unexpected(PresentationError::ContextInvalid);
    }

    const uint32_t slot = frameIndex;

    // A caller-owned window can be resized between frames. The primary window's
    // resize is the renderer's to handle (its own targets are tied to the
    // extent), so the caller says whether a rebuild is this call's to do.
    if (allowRebuild && swapchain.Valid() && desiredExtent.width != 0 && desiredExtent.height != 0) {
        const VkExtent2D current = swapchain.Get().extent;
        if (desiredExtent.width != current.width || desiredExtent.height != current.height) {
            if (auto rebuilt = Rebuild(desiredExtent.width, desiredExtent.height); !rebuilt) {
                return std::unexpected(rebuilt.error());
            }
            // The rebuild bumped the generation; the caller retires what it
            // cached against the old one.
        }
    }

    // The fence wait's own result, mapped the one way the frame path maps
    // results: FrameResult::DeviceLost for a lost device (with an infinite
    // timeout that is the practical case), the driver's code otherwise.
    if (const VkResult waited = sync.Wait(slot); waited != VK_SUCCESS) {
        return std::unexpected(ToFrameError(waited));
    }
    sync.ResetFence(slot);
    pools[slot].Reset();

    if (!swapchain.Valid()) {
        // Headless: no surface, no acquire. The frame renders into the
        // presentation context's offscreen color target and nothing is
        // presented, so the same call site works with no window system.
        auto& target = headlessColorTarget;
        if (!target.Valid()) {
            // No Vulkan call to quote here: this is the presenter's own state,
            // not a result.
            return std::unexpected(PresentationError::OffscreenTargetUnavailable);
        }
        return SwapchainTarget {
            .image       = MakeSlice(target.image.Handle(), target.view.Get(), target.extent, kHeadlessColorFormat),
            .imageIndex  = 0,
            .slot        = slot,
            .generation  = resourceGeneration,
            .presentable = false,
        };
    }

    const auto& sc = swapchain.Get();
    uint32_t    imageIndex = 0;
    ZHLN_AcquireDesc acquire {
        .swapchain       = sc.handle,
        .image_available = sync[slot].image_available,
        .timeout_ns      = UINT64_MAX,
    };
    // The call's own result. VK_SUBOPTIMAL_KHR is not a failure here: Vulkan
    // still hands over a usable image, and the present path is where
    // suboptimality becomes actionable. What reaches the caller is either the
    // image it vended (suboptimal or not) or, for an out-of-date swapchain, no
    // image at all -- nothing was vended, the rebuild below is what this call
    // could do about it, and the caller draws again next frame.
    const VkResult res = ZHLN_AcquireImage(_ctx->Device(), &acquire, &imageIndex);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        if (res == VK_ERROR_OUT_OF_DATE_KHR && desiredExtent.width != 0 && desiredExtent.height != 0) {
            (void)Rebuild(desiredExtent.width, desiredExtent.height);
        }
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            return std::nullopt;
        }
        return std::unexpected(ToFrameError(res));
    }

    // Observer: an image is now in flight, so past presents have had a chance
    // to complete -- drain their feedback into the scheduling baseline and
    // the slack margin before this frame's present is aimed. Never blocks.
    if (_pacer.IsTimingActive()) {
        _pacer.Observe(_ctx->Device(), sc.handle);
    }

    return SwapchainTarget {
        .image       = MakeSlice(sc.images[imageIndex], sc.views[imageIndex], sc.extent, sc.format),
        .imageIndex  = imageIndex,
        .slot        = slot,
        .generation  = resourceGeneration,
        .presentable = true,
    };
}

// Present

auto SwapchainPresenter::Present(
    VkQueue graphicsQueue, VkQueue presentQueue, VkCommandBuffer cmd, uint32_t imageIndex, VkImageLayout currentLayout,
    std::span<const VkSemaphoreSubmitInfo> extraWaits
) noexcept -> FrameOutcome<PresentSuboptimal> {
    const bool     presents = swapchain.Valid();
    const uint32_t slot     = frameIndex;

    // 1. Move the destination into the layout presentation requires. This is
    //    the presenter's call to make and nobody else's: it is the only code
    //    that knows the image belongs to a swapchain at all, and the only code
    //    that submits the stream the transition has to be part of. It
    //    therefore also has to happen *while the frame's command buffer is
    //    still recording*, which is why this call ends the recording below.
    //
    //    The source layout is whatever the last writer left, mapped from the
    //    vocabulary a pass speaks (AttachmentLayout): from Undefined -- a
    //    vended image no pass wrote -- that is exactly VK_IMAGE_LAYOUT_
    //    UNDEFINED, which is always a legal oldLayout because the contents
    //    are don't-care.
    if (presents && cmd != VK_NULL_HANDLE) {
        const VkImageMemoryBarrier2 barrier = Vk::MakeImageBarrier({
            .image      = swapchain.Get().images[imageIndex],
            .src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dst_access = 0,
            .src_layout = currentLayout,
            .dst_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .src_stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dst_stage  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
            .base_mip   = 0,
            .mip_count  = VK_REMAINING_MIP_LEVELS,
        });
        Vk::PipelineBarrier(cmd, std::span<const VkBufferMemoryBarrier2> {}, std::span<const VkImageMemoryBarrier2> {&barrier, 1});
        // The image is presentable now, and this is the only writer of that
        // fact: it is not an AttachmentLayout, so no pass can reach it, and the
        // caller's tracked layout stays in the pass vocabulary. A later frame
        // re-vends from Undefined.
    }

    // 2. Close the command buffer. Everything the frame records is now in it,
    //    so this is the last chance to touch it. Recording a barrier after this
    //    point leaves the transition out of the submitted stream, and the
    //    validation layer's complaint about it is a crash inside the layer
    //    rather than a readable error.
    if (cmd != VK_NULL_HANDLE) {
        ZHLN_EndCommandBuffer(cmd);
    }

    // 3. Submit: wait the image-available semaphore (behind whatever else the
    //    caller's frame used), signal the per-image present semaphore and this
    //    slot's fence.
    const ZHLN_FrameSync& frameSync = sync[slot];

    std::array<VkSemaphoreSubmitInfo, 4> waits {};
    uint32_t                             waitCount = 0;
    if (presents) {
        waits[waitCount++] = Vk::MakeSemaphoreSubmitInfo(frameSync.image_available, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    for (const VkSemaphoreSubmitInfo& extra: extraWaits) {
        if (waitCount == waits.size()) {
            break;
        }
        waits[waitCount++] = extra;
    }

    const VkSemaphore            presentSem = PresentSemaphore(imageIndex);
    const VkSemaphoreSubmitInfo  signal     = Vk::MakeSemaphoreSubmitInfo(presentSem, 0, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
    const VkCommandBufferSubmitInfo cmdInfo = Vk::MakeCommandBufferSubmitInfo(cmd);

    auto submitRes = Vk::QueueSubmit(
        graphicsQueue, std::span<const VkCommandBufferSubmitInfo> {&cmdInfo, 1}, std::span<const VkSemaphoreSubmitInfo> {waits.data(), waitCount},
        std::span<const VkSemaphoreSubmitInfo> {&signal, presents ? 1u : 0u}, frameSync.in_flight
    );
    if (!submitRes) [[unlikely]] {
        // QueueSubmit carries the submit call's VkResult; passing it through is
        // the whole diagnostic (VK_ERROR_OUT_OF_HOST_MEMORY and
        // VK_ERROR_DEVICE_LOST are not the same news and no longer arrive as
        // one word).
        return std::unexpected(submitRes.error());
    }

    if (!presents) {
        // Headless: submitted, nothing to present.
        return {};
    }

    // 4. Present -- timed when the closed loop is active: the predictor aims
    //    this present at its V-blank with the next present id. The temporary
    //    Vulkan pNext chain is assembled in-place on the current stack frame.
    const VkPresentId2KHR*           presentId  = nullptr;
    std::optional<TimedPresentChain> timedChain;
    if (auto prediction = _pacer.Predict()) {
        timedChain.emplace(*prediction);
        presentId = &timedChain->presentId;
    }
    const ZHLN_PresentDesc present {
        .present_queue   = presentQueue,
        .swapchain       = swapchain.Get().handle,
        .render_finished = presentSem,
        .image_index     = imageIndex,
        .present_id      = presentId,
    };
    auto presented = Vk::PresentFrame(present);
    if (!presented && presentId != nullptr && presented.error().Is(VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT)) {
        // The timing queue filled faster than AcquireNext drains it -- a hitch
        // stalled acquires while presents kept flowing. Drain now and retry
        // once, untimed, so the frame still goes out this V-blank instead of
        // failing the frame; the consumed id is skipped, never reused.
        if (_ctx != nullptr) {
            _pacer.Observe(_ctx->Device(), swapchain.Get().handle);
        }
        const ZHLN_PresentDesc retry {
            .present_queue   = presentQueue,
            .swapchain       = swapchain.Get().handle,
            .render_finished = presentSem,
            .image_index     = imageIndex,
            .present_id      = nullptr,
        };
        presented = Vk::PresentFrame(retry);
    }
    if (!presented) {
        // A real error: FrameResult::DeviceLost, or the driver's own code.
        return std::unexpected(presented.error());
    } else if (presented->has_value()) {
        // The image did not go through as asked (PresentSuboptimal, and the
        // presenter rebuilt what it could in the acquire above -- or will have
        // its Rebuild called by the caller): not an error, and not this
        // function's to act on. It is carried out so EndFrame can tell the
        // caller the frame was drawn but not shown as asked.
        return PresentSuboptimal {};
    }
    return {};
}

} // namespace ZHLN::Vk
