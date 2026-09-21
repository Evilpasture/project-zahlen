// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/presentation/Surface.cpp

#include "Surface.hpp"
#include <Zahlen/Log.hpp>
#include <vector>

namespace ZHLN::Vk {

// Surface Implementation

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

} // namespace ZHLN::Vk

namespace {

// --- Direct-to-display selection
//
// VK_KHR_display builds a surface from a physical device rather than from a
// window: the display, one of its modes and one of its planes are enumerated and
// picked here. First-of-each is the whole policy, which is what the TTY session
// had before this moved out of the window subsystem; a caller that wants to
// choose has the same three lists to choose from, in the same order.

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

[[nodiscard]] auto SelectDisplay(VkPhysicalDevice physicalDevice) noexcept -> std::expected<VkDisplayPropertiesKHR, ZHLN::ErrorCode> {
    auto displays = FetchVulkanVector<VkDisplayPropertiesKHR>([physicalDevice](uint32_t* c, VkDisplayPropertiesKHR* d) {
        vkGetPhysicalDeviceDisplayPropertiesKHR(physicalDevice, c, d);
    });
    if (displays.empty()) {
        ZHLN::Log("[Vk::Surface] FATAL: No displays found via VK_KHR_display");
        return std::unexpected(ZHLN::Vk::SurfaceCreationError::TTYSurfaceCreationFailed);
    }
    ZHLN::Log("[Vk::Surface] Using Display: {}", displays[0].displayName != nullptr ? displays[0].displayName : "Unknown");
    return displays[0];
}

[[nodiscard]] auto SelectMode(VkPhysicalDevice physicalDevice, VkDisplayKHR display) noexcept -> std::expected<VkDisplayModePropertiesKHR, ZHLN::ErrorCode> {
    auto modes = FetchVulkanVector<VkDisplayModePropertiesKHR>([physicalDevice, display](uint32_t* c, VkDisplayModePropertiesKHR* m) {
        vkGetDisplayModePropertiesKHR(physicalDevice, display, c, m);
    });
    if (modes.empty()) {
        ZHLN::Log("[Vk::Surface] FATAL: No compatible display modes found!");
        return std::unexpected(ZHLN::Vk::SurfaceCreationError::TTYSurfaceCreationFailed);
    }
    return modes[0];
}

// The plane already scanning out this display, or the first plane that lists it
// as supported; UINT32_MAX when none does.
[[nodiscard]] auto SelectPlane(VkPhysicalDevice physicalDevice, VkDisplayKHR display) noexcept -> uint32_t {
    auto planes = FetchVulkanVector<VkDisplayPlanePropertiesKHR>([physicalDevice](uint32_t* c, VkDisplayPlanePropertiesKHR* p) {
        vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physicalDevice, c, p);
    });

    const auto planeCount = static_cast<uint32_t>(planes.size());
    for (uint32_t i = 0; i < planeCount; ++i) {
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

namespace ZHLN::Vk {

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
    ZHLN::Log("[Vk::Surface] Selected Mode: {}x{}", outWidth, outHeight);

    const uint32_t planeIndex = SelectPlane(physicalDevice, display->display);
    if (planeIndex == UINT32_MAX) {
        ZHLN::Log("[Vk::Surface] FATAL: Could not find a compatible display plane!");
        return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
    }

    VkDisplayPlaneCapabilitiesKHR capabilities {};
    vkGetDisplayPlaneCapabilitiesKHR(physicalDevice, mode->displayMode, planeIndex, &capabilities);

    const VkDisplaySurfaceCreateInfoKHR createInfo {
        .sType           = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .pNext           = nullptr,
        .flags           = 0,
        .displayMode     = mode->displayMode,
        .planeIndex      = planeIndex,
        .planeStackIndex = 0,
        .transform       = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha     = 1.0F,
        .alphaMode       = SelectAlpha(capabilities),
        .imageExtent     = {.width = outWidth, .height = outHeight},
    };

    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    if (vkCreateDisplayPlaneSurfaceKHR(instance, &createInfo, nullptr, &rawSurface) != VK_SUCCESS) {
        ZHLN::Log("[Vk::Surface] FATAL: vkCreateDisplayPlaneSurfaceKHR failed!");
        return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
    }

    ZHLN::Log("[Vk::Surface] Surface successfully created on Plane {}", planeIndex);
    return Surface(instance, rawSurface);
}

} // namespace ZHLN::Vk
