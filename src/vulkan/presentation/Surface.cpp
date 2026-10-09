// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Surface.hpp"
#include <Zahlen/Log.hpp>
#include <vector>

namespace ZHLN::Vk {

Surface::Surface(VkInstance instance, VkSurfaceKHR surface): _instance(instance), _handle(surface) {
}

Surface::~Surface() {
    if (_handle != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(_instance, _handle, nullptr);
    }
}

Surface::Surface(Surface&& other) noexcept: _instance(std::exchange(other._instance, VK_NULL_HANDLE)), _handle(std::exchange(other._handle, VK_NULL_HANDLE)) {
}

auto Surface::operator=(Surface&& other) noexcept -> Surface& {
    if (this != &other) {
        if (_handle != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(_instance, _handle, nullptr);
        }
        _instance = std::exchange(other._instance, VK_NULL_HANDLE);
        _handle   = std::exchange(other._handle, VK_NULL_HANDLE);
    }
    return *this;
}

auto Surface::Get() const -> VkSurfaceKHR {
    return _handle;
}

namespace {

template <typename T, typename F>
[[nodiscard]] auto FetchVulkanVector(F&& enumerator) -> std::vector<T> {
    uint32_t count = 0;
    enumerator(&count, nullptr);
    std::vector<T> vec(count);
    if (count > 0) {
        enumerator(&count, vec.data());
    }
    return vec;
}

[[nodiscard]] auto SelectDisplay(VkPhysicalDevice physicalDevice) noexcept -> std::expected<VkDisplayPropertiesKHR, ErrorCode> {
    auto displays = FetchVulkanVector<VkDisplayPropertiesKHR>([physicalDevice](uint32_t* c, VkDisplayPropertiesKHR* d) {
        vkGetPhysicalDeviceDisplayPropertiesKHR(physicalDevice, c, d);
    });
    if (displays.empty()) {
        return std::unexpected(ZHLN::Vk::SurfaceCreationError::NoDisplayFound);
    }
    return displays[0];
}

[[nodiscard]] auto SelectMode(VkPhysicalDevice physicalDevice, VkDisplayKHR display) noexcept -> std::expected<VkDisplayModePropertiesKHR, ErrorCode> {
    auto modes = FetchVulkanVector<VkDisplayModePropertiesKHR>([physicalDevice, display](uint32_t* c, VkDisplayModePropertiesKHR* m) {
        vkGetDisplayModePropertiesKHR(physicalDevice, display, c, m);
    });
    if (modes.empty()) {
        return std::unexpected(ZHLN::Vk::SurfaceCreationError::IncompatibleModes);
    }
    return modes[0];
}

[[nodiscard]] auto SelectPlane(VkPhysicalDevice physicalDevice, VkDisplayKHR display) noexcept -> uint32_t {
    auto planes = FetchVulkanVector<VkDisplayPlanePropertiesKHR>([physicalDevice](uint32_t* c, VkDisplayPlanePropertiesKHR* p) {
        vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physicalDevice, c, p);
    });

    const auto plane_count = static_cast<uint32_t>(planes.size());
    for (uint32_t i = 0; i < plane_count; ++i) {
        if (planes[i].currentDisplay != VK_NULL_HANDLE && planes[i].currentDisplay != display) {
            continue;
        }
        auto supported = FetchVulkanVector<VkDisplayKHR>([physicalDevice, i](uint32_t* c, VkDisplayKHR* d) {
            vkGetDisplayPlaneSupportedDisplaysKHR(physicalDevice, i, c, d);
        });
        for (const auto* candidate: supported) {
            if (candidate == display) {
                return i;
            }
        }
    }
    return UINT32_MAX;
}

[[nodiscard]] auto SelectAlpha(const VkDisplayPlaneCapabilitiesKHR& capabilities) noexcept -> VkDisplayPlaneAlphaFlagBitsKHR {
    constexpr VkDisplayPlaneAlphaFlagBitsKHR opaque = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
    if (capabilities.supportedAlpha & opaque) {
        return opaque;
    }
    if (capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR) {
        return VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR;
    }
    if (capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR) {
        return VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR;
    }
    if (capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR) {
        return VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR;
    }
    return opaque;
}

} // namespace

auto CreateDisplaySurface(VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t& outWidth, uint32_t& outHeight) noexcept
    -> std::expected<Surface, ErrorCode> {
    if (physicalDevice == VK_NULL_HANDLE) {
        return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
    }

    const auto display = SelectDisplay(physicalDevice);
    if (!display) {
        return std::unexpected(display.error());
    }
    const auto mode = SelectMode(physicalDevice, display->display);
    if (!mode) {
        return std::unexpected(mode.error());
    }

    outWidth  = mode->parameters.visibleRegion.width;
    outHeight = mode->parameters.visibleRegion.height;

    const uint32_t plane_index = SelectPlane(physicalDevice, display->display);
    if (plane_index == UINT32_MAX) {
        return std::unexpected(SurfaceCreationError::IncompatibleDisplay);
    }

    VkDisplayPlaneCapabilitiesKHR capabilities {};
    if (const auto res = vkGetDisplayPlaneCapabilitiesKHR(physicalDevice, mode->displayMode, plane_index, &capabilities); res != VK_SUCCESS) {
        return std::unexpected(ToError(res));
    };

    const VkDisplaySurfaceCreateInfoKHR create_info {
        .sType           = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .pNext           = nullptr,
        .flags           = 0,
        .displayMode     = mode->displayMode,
        .planeIndex      = plane_index,
        .planeStackIndex = 0,
        .transform       = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha     = 1.0F,
        .alphaMode       = SelectAlpha(capabilities),
        .imageExtent     = {.width = outWidth, .height = outHeight},
    };

    VkSurfaceKHR raw_surface = VK_NULL_HANDLE;
    if (vkCreateDisplayPlaneSurfaceKHR(instance, &create_info, nullptr, &raw_surface) != VK_SUCCESS) {
        return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
    }

    return Surface(instance, raw_surface);
}

} // namespace ZHLN::Vk
