// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/VulkanHeader.hpp"

#include "vulkan/core/PhysicalDevice.hpp"
#include "vulkan/memory/ImageSlice.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
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

// Everything that can go wrong while negotiating and building a swapchain
// for a surface. Kept apart from SwapchainAcquireError below, because
// "out of date" is a normal frame-loop outcome, not a build failure.
enum class SwapchainError : uint8_t {
    InvalidArguments        ZHLN_ANNOTATION(ZHLN::Description<"Swapchain rebuild was given a null device, physical device or surface"> {}) = 1,
    DeviceMismatch          ZHLN_ANNOTATION(ZHLN::Description<"Swapchain rebuild was given a device other than the one the live swapchain was created with"> {}),
    NotInitialized          ZHLN_ANNOTATION(ZHLN::Description<"The swapchain has no image to acquire"> {}),
    SurfaceQueryFailed      ZHLN_ANNOTATION(ZHLN::Description<"Querying surface capabilities, formats or present modes failed"> {}),
    SurfaceUnsupported      ZHLN_ANNOTATION(ZHLN::Description<"The surface reports no format, present mode or image count a swapchain can use"> {}),
    CreationFailed          ZHLN_ANNOTATION(ZHLN::Description<"Swapchain creation failed"> {}),
    ImageRetrievalFailed    ZHLN_ANNOTATION(ZHLN::Description<"Retrieving the swapchain images failed"> {}),
    ImageViewCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Creating a swapchain image view failed"> {}),
    AcquireFailed           ZHLN_ANNOTATION(ZHLN::Description<"Acquiring the next swapchain image failed"> {}),
    // AcquireNext is on the frame path and its caller checks; creating a
    // swapchain is a rebuild, which the caller treats as a rebuild.
    DeviceLost ZHLN_ANNOTATION(ZHLN::Description<"The graphics device was lost (VK_ERROR_DEVICE_LOST)"> {}),
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
