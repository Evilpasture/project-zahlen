// src/render/Surface.hpp
#pragma once

#include <Zahlen/render/RenderCode.hpp>

namespace ZHLN::Vk {

// --- Monadic Configuration Typestates ---

template <typename WindowCreateCallback>
struct WindowedConfig {
    WindowCreateCallback windowCreate;
};

template <typename LogCallback, typename DisplaySelector, typename ModeSelector, typename PlaneSelector, typename AlphaSelector>
struct TTYConfig {
    VkPhysicalDevice physicalDevice;
    LogCallback      log;
    DisplaySelector  selectDisplay;
    ModeSelector     selectMode;
    PlaneSelector    selectPlane;
    AlphaSelector    selectAlpha;
};

// --- Zero-Cost Monadic Vulkan Enumeration Helpers ---

template <typename T, typename F>
auto FetchVulkanVector(F&& enumerator) {
    uint32_t count = 0;
    enumerator(&count, nullptr);
    std::vector<T> vec(count);
    if (count > 0) {
        enumerator(&count, vec.data());
    }
    return vec;
}

struct SurfacePipeline {
    // Monadic step to extract and validate the physical display
    template <typename Log, typename Selector>
    static auto SelectDisplay(VkPhysicalDevice pd, Log&& log, Selector&& selectDisplay) -> std::expected<VkDisplayPropertiesKHR, Error> {
        auto displays =
            FetchVulkanVector<VkDisplayPropertiesKHR>([pd](uint32_t* c, VkDisplayPropertiesKHR* d) { vkGetPhysicalDeviceDisplayPropertiesKHR(pd, c, d); });

        if (displays.empty()) {
            std::forward<Log>(log)("[Vk::Surface] FATAL: No displays found via VK_KHR_display");
            return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
        }

        auto target = std::forward<Selector>(selectDisplay)(std::span<const VkDisplayPropertiesKHR>(displays));
        std::forward<Log>(log)(std::format("[Vk::Surface] Using Display: {}", target.displayName ? target.displayName : "Unknown").c_str());
        return target;
    }

    // Monadic step to fetch and select the display mode
    template <typename Log, typename Selector>
    static auto SelectMode(VkPhysicalDevice pd, VkDisplayKHR display, Log&& log, Selector&& selectMode) -> std::expected<VkDisplayModePropertiesKHR, Error> {
        auto modes = FetchVulkanVector<VkDisplayModePropertiesKHR>([pd, display](uint32_t* c, VkDisplayModePropertiesKHR* m) {
            vkGetDisplayModePropertiesKHR(pd, display, c, m);
        });

        if (modes.empty()) {
            std::forward<Log>(log)("[Vk::Surface] FATAL: No compatible display modes found!");
            return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
        }

        return std::forward<Selector>(selectMode)(std::span<const VkDisplayModePropertiesKHR>(modes));
    }

    // Monadic step to query and isolate the surface plane
    template <typename Log, typename Selector>
    static auto SelectPlane(VkPhysicalDevice pd, VkDisplayKHR display, Log&& log, Selector&& selectPlane) -> std::expected<uint32_t, Error> {
        auto planes = FetchVulkanVector<VkDisplayPlanePropertiesKHR>([pd](uint32_t* c, VkDisplayPlanePropertiesKHR* p) {
            vkGetPhysicalDeviceDisplayPlanePropertiesKHR(pd, c, p);
        });

        auto get_supported_displays = [pd](uint32_t planeIndex) {
            return FetchVulkanVector<VkDisplayKHR>([pd, planeIndex](uint32_t* c, VkDisplayKHR* d) {
                vkGetDisplayPlaneSupportedDisplaysKHR(pd, planeIndex, c, d);
            });
        };

        uint32_t target_plane = std::forward<Selector>(selectPlane)(std::span<const VkDisplayPlanePropertiesKHR>(planes), display, get_supported_displays);

        if (target_plane == UINT32_MAX) {
            std::forward<Log>(log)("[Vk::Surface] FATAL: Could not find a compatible display plane!");
            return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
        }
        return target_plane;
    }
};

class Surface {
  public:
    Surface() = default;
    Surface(VkInstance instance, VkSurfaceKHR surface);
    ~Surface();

    Surface(const Surface&)                    = delete;
    auto operator=(const Surface&) -> Surface& = delete;

    Surface(Surface&& other) noexcept;
    auto operator=(Surface&& other) noexcept -> Surface&;

    [[nodiscard]] auto Get() const -> VkSurfaceKHR;

    [[nodiscard]] auto Release() noexcept -> VkSurfaceKHR {
        return std::exchange(_handle, VK_NULL_HANDLE);
    }

    /**
     * @brief Universal, branchless Monadic Surface Creator.
     *        Resolves the configuration variant at compile time using std::visit.
     */
    template <typename ConfigVariant>
    static std::expected<Surface, Error> Create(VkInstance instance, uint32_t& outWidth, uint32_t& outHeight, ConfigVariant&& config) {
        auto process = [&](auto&& cfg) -> std::expected<Surface, Error> {
            using ConfigType = decltype(cfg);

            // Path A: Standard windowing subsystem dispatch (GLFW/SDL)
            if constexpr (requires { std::forward<ConfigType>(cfg).windowCreate; }) {
                return std::forward<ConfigType>(cfg).windowCreate(instance, outWidth, outHeight).transform([instance](VkSurfaceKHR raw) {
                    return Surface(instance, raw);
                });
            } else {
                // Path B: Pure, un-flattened monadic direct-to-display railway (KMS/KDR)
                if (cfg.physicalDevice == VK_NULL_HANDLE) {
                    return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
                }

                // Explicitly bind references to eliminate nested lambda template deduction traps
                auto& pd  = cfg.physicalDevice;
                auto& log = cfg.log;

                return SurfacePipeline::SelectDisplay(pd, log, std::forward<ConfigType>(cfg).selectDisplay).and_then([&, pd](VkDisplayPropertiesKHR dispProps) {
                    return SurfacePipeline::SelectMode(pd, dispProps.display, log, std::forward<ConfigType>(cfg).selectMode)
                        .and_then([&, pd, disp = dispProps.display](VkDisplayModePropertiesKHR modeProps) {
                            outWidth  = modeProps.parameters.visibleRegion.width;
                            outHeight = modeProps.parameters.visibleRegion.height;
                            log(std::format("[Vk::Surface] Selected Mode: {}x{}", outWidth, outHeight).c_str());

                            return SurfacePipeline::SelectPlane(pd, disp, log, std::forward<ConfigType>(cfg).selectPlane)
                                .and_then([&, pd, mode = modeProps.displayMode](uint32_t planeIndex) -> std::expected<Surface, Error> {
                                    VkDisplayPlaneCapabilitiesKHR caps;
                                    vkGetDisplayPlaneCapabilitiesKHR(pd, mode, planeIndex, &caps);

                                    VkDisplaySurfaceCreateInfoKHR create_info = {
                                        .sType       = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
                                        .displayMode = mode,
                                        .planeIndex  = planeIndex,
                                        .transform   = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                        .globalAlpha = 1.0F,
                                        .alphaMode   = std::forward<ConfigType>(cfg).selectAlpha(caps),
                                        .imageExtent = {.width = outWidth, .height = outHeight}
                                    };

                                    VkSurfaceKHR raw_surface = VK_NULL_HANDLE;
                                    if (vkCreateDisplayPlaneSurfaceKHR(instance, &create_info, nullptr, &raw_surface) != VK_SUCCESS) {
                                        log("[Vk::Surface] FATAL: vkCreateDisplayPlaneSurfaceKHR failed!");
                                        return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
                                    }

                                    log(std::format("[Vk::Surface] Surface successfully created on Plane {}", planeIndex).c_str());
                                    return Surface(instance, raw_surface);
                                });
                        });
                });
            }
        };

        // Fallback supporting both std::variant and raw structural configurations
        if constexpr (requires { std::visit(process, std::forward<ConfigVariant>(config)); }) {
            return std::visit(process, std::forward<ConfigVariant>(config));
        } else {
            return process(std::forward<ConfigVariant>(config));
        }
    }

  private:
    VkInstance   _instance = VK_NULL_HANDLE;
    VkSurfaceKHR _handle   = VK_NULL_HANDLE;
};

} // namespace ZHLN::Vk
