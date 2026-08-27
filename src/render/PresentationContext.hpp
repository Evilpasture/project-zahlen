// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

// Swapchain / presentation subsystem bring-up failures.
enum class PresentationError : uint8_t {
    ContextInvalid[[= ZHLN::Reflect::Description("Presentation context is missing a device or allocator")]] = 1,
    SwapchainCreationFailed[[= ZHLN::Reflect::Description("Swapchain creation failed")]],
};

} // namespace ZHLN

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

    // Headless offscreen color target: used as the Blit pass output when no
    // swapchain exists.  R8G8B8A8_UNORM matches the most common sRGB
    // swapchain format and keeps the Blit pipeline format consistent.
    RenderTarget<VK_FORMAT_R8G8B8A8_UNORM> headlessColorTarget;

    PresentationContext()  = default;
    ~PresentationContext() = default;

    // Move-only
    PresentationContext(const PresentationContext&)                        = delete;
    auto operator=(const PresentationContext&) -> PresentationContext&     = delete;
    PresentationContext(PresentationContext&&) noexcept                    = default;
    auto operator=(PresentationContext&&) noexcept -> PresentationContext& = default;

    [[nodiscard]] auto
        Init(const Context& ctx, Allocator& alloc, VkSurfaceKHR surface, uint32_t width, uint32_t height, bool vsync = true) -> std::expected<void, Error>;

    [[nodiscard]] auto Rebuild(uint32_t width, uint32_t height) -> std::expected<void, Error>;

    /// @brief Returns the effective color format for the Blit pass output.
    ///        Uses the swapchain format when available, otherwise R8G8B8A8_UNORM
    ///        from the headless color target.
    [[nodiscard]] VkFormat GetPresentFormat() const noexcept {
        if (swapchain.Valid()) {
            return swapchain.Get().format;
        }
        return VK_FORMAT_R8G8B8A8_UNORM;
    }

  private:
    const Context* _ctx     = nullptr;
    Allocator*     _alloc   = nullptr;
    VkSurfaceKHR   _surface = VK_NULL_HANDLE;
    bool           _vsync   = true;
};

} // namespace ZHLN::Vk
