// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Surface.hpp"
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <optional>
#include <span>
#include <utility>
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

[[nodiscard]] auto FindCompatiblePlane(
    VkPhysicalDevice physicalDevice, std::span<const VkDisplayPlanePropertiesKHR> planes, VkDisplayKHR display
) noexcept -> uint32_t {
    const auto plane_count   = static_cast<uint32_t>(planes.size());
    uint32_t   unbound_match = UINT32_MAX;

    for (uint32_t i = 0; i < plane_count; ++i) {
        if (planes[i].currentDisplay != VK_NULL_HANDLE && planes[i].currentDisplay != display) {
            continue;
        }
        auto supported = FetchVulkanVector<VkDisplayKHR>([physicalDevice, i](uint32_t* c, VkDisplayKHR* d) {
            vkGetDisplayPlaneSupportedDisplaysKHR(physicalDevice, i, c, d);
        });
        const bool supports_display = std::ranges::any_of(supported, [display](VkDisplayKHR candidate) { return candidate == display; });
        if (!supports_display) {
            continue;
        }
        if (planes[i].currentDisplay == display) {
            return i;
        }
        if (unbound_match == UINT32_MAX) {
            unbound_match = i;
        }
    }
    return unbound_match;
}

[[nodiscard]] auto SelectBestMode(
    std::span<const VkDisplayModePropertiesKHR> modes, VkExtent2D preferredResolution
) noexcept -> VkDisplayModePropertiesKHR {
    const VkDisplayModePropertiesKHR* best_preferred = nullptr;
    const VkDisplayModePropertiesKHR* best_overall   = &modes[0];

    for (const auto& candidate: modes) {
        const VkExtent2D vis      = candidate.parameters.visibleRegion;
        const VkExtent2D best_vis = best_overall->parameters.visibleRegion;
        const uint64_t   pixels   = static_cast<uint64_t>(vis.width) * static_cast<uint64_t>(vis.height);
        const uint64_t   best_px  = static_cast<uint64_t>(best_vis.width) * static_cast<uint64_t>(best_vis.height);
        if (pixels > best_px || (pixels == best_px && candidate.parameters.refreshRate > best_overall->parameters.refreshRate)) {
            best_overall = &candidate;
        }

        if (preferredResolution.width > 0 && preferredResolution.height > 0 && vis.width == preferredResolution.width &&
            vis.height == preferredResolution.height) {
            if (best_preferred == nullptr || candidate.parameters.refreshRate > best_preferred->parameters.refreshRate) {
                best_preferred = &candidate;
            }
        }
    }
    return best_preferred != nullptr ? *best_preferred : *best_overall;
}

struct SelectedDisplayPlan {
    VkDisplayPropertiesKHR     display {};
    VkDisplayModePropertiesKHR mode {};
    uint32_t                   planeIndex = UINT32_MAX;
    uint32_t                   stackIndex = 0;
};

[[nodiscard]] auto SelectDisplayPlan(VkPhysicalDevice physicalDevice) noexcept -> std::expected<SelectedDisplayPlan, ErrorCode> {
    if (vkGetPhysicalDeviceDisplayPropertiesKHR == nullptr || vkGetDisplayModePropertiesKHR == nullptr ||
        vkGetPhysicalDeviceDisplayPlanePropertiesKHR == nullptr || vkGetDisplayPlaneSupportedDisplaysKHR == nullptr) {
        return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
    }

    auto displays = FetchVulkanVector<VkDisplayPropertiesKHR>([physicalDevice](uint32_t* c, VkDisplayPropertiesKHR* d) {
        vkGetPhysicalDeviceDisplayPropertiesKHR(physicalDevice, c, d);
    });
    if (displays.empty()) {
        return std::unexpected(SurfaceCreationError::NoDisplayFound);
    }

    auto planes = FetchVulkanVector<VkDisplayPlanePropertiesKHR>([physicalDevice](uint32_t* c, VkDisplayPlanePropertiesKHR* p) {
        vkGetPhysicalDeviceDisplayPlanePropertiesKHR(physicalDevice, c, p);
    });
    if (planes.empty()) {
        return std::unexpected(SurfaceCreationError::IncompatibleDisplay);
    }

    std::optional<SelectedDisplayPlan> fallback_plan;
    bool                               saw_modes = false;

    for (const auto& display: displays) {
        auto modes = FetchVulkanVector<VkDisplayModePropertiesKHR>([physicalDevice, disp = display.display](uint32_t* c, VkDisplayModePropertiesKHR* m) {
            vkGetDisplayModePropertiesKHR(physicalDevice, disp, c, m);
        });
        if (modes.empty()) {
            continue;
        }
        saw_modes = true;

        const uint32_t plane_index = FindCompatiblePlane(physicalDevice, planes, display.display);
        if (plane_index == UINT32_MAX) {
            continue;
        }

        SelectedDisplayPlan plan {
            .display    = display,
            .mode       = SelectBestMode(modes, display.physicalResolution),
            .planeIndex = plane_index,
            .stackIndex = planes[plane_index].currentStackIndex,
        };
        if (planes[plane_index].currentDisplay == display.display) {
            return plan;
        }
        if (!fallback_plan.has_value()) {
            fallback_plan = plan;
        }
    }

    if (fallback_plan.has_value()) {
        return *fallback_plan;
    }
    return std::unexpected(saw_modes ? SurfaceCreationError::IncompatibleDisplay : SurfaceCreationError::IncompatibleModes);
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

auto HasConnectedDisplay(VkPhysicalDevice physicalDevice) noexcept -> bool {
    if (physicalDevice == VK_NULL_HANDLE) {
        return false;
    }
    return SelectDisplayPlan(physicalDevice).has_value();
}

auto CreateDisplaySurface(VkInstance instance, VkPhysicalDevice physicalDevice) noexcept
    -> std::expected<DisplaySurface, ErrorCode> {
    if (physicalDevice == VK_NULL_HANDLE || vkCreateDisplayPlaneSurfaceKHR == nullptr || vkGetDisplayPlaneCapabilitiesKHR == nullptr) {
        return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
    }

    const auto plan = SelectDisplayPlan(physicalDevice);
    if (!plan) {
        return std::unexpected(plan.error());
    }

    VkDisplayPlaneCapabilitiesKHR capabilities {};
    if (const auto res = vkGetDisplayPlaneCapabilitiesKHR(physicalDevice, plan->mode.displayMode, plan->planeIndex, &capabilities); res != VK_SUCCESS) {
        return std::unexpected(ToError(res));
    }

    VkExtent2D image_extent = plan->mode.parameters.visibleRegion;
    if (capabilities.maxDstExtent.width > 0 && capabilities.maxDstExtent.height > 0) {
        image_extent.width  = std::clamp(image_extent.width, capabilities.minDstExtent.width, capabilities.maxDstExtent.width);
        image_extent.height = std::clamp(image_extent.height, capabilities.minDstExtent.height, capabilities.maxDstExtent.height);
    }

    const VkDisplaySurfaceCreateInfoKHR create_info {
        .sType           = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .pNext           = nullptr,
        .flags           = 0,
        .displayMode     = plan->mode.displayMode,
        .planeIndex      = plan->planeIndex,
        .planeStackIndex = plan->stackIndex,
        .transform       = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha     = 1.0F,
        .alphaMode       = SelectAlpha(capabilities),
        .imageExtent     = image_extent,
    };

    VkSurfaceKHR raw_surface = VK_NULL_HANDLE;
    if (vkCreateDisplayPlaneSurfaceKHR(instance, &create_info, nullptr, &raw_surface) != VK_SUCCESS) {
        return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
    }

    ZHLN::Log(
        "[Vulkan] Created TTY display surface {}x{} @ {:.2f} Hz (plane {}, stack {}).", image_extent.width, image_extent.height,
        static_cast<double>(plan->mode.parameters.refreshRate) / 1000.0, plan->planeIndex, plan->stackIndex
    );

    return DisplaySurface {
        .surface = Surface(instance, raw_surface),
        .extent  = image_extent,
    };
}

} // namespace ZHLN::Vk
