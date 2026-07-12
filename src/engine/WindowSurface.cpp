// src/engine/WindowSurface.cpp
#include "WindowInternal.hpp"
#include <Rendering.hpp>
#include <Zahlen/Log.hpp>
#include <variant>

namespace ZHLN {

std::expected<void*, Error> Window::CreateVulkanSurface(void* instance, void* physicalDevice, int& outWidth, int& outHeight) noexcept {
    auto logger = [](const char* msg) { ZHLN::Log("{}", msg); };

    auto displaySelector = [](std::span<const VkDisplayPropertiesKHR> displays) -> VkDisplayPropertiesKHR { return displays[0]; };

    auto modeSelector = [](std::span<const VkDisplayModePropertiesKHR> modes) -> VkDisplayModePropertiesKHR { return modes[0]; };

    auto planeSelector = [](std::span<const VkDisplayPlanePropertiesKHR> planeProps, VkDisplayKHR targetDisplay, auto&& getSupported) -> uint32_t {
        auto planeCount = static_cast<uint32_t>(planeProps.size());
        for (uint32_t i = 0; i < planeCount; i++) {
            if (planeProps[i].currentDisplay != VK_NULL_HANDLE && planeProps[i].currentDisplay != targetDisplay) {
                continue;
            }
            std::vector<VkDisplayKHR> supported = getSupported(i);
            for (auto* d: supported) {
                if (d == targetDisplay) {
                    return i;
                }
            }
        }
        return UINT32_MAX;
    };

    auto alphaSelector = [](const VkDisplayPlaneCapabilitiesKHR& planeCaps) -> VkDisplayPlaneAlphaFlagBitsKHR {
        VkDisplayPlaneAlphaFlagBitsKHR alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
        if (!(planeCaps.supportedAlpha & alphaMode)) {
            if (planeCaps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR) {
                alphaMode = VK_DISPLAY_PLANE_ALPHA_GLOBAL_BIT_KHR;
            } else if (planeCaps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR) {
                alphaMode = VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_BIT_KHR;
            } else if (planeCaps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR) {
                alphaMode = VK_DISPLAY_PLANE_ALPHA_PER_PIXEL_PREMULTIPLIED_BIT_KHR;
            }
        }
        return alphaMode;
    };

    auto windowCreate = [this](VkInstance inst, uint32_t& w, uint32_t& h) -> std::expected<VkSurfaceKHR, Error> {
        if (glfwVulkanSupported() == GLFW_FALSE) {
            return std::unexpected(SurfaceCreationError::WindowSurfaceUnsupported);
        }
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkResult     err     = glfwCreateWindowSurface(inst, _impl->handle, nullptr, &surface);
        auto         check   = Vk::CheckResult(err, "Failed to create GLFW Vulkan window surface");
        if (!check) {
            return std::unexpected(SurfaceCreationError::GLFWSurfaceCreationFailed);
        }
        int f_w = 0;
        int f_h = 0;
        glfwGetFramebufferSize(_impl->handle, &f_w, &f_h);
        w = static_cast<uint32_t>(f_w);
        h = static_cast<uint32_t>(f_h);
        return surface;
    };

    uint32_t hwWidth  = 0;
    uint32_t hwHeight = 0;

    // Define a type-erased variant representing our possible configurations
    using ConfigVariant = std::variant<
        ZHLN::Vk::WindowedConfig<decltype(windowCreate)>,
        ZHLN::Vk::TTYConfig<decltype(logger), decltype(displaySelector), decltype(modeSelector), decltype(planeSelector), decltype(alphaSelector)>>;

    // Use an Immediately Invoked Function Expression (IIFE) to initialize the variant directly,
    // bypassing the default-constructibility constraint of the first alternative.
    const auto config = [&]() -> ConfigVariant {
        if (_impl->is_tty) {
            return ZHLN::Vk::TTYConfig {
                .physicalDevice = static_cast<VkPhysicalDevice>(physicalDevice),
                .log            = std::move(logger),
                .selectDisplay  = std::move(displaySelector),
                .selectMode     = std::move(modeSelector),
                .selectPlane    = std::move(planeSelector),
                .selectAlpha    = std::move(alphaSelector)
            };
        }
        return ZHLN::Vk::WindowedConfig {.windowCreate = std::move(windowCreate)};
    }();

    auto surface_res = ZHLN::Vk::Surface::Create(static_cast<VkInstance>(instance), hwWidth, hwHeight, config);

    if (!surface_res) {
        return std::unexpected(surface_res.error());
    }

    outWidth      = static_cast<int>(hwWidth);
    outHeight     = static_cast<int>(hwHeight);
    _impl->width  = hwWidth;
    _impl->height = hwHeight;

    return static_cast<void*>(surface_res->Release());
}

} // namespace ZHLN
