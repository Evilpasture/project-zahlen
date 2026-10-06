// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Swapchain.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace ZHLN::Vk {
namespace {

template <typename T, typename Enumerate>
[[nodiscard]] auto EnumerateSurfaceValues(Enumerate&& enumerate) noexcept -> std::expected<std::vector<T>, VkResult> {
    std::vector<T> values;
    for (;;) {
        uint32_t count = 0;
        VkResult result = enumerate(&count, nullptr);
        if (result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        if (count == 0) {
            return values;
        }

        values.resize(count);
        result = enumerate(&count, values.data());
        if (result == VK_SUCCESS) {
            values.resize(count);
            return values;
        }
        if (result != VK_INCOMPLETE) {
            return std::unexpected(result);
        }
    }
}

[[nodiscard]] auto QuerySwapchainSupport(const VkPhysicalDevice physical, const VkSurfaceKHR surface) noexcept
    -> std::expected<SwapchainSupport, VkResult> {
    if (physical == VK_NULL_HANDLE || surface == VK_NULL_HANDLE || vkGetPhysicalDeviceSurfaceCapabilitiesKHR == nullptr ||
        vkGetPhysicalDeviceSurfaceFormatsKHR == nullptr || vkGetPhysicalDeviceSurfacePresentModesKHR == nullptr) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    SwapchainSupport support {};
    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &support.capabilities);
    if (result != VK_SUCCESS) {
        return std::unexpected(result);
    }

    auto formats = EnumerateSurfaceValues<VkSurfaceFormatKHR>([physical, surface](uint32_t* count, VkSurfaceFormatKHR* values) {
        return vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, count, values);
    });
    if (!formats) {
        return std::unexpected(formats.error());
    }
    support.formats = std::move(*formats);

    auto presentModes = EnumerateSurfaceValues<VkPresentModeKHR>([physical, surface](uint32_t* count, VkPresentModeKHR* values) {
        return vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, count, values);
    });
    if (!presentModes) {
        return std::unexpected(presentModes.error());
    }
    support.present_modes = std::move(*presentModes);
    return support;
}

[[nodiscard]] auto Advertises(const std::span<const VkPresentModeKHR> modes, const VkPresentModeKHR requested) noexcept -> bool {
    return std::ranges::contains(modes, requested);
}

[[nodiscard]] auto ChoosePresentMode(
    const std::span<const VkPresentModeKHR> modes,
    const VkPresentModeKHR requested,
    const bool vsync
) noexcept -> VkPresentModeKHR {
    if (requested != VK_PRESENT_MODE_MAX_ENUM_KHR && Advertises(modes, requested)) {
        return requested;
    }
    if (!vsync && Advertises(modes, VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (Advertises(modes, VK_PRESENT_MODE_MAILBOX_KHR)) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (Advertises(modes, VK_PRESENT_MODE_FIFO_KHR)) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    return VK_PRESENT_MODE_MAX_ENUM_KHR;
}

[[nodiscard]] auto ChooseFormat(const std::span<const VkSurfaceFormatKHR> formats) noexcept -> VkSurfaceFormatKHR {
    const auto preferred = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR& format) {
        return format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (preferred != formats.end()) {
        return *preferred;
    }
    if (formats.size() == 1 && formats.front().format == VK_FORMAT_UNDEFINED) {
        return VkSurfaceFormatKHR {
            .format = VK_FORMAT_B8G8R8A8_SRGB,
            .colorSpace = formats.front().colorSpace,
        };
    }
    return formats.empty() ? VkSurfaceFormatKHR {} : formats.front();
}

[[nodiscard]] auto ChooseExtent(
    const VkSurfaceCapabilitiesKHR& capabilities,
    const VkExtent2D requested
) noexcept -> VkExtent2D {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }
    return VkExtent2D {
        .width  = std::clamp(requested.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        .height = std::clamp(requested.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
    };
}

[[nodiscard]] auto ChooseCompositeAlpha(const VkCompositeAlphaFlagsKHR supported) noexcept -> VkCompositeAlphaFlagBitsKHR {
    constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> preferred {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (const VkCompositeAlphaFlagBitsKHR alpha: preferred) {
        if ((supported & alpha) != 0) {
            return alpha;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

[[nodiscard]] auto RetrieveImages(const VkDevice device, const VkSwapchainKHR swapchain) noexcept -> std::expected<std::vector<VkImage>, VkResult> {
    std::vector<VkImage> images;
    for (;;) {
        uint32_t count = 0;
        VkResult result = vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
        if (result != VK_SUCCESS) {
            return std::unexpected(result);
        }
        if (count == 0) {
            return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
        }

        images.resize(count);
        result = vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
        if (result == VK_SUCCESS) {
            images.resize(count);
            return images;
        }
        if (result != VK_INCOMPLETE) {
            return std::unexpected(result);
        }
    }
}

} // namespace

Swapchain::~Swapchain() noexcept {
    Destroy();
}

Swapchain::Swapchain(Swapchain&& other) noexcept:
    _device(std::exchange(other._device, VK_NULL_HANDLE)), _data(std::exchange(other._data, {})) {}

auto Swapchain::operator=(Swapchain&& other) noexcept -> Swapchain& {
    if (this != &other) {
        Destroy();
        _device = std::exchange(other._device, VK_NULL_HANDLE);
        _data   = std::exchange(other._data, {});
    }
    return *this;
}

auto Swapchain::Rebuild(
    const VkDevice device,
    const PhysicalDeviceInfo& physical,
    const VkSurfaceKHR surface,
    const VkExtent2D extent,
    const bool vsync,
    const VkPresentModeKHR requestedPresentMode,
    const bool enablePresentTiming
) noexcept -> std::expected<void, VkResult> {
    if (device == VK_NULL_HANDLE || physical.handle == VK_NULL_HANDLE || surface == VK_NULL_HANDLE ||
        (Valid() && _device != device)) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    auto supportResult = QuerySwapchainSupport(physical.handle, surface);
    if (!supportResult) {
        return std::unexpected(supportResult.error());
    }
    const SwapchainSupport& support = *supportResult;
    if (support.formats.empty() || support.present_modes.empty()) {
        return std::unexpected(VK_ERROR_FORMAT_NOT_SUPPORTED);
    }

    const VkSurfaceFormatKHR format = ChooseFormat(support.formats);
    const VkPresentModeKHR presentMode = ChoosePresentMode(support.present_modes, requestedPresentMode, vsync);
    if (presentMode == VK_PRESENT_MODE_MAX_ENUM_KHR) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }
    const VkExtent2D chosenExtent = ChooseExtent(support.capabilities, extent);

    uint32_t imageCount = support.capabilities.minImageCount;
    if (imageCount < UINT32_MAX) {
        ++imageCount;
    }
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }
    if (imageCount == 0) {
        return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
    }

    const std::array<uint32_t, 2> queueFamilies {physical.graphics_family, physical.present_family};
    const bool sharedQueue = queueFamilies[0] == queueFamilies[1];
    const bool supportsPresentModeChain = vkReleaseSwapchainImagesKHR != nullptr;

    const VkSwapchainPresentModesCreateInfoKHR presentModesInfo {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR,
        .pNext = nullptr,
        .presentModeCount = 1,
        .pPresentModes = &presentMode,
    };
    const VkSwapchainCreateInfoKHR createInfo {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = supportsPresentModeChain ? &presentModesInfo : nullptr,
        .flags = enablePresentTiming
            ? static_cast<VkSwapchainCreateFlagsKHR>(VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR)
            : VkSwapchainCreateFlagsKHR {0},
        .surface = surface,
        .minImageCount = imageCount,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = chosenExtent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = sharedQueue ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = sharedQueue ? 0U : static_cast<uint32_t>(queueFamilies.size()),
        .pQueueFamilyIndices = sharedQueue ? nullptr : queueFamilies.data(),
        .preTransform = support.capabilities.currentTransform,
        .compositeAlpha = ChooseCompositeAlpha(support.capabilities.supportedCompositeAlpha),
        .presentMode = presentMode,
        .clipped = VK_TRUE,
        .oldSwapchain = _data.handle,
    };

    VkSwapchainKHR newHandle = VK_NULL_HANDLE;
    const VkResult created = vkCreateSwapchainKHR(device, &createInfo, nullptr, &newHandle);
    if (created != VK_SUCCESS) {
        return std::unexpected(created);
    }

    auto imagesResult = RetrieveImages(device, newHandle);
    if (!imagesResult) {
        vkDestroySwapchainKHR(device, newHandle, nullptr);
        return std::unexpected(imagesResult.error());
    }

    SwapchainData next {
        .handle = newHandle,
        .images = std::move(*imagesResult),
        .views = {},
        .image_count = 0,
        .format = format.format,
        .extent = chosenExtent,
        .present_mode = presentMode,
    };
    next.image_count = static_cast<uint32_t>(next.images.size());
    next.views.resize(next.images.size(), VK_NULL_HANDLE);

    const VkImageViewCreateInfo viewInfoTemplate {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = VK_NULL_HANDLE,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format.format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };

    for (size_t i = 0; i < next.images.size(); ++i) {
        VkImageViewCreateInfo viewInfo = viewInfoTemplate;
        viewInfo.image = next.images[i];
        const VkResult viewCreated = vkCreateImageView(device, &viewInfo, nullptr, &next.views[i]);
        if (viewCreated != VK_SUCCESS) {
            for (VkImageView view: next.views) {
                if (view != VK_NULL_HANDLE) {
                    vkDestroyImageView(device, view, nullptr);
                }
            }
            vkDestroySwapchainKHR(device, newHandle, nullptr);
            return std::unexpected(viewCreated);
        }
    }

    Destroy();
    _device = device;
    _data   = std::move(next);
    return {};
}

void Swapchain::Destroy() noexcept {
    if (_device != VK_NULL_HANDLE) {
        for (const VkImageView view: _data.views) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(_device, view, nullptr);
            }
        }
        if (_data.handle != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(_device, _data.handle, nullptr);
        }
    }
    _data   = {};
    _device = VK_NULL_HANDLE;
}

} // namespace ZHLN::Vk
