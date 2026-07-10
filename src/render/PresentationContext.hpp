// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

/**
 * @brief Infrastructure orchestrator. Manages Swapchain, Present Semaphores,
 * and the Window-bound Depth Buffer.
 */
class PresentationContext {
  public:
    Swapchain     swapchain;
    SemaphorePool presentSemaphores;

    // The main depth buffer is tied to the window resolution
    RenderTarget<VK_FORMAT_D32_SFLOAT> depthTarget;

    PresentationContext()  = default;
    ~PresentationContext() = default;

    // Move-only
    PresentationContext(const PresentationContext&)                        = delete;
    auto operator=(const PresentationContext&) -> PresentationContext&     = delete;
    PresentationContext(PresentationContext&&) noexcept                    = default;
    auto operator=(PresentationContext&&) noexcept -> PresentationContext& = default;

    [[nodiscard]] auto Init(const Context& ctx, Allocator& alloc, VkSurfaceKHR surface, uint32_t width, uint32_t height, bool vsync = true) -> bool {
        _ctx     = &ctx;
        _alloc   = &alloc;
        _surface = surface;
        _vsync   = vsync;
        return Rebuild(width, height);
    }

    [[nodiscard]] auto Rebuild(uint32_t width, uint32_t height) -> bool {
        if ((_ctx == nullptr) || (_alloc == nullptr)) {
            return false;
        }

        if (Vk::WaitIdle(_ctx->Device()) != VK_SUCCESS) {
            return false;
        }

        const ZHLN_Device raw_dev = {
            .handle = _ctx->Device(), .graphics_queue = _ctx->GraphicsQueue(), .present_queue = _ctx->PresentQueue(), .transfer_queue = _ctx->TransferQueue()
        };
        const ZHLN_PhysicalDeviceInfo raw_phys = _ctx->PhysicalInfo();
        ZHLN_SwapchainDesc            s_desc   = {
            .device        = &raw_dev,
            .physical      = &raw_phys,
            .surface       = _surface,
            .width         = width,
            .height        = height,
            .vsync         = _vsync,
            .old_swapchain = swapchain.Get().handle,
        };

        if (!swapchain.Rebuild(s_desc)) {
            return false;
        }
        presentSemaphores.Rebuild(_ctx->Device(), swapchain.Get().image_count);

        // Automatically recreate the depth buffer to match the new swapchain extent
        depthTarget = RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
            *_alloc, *_ctx, swapchain.Get().extent, {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT}
        );

        return depthTarget.Valid();
    }

  private:
    const Context* _ctx     = nullptr;
    Allocator*     _alloc   = nullptr;
    VkSurfaceKHR   _surface = VK_NULL_HANDLE;
    bool           _vsync   = true;
};

} // namespace ZHLN::Vk
