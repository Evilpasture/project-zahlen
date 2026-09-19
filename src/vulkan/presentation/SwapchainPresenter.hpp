// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/presentation/SwapchainPresenter.hpp
//
// Presentation, and nothing but presentation: one surface, the swapchain built
// on it, the per-frame sync and command objects that submit into it, the depth
// and headless targets that go with its extent, and the three verbs that make
// an image appear on screen -- acquire, transition-to-present, present.
//
// This is the only place in the engine that knows an image belongs to a
// swapchain. It replaces the pair this used to be split across
// (PresentationContext, which owned the swapchain and the targets, and
// SwapchainSession, which bundled that with the sync objects): the split was
// along "whose member is it", and the acquire/present verbs lived in
// src/render, which is why a renderer file ended up assembling VkSubmitInfo2
// and recording the present transition by hand.
//
// No Window and no engine type appears here -- a window is a surface KHR by the
// time it arrives, and the renderer's job is to produce one. `HasSurface()`
// false is the headless case: no swapchain, the frame renders into
// `headlessColorTarget`, and `Present` submits without presenting.
//
// The members below are public, matching what they replaced: roughly ten
// renderer files read the depth target, the swapchain, the frame index and the
// frame's timeline, and hiding them behind accessors would rename those call
// sites without changing who owns what. The *verbs* are the interface; the
// state is the state.

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Error.hpp>
#include <Zahlen/FrameResult.hpp>
#include <cstdint>
#include <span>

namespace ZHLN::Vk {

// Swapchain / presentation subsystem bring-up failures.
//
// Deliberately bring-up only: what the frame verbs can fail on is a Vulkan
// call's result, and it travels as that call's own VkResult inside ErrorCode
// (ImageAcquireFailed / SwapchainOutOfDate / SubmitFailed / PresentFailed /
// DeviceLost used to say the same thing in this enum's words, minus the part a
// reader needs -- *which* failure). This enum is for the cases Vulkan has
// nothing to say about, because the failure is here: no device to create a
// swapchain on, a window that owns no presenter, a format that disagrees with
// the primary's.
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

/// An image handed to the renderer to draw into, whatever backs it.
struct SwapchainTarget {
    VkImage    image       = VK_NULL_HANDLE;
    VkImageView view       = VK_NULL_HANDLE;
    VkExtent2D extent      = {};
    VkFormat   format      = VK_FORMAT_UNDEFINED;
    uint32_t   imageIndex  = 0;
    /// The frame slot the acquisition used. The caller opens its command
    /// buffer through SlotCommand(slot).
    uint32_t slot          = 0;
    /// The generation the image belongs to; a caller caching handles compares
    /// this before using one.
    uint64_t generation    = 1;
    /// Swapchain-backed: this image must be transitioned to PRESENT_SRC_KHR and
    /// presented. False for the headless color target.
    bool     presentable   = false;
};

// PresentStatus (Presented / Suboptimal / OutOfDate) used to live here, as a
// three-value projection of what vkQueuePresentKHR said, and then as a
// FrameResult member in the error channel. Both are gone: Present reports what
// the calls said in the frame vocabulary instead (Zahlen/FrameResult.hpp) --
// std::nullopt for "presented", PresentSuboptimal in the value slot for "did not
// go through as asked", an ErrorCode for the rest -- and the mapping from a
// VkResult to an error lives in exactly one place, Vk::ToFrameError next to the
// frame API.

/// One window's (or one headless frame's) presentation resources.
class SwapchainPresenter {
  public:
    SwapchainPresenter() noexcept  = default;
    ~SwapchainPresenter() noexcept = default;

    SwapchainPresenter(const SwapchainPresenter&)                    = delete;
    auto operator=(const SwapchainPresenter&) -> SwapchainPresenter& = delete;
    SwapchainPresenter(SwapchainPresenter&&) noexcept                = default;
    auto operator=(SwapchainPresenter&&) noexcept -> SwapchainPresenter& = default;

    // --- State -------------------------------------------------------------
    // `surface` is assigned by the caller before Init (a window the renderer
    // asked for a surface from); VK_NULL_HANDLE means headless.

    Surface      surface;
    Swapchain    swapchain;
    SemaphorePool presentSemaphores;

    RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT> depthTarget;
    RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>     headlessColorTarget;

    FrameSync<2>                         sync;
    CommandPools<2, QueueType::Graphics> pools;

    /// Double-buffered frame slot. The renderer reads it to index its own
    /// per-frame arrays and writes it once per frame (see EndFrame).
    uint32_t frameIndex = 0;

    /// Bumped by every successful Rebuild. Rebuild replaces the swapchain
    /// images and the offscreen targets with brand-new VkImage/VkImageView
    /// handles, so anything that cached a handle -- the renderer's destination
    /// records above all -- must compare this before using it. Without that,
    /// a resize followed by a frame is a use-after-free on the driver side.
    uint64_t resourceGeneration = 1;

    // --- Bring-up ----------------------------------------------------------

    /// Creates the sync objects and the command pools, then (re)builds the
    /// swapchain and the targets from `surface`.
    [[nodiscard]] auto Init(const Context& ctx, Allocator& alloc, uint32_t width, uint32_t height, uint32_t graphicsFamily, bool vsync = true)
        -> std::expected<void, ErrorCode>;

    /// Waits for the device to go idle and replaces the swapchain, the present
    /// semaphores and the depth target. Bumps resourceGeneration.
    [[nodiscard]] auto Rebuild(uint32_t width, uint32_t height) -> std::expected<void, ErrorCode>;

    // --- The frame's verbs -------------------------------------------------

    /// Rebuilds first when the window has outgrown the swapchain (the caller
    /// says whether it may, because the primary window's resize also recreates
    /// the renderer's own targets), waits the slot's fence, resets its pool and
    /// acquires the next image -- or names the headless color target when there
    /// is no surface. Nothing is recorded here: the caller owns the command
    /// buffer, and opens it through SlotCommand(slot).
    ///
    /// The image is the value: std::nullopt means nothing was vended (the
    /// swapchain no longer matched the surface, and this call rebuilt what it
    /// could -- nothing is wrong, there is just nothing to draw into this
    /// frame), and an error is an error.
    [[nodiscard]] auto AcquireNext(VkExtent2D desiredExtent, bool allowRebuild) noexcept -> FrameOutcome<SwapchainTarget>;

    /// The end of a frame for one destination, in the order the driver needs:
    /// record the transition of `imageIndex` from `currentLayout` to
    /// PRESENT_SRC_KHR into `cmd` (which is why this call also ends the
    /// recording -- the transition has to be in the submitted stream), submit
    /// `cmd` waiting on the image-available semaphore plus `extraWaits` and
    /// signalling the image's present semaphore behind the slot's fence, then
    /// present.
    ///
    /// `extraWaits` is how the caller orders this submission behind the other
    /// queues it used this frame (the transfer ring, the compute timeline);
    /// the presenter has no opinion about those.
    ///
    /// What it returns is what the calls said, in the frame vocabulary:
    /// std::nullopt means the submission was made and the image went to the
    /// presentation engine (or, headless, was simply submitted),
    /// PresentSuboptimal means it did not go through as asked (the swapchain and
    /// the surface disagree; the caller rebuilds and draws again -- see that
    /// type), and otherwise the error is FrameResult::DeviceLost or the driver's
    /// own code.
    [[nodiscard]] auto Present(
        VkQueue graphicsQueue, VkQueue presentQueue, VkCommandBuffer cmd, uint32_t imageIndex, VkImageLayout currentLayout,
        std::span<const VkSemaphoreSubmitInfo> extraWaits = {}
    ) noexcept -> FrameOutcome<PresentSuboptimal>;

    /// Advances this presenter's parity. The renderer calls it once per frame,
    /// after every destination has been presented.
    void AdvanceFrame() noexcept {
        frameIndex = (frameIndex + 1) & 1u;
    }

    // --- Queries -----------------------------------------------------------

    /// The format a pass that writes presentation-bound color must use: the
    /// swapchain's, or the headless color target's.
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

    /// The semaphore this slot's submission must signal for the present of
    /// `imageIndex` to be ordered behind it. VK_NULL_HANDLE when headless.
    [[nodiscard]] auto PresentSemaphore(uint32_t imageIndex) const noexcept -> VkSemaphore {
        return swapchain.Valid() ? presentSemaphores[imageIndex] : VK_NULL_HANDLE;
    }

  private:
    const Context* _ctx     = nullptr;
    Allocator*     _alloc   = nullptr;
    bool           _vsync   = true;
};

} // namespace ZHLN::Vk
