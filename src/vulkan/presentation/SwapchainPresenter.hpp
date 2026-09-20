// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/presentation/SwapchainPresenter.hpp
//
// Presentation and nothing but presentation: one surface, the swapchain on it, the
// per-frame sync and command objects that submit into it, the depth and headless
// targets for its extent, and the three verbs that put an image on screen -- acquire,
// transition-to-present, present. The only place in the engine that knows an image
// belongs to a swapchain.
//
//   * No Window and no engine type appears here: a window is a VkSurfaceKHR by the
//     time it arrives. `HasSurface()` false is the headless case -- no swapchain, the
//     frame renders into `headlessColorTarget`, and `Present` submits without
//     presenting.
//   * The state is public on purpose (some ten renderer files read the depth target,
//     swapchain, frame index and timeline); the *verbs* are the interface.

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Error.hpp>
#include <Zahlen/FrameResult.hpp>
#include <cstdint>
#include <span>

namespace ZHLN::Vk {

// Swapchain/presentation bring-up failures, deliberately bring-up only: what the frame
// verbs fail on is a Vulkan call's result and travels as that VkResult inside
// ErrorCode. This enum covers the cases Vulkan has nothing to say about -- no device,
// a window that owns no presenter, a format that disagrees with the primary's.
enum class PresentationError : uint8_t {
    ContextInvalid ZHLN_ANNOTATION(ZHLN::Description<"Presentation context is missing a device or allocator">{}) = 1,
    SwapchainCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Swapchain creation failed">{}),
    NativeSwapchainRequired ZHLN_ANNOTATION(ZHLN::Description<"Native swapchain presentation is required">{}),
    PrimaryWindowAlreadyPresented ZHLN_ANNOTATION(ZHLN::Description<"Primary window already has a swapchain">{}),
    WindowNotPresented ZHLN_ANNOTATION(ZHLN::Description<"No viewport for this window">{}),
    SyncCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Frame sync or command pool creation failed">{}),
    PresentFormatMismatch ZHLN_ANNOTATION(ZHLN::Description<"Viewport present format does not match the primary swapchain">{}),
    OffscreenTargetUnavailable
        ZHLN_ANNOTATION(ZHLN::Description<"A headless destination has no offscreen color target to draw into">{}),
};

/// An image handed to the renderer to draw into, whatever backs it. An `ImageSlice`
/// because it is not ours to own and its four fields (handle, view, extent, format)
/// are exactly what the rest of the renderer wants from it.
struct SwapchainTarget {
    ImageSlice image {};
    uint32_t   imageIndex  = 0;
    /// The frame slot the acquisition used; the caller opens its command buffer
    /// through SlotCommand(slot).
    uint32_t slot = 0;
    /// The generation the image belongs to; a caller caching handles compares this
    /// before using one.
    uint64_t generation = 1;
    /// Swapchain-backed, so it must be transitioned to PRESENT_SRC_KHR and presented.
    /// False for the headless color target.
    bool presentable = false;
};

// Present reports in the frame vocabulary (Zahlen/FrameResult.hpp) rather than a
// status enum of its own, and the VkResult-to-error mapping lives in one place:
// Vk::ToFrameError.

/// One window's (or one headless frame's) presentation resources.
class SwapchainPresenter {
  public:
    SwapchainPresenter() noexcept  = default;
    ~SwapchainPresenter() noexcept = default;

    SwapchainPresenter(const SwapchainPresenter&)                    = delete;
    auto operator=(const SwapchainPresenter&) -> SwapchainPresenter& = delete;
    SwapchainPresenter(SwapchainPresenter&&) noexcept                = default;
    auto operator=(SwapchainPresenter&&) noexcept -> SwapchainPresenter& = default;

    // --- State
    // `surface` is assigned by the caller before Init; VK_NULL_HANDLE means headless.

    Surface      surface;
    Swapchain    swapchain;
    SemaphorePool presentSemaphores;

    RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT> depthTarget;
    RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>     headlessColorTarget;

    FrameSync<2>                         sync;
    CommandPools<2, QueueType::Graphics> pools;

    /// Double-buffered frame slot: the renderer reads it to index its own per-frame
    /// arrays and writes it once per frame (see EndFrame).
    uint32_t frameIndex = 0;

    /// Bumped by every successful Rebuild, which replaces the swapchain images and
    /// offscreen targets with new handles: anything that cached one -- the renderer's
    /// destination records above all -- must compare this before using it, or a resize
    /// followed by a frame is a driver-side use-after-free.
    uint64_t resourceGeneration = 1;

    // --- Bring-up

    /// Creates the sync objects and command pools, then (re)builds the swapchain and
    /// targets from `surface`.
    [[nodiscard]] auto Init(const Context& ctx, Allocator& alloc, uint32_t width, uint32_t height, uint32_t graphicsFamily, bool vsync = true)
        -> std::expected<void, ErrorCode>;

    /// Waits for the device to go idle, then replaces the swapchain, present
    /// semaphores and depth target, bumping resourceGeneration.
    [[nodiscard]] auto Rebuild(uint32_t width, uint32_t height) -> std::expected<void, ErrorCode>;

    // --- The frame's verbs

    /// Rebuilds first when the window has outgrown the swapchain (the caller says
    /// whether it may -- the primary's resize also recreates the renderer's targets),
    /// waits the slot's fence, resets its pool and acquires the next image, or names
    /// the headless color target when there is no surface. Nothing is recorded here:
    /// the caller owns the command buffer and opens it through SlotCommand(slot).
    ///
    /// std::nullopt means nothing was vended -- the swapchain no longer matched and
    /// this call rebuilt what it could, so there is simply nothing to draw into.
    [[nodiscard]] auto AcquireNext(VkExtent2D desiredExtent, bool allowRebuild) noexcept -> FrameOutcome<SwapchainTarget>;

    /// The end of a frame for one destination, in the order the driver needs: record
    /// the transition of `imageIndex` from `currentLayout` to PRESENT_SRC_KHR into
    /// `cmd` (which is why this call also ends the recording), submit `cmd` waiting on
    /// the image-available semaphore plus `extraWaits` and signalling the image's
    /// present semaphore behind the slot's fence, then present.
    ///
    /// `extraWaits` is how the caller orders this behind the other queues it used this
    /// frame; the presenter has no opinion about those. Returns nullopt when the image
    /// went to the presentation engine, PresentSuboptimal when swapchain and surface
    /// disagree (caller rebuilds and draws again), else DeviceLost or the driver's code.
    [[nodiscard]] auto Present(
        VkQueue graphicsQueue, VkQueue presentQueue, VkCommandBuffer cmd, uint32_t imageIndex, VkImageLayout currentLayout,
        std::span<const VkSemaphoreSubmitInfo> extraWaits = {}
    ) noexcept -> FrameOutcome<PresentSuboptimal>;

    /// Advances this presenter's parity, once per frame after every destination has
    /// been presented.
    void AdvanceFrame() noexcept {
        frameIndex = (frameIndex + 1) & 1u;
    }

    // --- Queries

    /// The format a pass writing presentation-bound color must use: the swapchain's, or
    /// the headless color target's.
    [[nodiscard]] auto GetPresentFormat() const noexcept -> VkFormat {
        return swapchain.Valid() ? swapchain.Get().format : VK_FORMAT_R8G8B8A8_UNORM;
    }

    [[nodiscard]] auto HasSwapchain() const noexcept -> bool {
        return swapchain.Valid();
    }
    [[nodiscard]] auto HasSurface() const noexcept -> bool {
        return surface.Get() != VK_NULL_HANDLE;
    }
    [[nodiscard]] auto Extent() const noexcept -> VkExtent2D {
        return swapchain.Valid() ? swapchain.Get().extent : headlessColorTarget.extent;
    }
    [[nodiscard]] auto DepthTarget() noexcept -> RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>& {
        return depthTarget;
    }

    /// The frame slot's command buffer, for the caller that opens one.
    [[nodiscard]] auto SlotCommand(uint32_t slot) const noexcept -> VkCommandBuffer {
        return pools.Cmd(slot);
    }

    /// The semaphore this slot's submission must signal for the present of `imageIndex`
    /// to be ordered behind it; VK_NULL_HANDLE when headless.
    [[nodiscard]] auto PresentSemaphore(uint32_t imageIndex) const noexcept -> VkSemaphore {
        return swapchain.Valid() ? presentSemaphores[imageIndex] : VK_NULL_HANDLE;
    }

  private:
    const Context* _ctx     = nullptr;
    Allocator*     _alloc   = nullptr;
    bool           _vsync   = true;
};

} // namespace ZHLN::Vk
