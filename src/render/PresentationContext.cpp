// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/PresentationContext.cpp

#include "PresentationContext.hpp"

namespace ZHLN::Vk {

auto PresentationContext::Init(const Context& ctx, Allocator& alloc, VkSurfaceKHR surface, uint32_t width, uint32_t height, bool vsync)
    -> std::expected<void, ZHLN::Error> {
    _ctx     = &ctx;
    _alloc   = &alloc;
    _surface = surface;
    _vsync   = vsync;
    return Rebuild(width, height);
}

auto PresentationContext::Rebuild(uint32_t width, uint32_t height) -> std::expected<void, Error> {
    if ((_ctx == nullptr) || (_alloc == nullptr)) {
        return std::unexpected(RenderInitError::SubsystemAllocationFailed);
    }

    auto idle_res = Vk::WaitIdle(_ctx->Device());
    if (!idle_res) {
        return std::unexpected(idle_res.error());
    }

    // In headless mode (no surface), skip swapchain construction entirely.
    // Allocate a depth target and a color target using the requested render
    // extent for offscreen rendering.
    if (_surface == VK_NULL_HANDLE) {
        const VkExtent2D renderExtent = {.width = width, .height = height};
        {
            auto dt_res = RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>::Create(
                *_alloc, *_ctx, renderExtent, {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT}
            );
            if (!dt_res) {
                return std::unexpected(dt_res.error());
            }
            depthTarget = std::move(*dt_res);
        }

        // Headless offscreen color target for the Blit pass output
        {
            auto hct_res = RenderTarget<VK_FORMAT_R8G8B8A8_UNORM>::Create(
                *_alloc, *_ctx, renderExtent, {.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT}
            );
            if (!hct_res) {
                return std::unexpected(hct_res.error());
            }
            headlessColorTarget = std::move(*hct_res);
        }

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
        .device        = &raw_dev,
        .physical      = &raw_phys,
        .surface       = _surface,
        .width         = width,
        .height        = height,
        .vsync         = _vsync,
        .old_swapchain = swapchain.Get().handle,
    };

    if (!swapchain.Rebuild(s_desc)) {
        return std::unexpected(RenderInitError::PresentationFailed);
    }
    presentSemaphores.Rebuild(_ctx->Device(), swapchain.Get().image_count);

    // Automatically recreate the depth buffer to match the new swapchain extent
    {
        auto dt_res = RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>::Create(
            *_alloc, *_ctx, swapchain.Get().extent, {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT}
        );
        if (!dt_res) {
            return std::unexpected(dt_res.error());
        }
        depthTarget = std::move(*dt_res);
    }

    return {};
}

} // namespace ZHLN::Vk
