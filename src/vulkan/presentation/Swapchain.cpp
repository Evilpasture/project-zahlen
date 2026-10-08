// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Swapchain.hpp"
#include <algorithm>
#include <array>
#include <utility>

namespace ZHLN::Vk {
namespace {

template <typename T, typename Enumerate>
[[nodiscard]] auto EnumerateSurfaceValues(Enumerate&& enumerate) noexcept -> std::expected<std::vector<T>, Vk::Error> {
    std::vector<T> values;
    for (;;) {
        uint32_t count  = 0;
        VkResult result = enumerate(&count, nullptr);
        if (result != VK_SUCCESS) {
            return std::unexpected(Vk::Error {result});
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
            return std::unexpected(Vk::Error {result});
        }
    }
}

[[nodiscard]] auto QuerySwapchainSupport(const VkPhysicalDevice physical, const VkSurfaceKHR surface) noexcept -> std::expected<SwapchainSupport, Vk::Error> {
    if (physical == VK_NULL_HANDLE || surface == VK_NULL_HANDLE || vkGetPhysicalDeviceSurfaceCapabilitiesKHR == nullptr ||
        vkGetPhysicalDeviceSurfaceFormatsKHR == nullptr || vkGetPhysicalDeviceSurfacePresentModesKHR == nullptr) {
        return std::unexpected(Vk::Error {VK_ERROR_INITIALIZATION_FAILED});
    }

    SwapchainSupport support {};
    VkResult         result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &support.capabilities);
    if (result != VK_SUCCESS) {
        return std::unexpected(Vk::Error {result});
    }

    auto formats = EnumerateSurfaceValues<VkSurfaceFormatKHR>([physical, surface](uint32_t* count, VkSurfaceFormatKHR* values) {
        return vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, count, values);
    });
    if (!formats) {
        return std::unexpected(formats.error());
    }
    support.formats = std::move(*formats);

    auto present_modes = EnumerateSurfaceValues<VkPresentModeKHR>([physical, surface](uint32_t* count, VkPresentModeKHR* values) {
        return vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, count, values);
    });
    if (!present_modes) {
        return std::unexpected(present_modes.error());
    }
    support.presentModes = std::move(*present_modes);
    return support;
}

[[nodiscard]] auto Advertises(const std::span<const VkPresentModeKHR> modes, const VkPresentModeKHR requested) noexcept -> bool {
    return std::ranges::contains(modes, requested);
}

[[nodiscard]] auto
    ChoosePresentMode(const std::span<const VkPresentModeKHR> modes, const VkPresentModeKHR requested, const bool vsync) noexcept -> VkPresentModeKHR {
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
            .format     = VK_FORMAT_B8G8R8A8_SRGB,
            .colorSpace = formats.front().colorSpace,
        };
    }
    return formats.empty() ? VkSurfaceFormatKHR {} : formats.front();
}

[[nodiscard]] auto ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, const VkExtent2D requested) noexcept -> VkExtent2D {
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

[[nodiscard]] auto RetrieveImages(const VkDevice device, const VkSwapchainKHR swapchain) noexcept -> std::expected<std::vector<VkImage>, Vk::Error> {
    std::vector<VkImage> images;
    for (;;) {
        uint32_t count  = 0;
        VkResult result = vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
        if (result != VK_SUCCESS) {
            return std::unexpected(Vk::Error {result});
        }
        if (count == 0) {
            return std::unexpected(Vk::Error {VK_ERROR_INITIALIZATION_FAILED});
        }

        images.resize(count);
        result = vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());
        if (result == VK_SUCCESS) {
            images.resize(count);
            return images;
        }
        if (result != VK_INCOMPLETE) {
            return std::unexpected(Vk::Error {result});
        }
    }
}

} // namespace

Swapchain::~Swapchain() noexcept {
    Destroy();
}

Swapchain::Swapchain(Swapchain&& other) noexcept: _device(std::exchange(other._device, VK_NULL_HANDLE)), _data(std::exchange(other._data, {})) {
}

auto Swapchain::operator=(Swapchain&& other) noexcept -> Swapchain& {
    if (this != &other) {
        Destroy();
        _device = std::exchange(other._device, VK_NULL_HANDLE);
        _data   = std::exchange(other._data, {});
    }
    return *this;
}

auto Swapchain::Rebuild(
    const VkDevice            device,
    const PhysicalDeviceInfo& physical,
    const VkSurfaceKHR        surface,
    const VkExtent2D          extent,
    const bool                vsync,
    const VkPresentModeKHR    requestedPresentMode,
    const bool                enablePresentTiming
) noexcept -> std::expected<void, Vk::Error> {
    if (device == VK_NULL_HANDLE || physical.handle == VK_NULL_HANDLE || surface == VK_NULL_HANDLE || (Valid() && _device != device)) {
        return std::unexpected(Vk::Error {VK_ERROR_INITIALIZATION_FAILED});
    }

    auto support_result = QuerySwapchainSupport(physical.handle, surface);
    if (!support_result) {
        return std::unexpected(support_result.error());
    }
    const SwapchainSupport& support = *support_result;
    if (support.formats.empty() || support.presentModes.empty()) {
        return std::unexpected(Vk::Error {VK_ERROR_FORMAT_NOT_SUPPORTED});
    }

    const VkSurfaceFormatKHR format       = ChooseFormat(support.formats);
    const VkPresentModeKHR   present_mode = ChoosePresentMode(support.presentModes, requestedPresentMode, vsync);
    if (present_mode == VK_PRESENT_MODE_MAX_ENUM_KHR) {
        return std::unexpected(Vk::Error {VK_ERROR_INITIALIZATION_FAILED});
    }
    const VkExtent2D chosen_extent = ChooseExtent(support.capabilities, extent);

    uint32_t image_count = support.capabilities.minImageCount;
    if (image_count < UINT32_MAX) {
        ++image_count;
    }
    if (support.capabilities.maxImageCount > 0 && image_count > support.capabilities.maxImageCount) {
        image_count = support.capabilities.maxImageCount;
    }
    if (image_count == 0) {
        return std::unexpected(Vk::Error {VK_ERROR_INITIALIZATION_FAILED});
    }

    const std::array<uint32_t, 2> queue_families {physical.graphicsFamily, physical.presentFamily};
    const bool                    shared_queue                = queue_families[0] == queue_families[1];
    const bool                    supports_present_mode_chain = vkReleaseSwapchainImagesKHR != nullptr;

    const VkSwapchainPresentModesCreateInfoKHR present_modes_info {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR,
        .pNext            = nullptr,
        .presentModeCount = 1,
        .pPresentModes    = &present_mode,
    };
    const VkSwapchainCreateInfoKHR create_info {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext            = supports_present_mode_chain ? &present_modes_info : nullptr,
        .flags            = enablePresentTiming ?
                                static_cast<VkSwapchainCreateFlagsKHR>(VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT | VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR) :
                                VkSwapchainCreateFlagsKHR {0},
        .surface          = surface,
        .minImageCount    = image_count,
        .imageFormat      = format.format,
        .imageColorSpace  = format.colorSpace,
        .imageExtent      = chosen_extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = shared_queue ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = shared_queue ? 0U : static_cast<uint32_t>(queue_families.size()),
        .pQueueFamilyIndices   = shared_queue ? nullptr : queue_families.data(),
        .preTransform          = support.capabilities.currentTransform,
        .compositeAlpha        = ChooseCompositeAlpha(support.capabilities.supportedCompositeAlpha),
        .presentMode           = present_mode,
        .clipped               = VK_TRUE,
        .oldSwapchain          = _data.handle,
    };

    VkSwapchainKHR new_handle = VK_NULL_HANDLE;
    const VkResult created    = vkCreateSwapchainKHR(device, &create_info, nullptr, &new_handle);
    if (created != VK_SUCCESS) {
        return std::unexpected(Vk::Error {created});
    }

    auto images_result = RetrieveImages(device, new_handle);
    if (!images_result) {
        vkDestroySwapchainKHR(device, new_handle, nullptr);
        return std::unexpected(images_result.error());
    }

    SwapchainData next {
        .handle      = new_handle,
        .images      = std::move(*images_result),
        .views       = {},
        .imageCount  = 0,
        .format      = format.format,
        .extent      = chosen_extent,
        .presentMode = present_mode,
    };
    next.imageCount = static_cast<uint32_t>(next.images.size());
    next.views.resize(next.images.size(), VK_NULL_HANDLE);

    const VkImageViewCreateInfo view_info_template {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = nullptr,
        .flags    = 0,
        .image    = VK_NULL_HANDLE,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = format.format,
        .components =
            {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        },
    };

    for (size_t i = 0; i < next.images.size(); ++i) {
        VkImageViewCreateInfo view_info = view_info_template;
        view_info.image                 = next.images[i];
        const VkResult view_created     = vkCreateImageView(device, &view_info, nullptr, &next.views[i]);
        if (view_created != VK_SUCCESS) {
            for (VkImageView view: next.views) {
                if (view != VK_NULL_HANDLE) {
                    vkDestroyImageView(device, view, nullptr);
                }
            }
            vkDestroySwapchainKHR(device, new_handle, nullptr);
            return std::unexpected(Vk::Error {view_created});
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
