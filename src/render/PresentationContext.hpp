// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

class Context;
class Allocator;

/**
 * @brief Infrastructure orchestrator. Manages Swapchain, Present Semaphores,
 * and the Window-bound Depth Buffer.
 */
class PresentationContext {
  public:
    Swapchain     swapchain;
    SemaphorePool presentSemaphores;

    // The main depth buffer is tied to the window resolution
    RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT> depthTarget;

    PresentationContext()  = default;
    ~PresentationContext() = default;

    // Move-only
    PresentationContext(const PresentationContext&)                        = delete;
    auto operator=(const PresentationContext&) -> PresentationContext&     = delete;
    PresentationContext(PresentationContext&&) noexcept                    = default;
    auto operator=(PresentationContext&&) noexcept -> PresentationContext& = default;

    [[nodiscard]] auto Init(const Context& ctx, Allocator& alloc, VkSurfaceKHR surface, uint32_t width, uint32_t height, bool vsync = true)
        -> std::expected<void, ZHLN::Error>;

    [[nodiscard]] auto Rebuild(uint32_t width, uint32_t height) -> std::expected<void, ZHLN::Error>;

  private:
    const Context* _ctx     = nullptr;
    Allocator*     _alloc   = nullptr;
    VkSurfaceKHR   _surface = VK_NULL_HANDLE;
    bool           _vsync   = true;
};

} // namespace ZHLN::Vk
