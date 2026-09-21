// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/presentation/Surface.cpp

#include "Surface.hpp"
#include "NativeSurfaceInternal.hpp"

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

// --- The platform WSI entry points, resolved through the loader.
//
// Volk declares vkCreateWin32SurfaceKHR & friends only for the platforms its own
// VK_USE_PLATFORM_* macros are set for, and this project sets none of them:
// VK_USE_PLATFORM_WAYLAND_KHR, _XCB_KHR and _XLIB_KHR would each pull a system
// header (wayland-client.h, xcb/xcb.h, X11/Xlib.h) into the RHI, which is
// exactly the dependency the presentation bridge exists to remove.
//
// What is header-free and stable is the loader. vkGetInstanceProcAddr is a Volk
// global from ZHLN_EnsureVulkanLoader() on, and asking an *instance* for an
// extension's entry point is the specified way to get one (it is also what GLFW
// does internally, which is the other half of why nothing here needs GLFW). The
// create-info layouts below are frozen by the specification: every field is a
// pointer or a fixed-width integer, so spelling the OS handle as void* is
// ABI-identical to the real struct, and the sType enumerators come from
// vulkan_core.h, which is always present.
//
// A null function pointer is not an error to work around, it is the answer: it
// means this loader has no such WSI, and the caller is told "unsupported".

struct Win32SurfaceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    void*           hinstance; // HINSTANCE
    void*           hwnd;      // HWND
};

struct WaylandSurfaceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    void*           display; // struct wl_display*
    void*           surface; // struct wl_surface*
};

struct XlibSurfaceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    void*           dpy;   // Display*
    unsigned long   window = 0; // Window, an XID
};

struct MetalSurfaceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    const void*     layer; // const CAMetalLayer*
};

template <typename CreateInfo>
using CreateSurfaceFn = VkResult(VKAPI_PTR*)(VkInstance, const CreateInfo*, const VkAllocationCallbacks*, VkSurfaceKHR*);

template <typename CreateInfo>
[[nodiscard]] auto CreatePlatformSurface(VkInstance instance, const char* entryPoint, const char* extension, CreateInfo info) noexcept
    -> std::expected<ZHLN::Vk::Surface, ZHLN::ErrorCode> {
    const auto create = reinterpret_cast<CreateSurfaceFn<CreateInfo>>(vkGetInstanceProcAddr(instance, entryPoint));
    if (create == nullptr) {
        // The extension was not enabled on this instance, or this loader has no
        // such WSI at all. Either way there is nothing to build a surface with,
        // and saying which is worth the line: it is the difference between "the
        // driver is old" and "the instance was built without the extension".
        ZHLN::Log("[Vk::Surface] {} is unavailable; {} was not enabled on the instance.", entryPoint, extension);
        return std::unexpected(ZHLN::Vk::SurfaceCreationError::WindowSurfaceUnsupported);
    }

    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    if (create(instance, &info, nullptr, &rawSurface) != VK_SUCCESS || rawSurface == VK_NULL_HANDLE) {
        ZHLN::Log("[Vk::Surface] {} failed for this window.", entryPoint);
        return std::unexpected(ZHLN::Vk::SurfaceCreationError::WindowSurfaceCreationFailed);
    }
    return ZHLN::Vk::Surface(instance, rawSurface);
}

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
        vkGetPhysicalDeviceDisplayModePropertiesKHR(physicalDevice, display, c, m);
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

auto CreateSurfaceFromNative(VkInstance instance, const NativeSurfaceHandle& handle) noexcept -> std::expected<Surface, ErrorCode> {
    if (!handle.Valid()) {
        // A window that never opened has no descriptor. This is the same answer
        // a platform with no WSI gets, because to the renderer it is the same
        // situation: there is nothing here to present to.
        return std::unexpected(SurfaceCreationError::WindowSurfaceUnsupported);
    }

    return Visit(
        handle,
        Overloaded {
            [instance](const Win32Target& w) -> std::expected<Surface, ErrorCode> {
                if (w.hwnd == nullptr) {
                    return std::unexpected(SurfaceCreationError::WindowSurfaceUnsupported);
                }
                return CreatePlatformSurface(
                    instance, "vkCreateWin32SurfaceKHR", "VK_KHR_win32_surface",
                    Win32SurfaceCreateInfo {
                        .sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                        .pNext     = nullptr,
                        .flags     = 0,
                        .hinstance = w.hinstance,
                        .hwnd      = w.hwnd,
                    }
                );
            },
            [instance](const WaylandTarget& w) -> std::expected<Surface, ErrorCode> {
                if (w.display == nullptr || w.surface == nullptr) {
                    return std::unexpected(SurfaceCreationError::WindowSurfaceUnsupported);
                }
                return CreatePlatformSurface(
                    instance, "vkCreateWaylandSurfaceKHR", "VK_KHR_wayland_surface",
                    WaylandSurfaceCreateInfo {
                        .sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                        .pNext   = nullptr,
                        .flags   = 0,
                        .display = w.display,
                        .surface = w.surface,
                    }
                );
            },
            [instance](const X11Target& x) -> std::expected<Surface, ErrorCode> {
                if (x.display == nullptr || x.window == 0) {
                    return std::unexpected(SurfaceCreationError::WindowSurfaceUnsupported);
                }
                // Xlib, not xcb: what the window side can hand over without
                // linking an X library is the Display* and the XID, and the
                // xlib surface takes exactly those. Both extensions are asked
                // for as optional (see AppendPlatformSurfaceExtensions), so a
                // loader without xlib reports unsupported here rather than
                // failing instance creation earlier.
                return CreatePlatformSurface(
                    instance, "vkCreateXlibSurfaceKHR", "VK_KHR_xlib_surface",
                    XlibSurfaceCreateInfo {
                        .sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
                        .pNext  = nullptr,
                        .flags  = 0,
                        .dpy    = x.display,
                        .window = x.window,
                    }
                );
            },
            [instance](const CocoaTarget& c) -> std::expected<Surface, ErrorCode> {
                if (c.caMetalLayer == nullptr) {
                    // No native Vulkan WSI on macOS: the session renders
                    // offscreen and the host-blit plugin presents it. A null
                    // surface is that state, not a failure.
                    return Surface(instance, VK_NULL_HANDLE);
                }
                return CreatePlatformSurface(
                    instance, "vkCreateMetalSurfaceEXT", "VK_EXT_metal_surface",
                    MetalSurfaceCreateInfo {
                        .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
                        .pNext = nullptr,
                        .flags = 0,
                        .layer = c.caMetalLayer,
                    }
                );
            },
            [instance](const HeadlessTarget&) -> std::expected<Surface, ErrorCode> {
                // Offscreen session: no WSI, no surface, nothing to fail. The
                // renderer reads a null handle as "nothing to present to".
                return Surface(instance, VK_NULL_HANDLE);
            },
            [](const DrmTarget&) -> std::expected<Surface, ErrorCode> {
                // Direct-to-display is built from the physical device, which
                // this entry point is never called with: the caller takes
                // CreateDisplaySurface instead. Reaching here means a DRM
                // target was handed to the windowed path.
                return std::unexpected(SurfaceCreationError::TTYSurfaceCreationFailed);
            },
        }
    );
}

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
