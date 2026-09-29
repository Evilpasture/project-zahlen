// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "PresentPacer.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/Render/PresentTiming.hpp>
#include <cstdint>
#include <optional>
#include <span>

namespace ZHLN::Vk {

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

struct SwapchainTarget {
    ImageSlice image {};
    uint32_t   imageIndex  = 0;
    uint32_t slot = 0;
    uint64_t generation = 1;
    bool presentable = false;
};


inline constexpr VkFormat kHeadlessColorFormat = VK_FORMAT_R8G8B8A8_SRGB;

class SwapchainPresenter {
  public:
    SwapchainPresenter() noexcept  = default;
    ~SwapchainPresenter() noexcept;

    SwapchainPresenter(const SwapchainPresenter&)                    = delete;
    auto operator=(const SwapchainPresenter&) -> SwapchainPresenter& = delete;
    SwapchainPresenter(SwapchainPresenter&&) noexcept                = default;
    auto operator=(SwapchainPresenter&&) noexcept -> SwapchainPresenter&;


    Surface      surface;
    Swapchain    swapchain;
    SemaphorePool presentSemaphores;

    RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT> depthTarget;
    RenderTarget<kHeadlessColorFormat>         headlessColorTarget;

    FrameSync<kFramesInFlight>                         sync;
    CommandPools<kFramesInFlight, QueueType::Graphics> pools;

    uint32_t frameIndex = 0;

    uint64_t resourceGeneration = 1;


    [[nodiscard]] auto Init(const Context& ctx, Allocator& alloc, uint32_t width, uint32_t height, uint32_t graphicsFamily, bool vsync = true)
        -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto Rebuild(uint32_t width, uint32_t height) -> std::expected<void, ErrorCode>;
    // Destruction requires all submitted frames to have completed.
    void Cleanup() noexcept;


    [[nodiscard]] auto AcquireNext(VkExtent2D desiredExtent, bool allowRebuild) noexcept -> FrameOutcome<SwapchainTarget>;

    [[nodiscard]] auto Present(
        VkQueue graphicsQueue, VkQueue presentQueue, VkCommandBuffer cmd, uint32_t imageIndex, VkImageLayout currentLayout,
        std::span<const VkSemaphoreSubmitInfo> extraWaits = {}
    ) noexcept -> FrameOutcome<PresentSuboptimal>;

    void AdvanceFrame() noexcept {
        frameIndex = NextFrameSlot(frameIndex);
    }


    [[nodiscard]] auto GetPresentFormat() const noexcept -> VkFormat {
        return swapchain.Valid() ? swapchain.Get().format : kHeadlessColorFormat;
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

    [[nodiscard]] auto SlotCommand(uint32_t slot) const noexcept -> VkCommandBuffer {
        return pools.Cmd(slot);
    }

    [[nodiscard]] auto PresentSemaphore(uint32_t imageIndex) const noexcept -> VkSemaphore {
        return swapchain.Valid() ? presentSemaphores[imageIndex] : VK_NULL_HANDLE;
    }

    [[nodiscard]] auto GetPresentTiming() const noexcept -> PresentTimingMetrics {
        return _pacer.Metrics();
    }
    [[nodiscard]] auto GetPacedDeltaTime() const noexcept -> std::optional<float> {
        return _pacer.PacedDeltaSeconds();
    }

  private:
    const Context*   _ctx   = nullptr;
    Allocator*       _alloc = nullptr;
    bool             _vsync = true;
    PresentPacer _pacer;
};

}
