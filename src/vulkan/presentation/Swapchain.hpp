// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include "../core/PhysicalDevice.hpp"
#include "../memory/ImageSlice.hpp"
#include <Zahlen/Core/Description.hpp>
#include <expected>
#include <span>
#include <vector>

namespace ZHLN::Vk {

struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR        capabilities {};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR>   presentModes;

    [[nodiscard]] auto Formats() const noexcept -> std::span<const VkSurfaceFormatKHR> {
        return formats;
    }
    [[nodiscard]] auto PresentModes() const noexcept -> std::span<const VkPresentModeKHR> {
        return presentModes;
    }
};

struct SwapchainData {
    VkSwapchainKHR           handle = VK_NULL_HANDLE;
    std::vector<VkImage>     images;
    std::vector<VkImageView> views;
    uint32_t                 imageCount = 0;
    VkFormat                 format     = VK_FORMAT_UNDEFINED;
    VkExtent2D               extent {};
    VkPresentModeKHR         presentMode = VK_PRESENT_MODE_MAX_ENUM_KHR;
};

struct AcquiredSwapchainImage {
    ImageSlice image;
    uint32_t   imageIndex = 0;
    bool       suboptimal = false;
};

enum class SwapchainAcquireError : uint8_t {
    OutOfDate ZHLN_ANNOTATION(ZHLN::Description<"Swapchain is out of date and must be rebuilt"> {}) = 1,
};

class Swapchain {
  public:
    Swapchain() noexcept = default;
    ~Swapchain() noexcept;

    Swapchain(const Swapchain&)                    = delete;
    auto operator=(const Swapchain&) -> Swapchain& = delete;

    Swapchain(Swapchain&& other) noexcept;
    auto operator=(Swapchain&& other) noexcept -> Swapchain&;

    [[nodiscard]] auto Get() const noexcept -> const SwapchainData& {
        return _data;
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _data.handle != VK_NULL_HANDLE;
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]] auto Rebuild(
        VkDevice                  device,
        const PhysicalDeviceInfo& physical,
        VkSurfaceKHR              surface,
        VkExtent2D                extent,
        bool                      vsync,
        VkPresentModeKHR          requestedPresentMode,
        bool                      enablePresentTiming,
        bool                      allowMutableFormat = false
    ) noexcept -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto AcquireNext(VkSemaphore signalSemaphore, uint64_t timeout = UINT64_MAX) noexcept
        -> std::expected<AcquiredSwapchainImage, ErrorCode>;

  private:
    void Destroy() noexcept;

    VkDevice      _device = VK_NULL_HANDLE;
    SwapchainData _data {};
    bool          _mutableViewActive = false;
};

} // namespace ZHLN::Vk
