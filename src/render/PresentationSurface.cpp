// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/PresentationSurface.cpp

#include "PresentationSurface.hpp"
#include "NativeSurfaceInternal.hpp"
#include "Rendering.hpp"
#include <Zahlen/Log.hpp>

namespace {

// --- The platform WSI entry points, resolved through the loader.
//
// Volk declares vkCreateWin32SurfaceKHR & friends only for the platforms its own
// VK_USE_PLATFORM_* macros are set for, and this project sets none of them:
// VK_USE_PLATFORM_WAYLAND_KHR, _XCB_KHR and _XLIB_KHR would each pull a system
// header (wayland-client.h, xcb/xcb.h, X11/Xlib.h) into the renderer, which is
// exactly the dependency the presentation seam exists to remove.
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
    void*           dpy;        // Display*
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
        ZHLN::Log("[PresentationSurface] {} is unavailable; {} was not enabled on the instance.", entryPoint, extension);
        return std::unexpected(ZHLN::Vk::SurfaceCreationError::WindowSurfaceUnsupported);
    }

    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    if (create(instance, &info, nullptr, &rawSurface) != VK_SUCCESS || rawSurface == VK_NULL_HANDLE) {
        ZHLN::Log("[PresentationSurface] {} failed for this window.", entryPoint);
        return std::unexpected(ZHLN::Vk::SurfaceCreationError::WindowSurfaceCreationFailed);
    }
    return ZHLN::Vk::Surface(instance, rawSurface);
}

} // namespace

namespace ZHLN {

auto CreateSurfaceFromNative(VkInstance instance, const NativeSurfaceHandle& handle) noexcept -> std::expected<Vk::Surface, ErrorCode> {
    if (!handle.Valid()) {
        // A window that never opened has no descriptor. This is the same answer
        // a platform with no WSI gets, because to the renderer it is the same
        // situation: there is nothing here to present to.
        return std::unexpected(Vk::SurfaceCreationError::WindowSurfaceUnsupported);
    }

    return Visit(
        handle, Overloaded {
                    [instance](const Win32Target& w) -> std::expected<Vk::Surface, ErrorCode> {
                        if (w.hwnd == nullptr) {
                            return std::unexpected(Vk::SurfaceCreationError::WindowSurfaceUnsupported);
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
                    [instance](const WaylandTarget& w) -> std::expected<Vk::Surface, ErrorCode> {
                        if (w.display == nullptr || w.surface == nullptr) {
                            return std::unexpected(Vk::SurfaceCreationError::WindowSurfaceUnsupported);
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
                    [instance](const X11Target& x) -> std::expected<Vk::Surface, ErrorCode> {
                        if (x.display == nullptr || x.window == 0) {
                            return std::unexpected(Vk::SurfaceCreationError::WindowSurfaceUnsupported);
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
                    [instance](const CocoaTarget& c) -> std::expected<Vk::Surface, ErrorCode> {
                        if (c.caMetalLayer == nullptr) {
                            // No native Vulkan WSI on macOS: the session renders
                            // offscreen and the host-blit plugin presents it. A null
                            // surface is that state, not a failure.
                            return Vk::Surface(instance, VK_NULL_HANDLE);
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
                    [instance](const HeadlessTarget&) -> std::expected<Vk::Surface, ErrorCode> {
                        // A window on a platform this build has no native backend
                        // for publishes this instead of a handle. No WSI, no
                        // surface, nothing to fail: the renderer reads a null
                        // handle as "nothing to present to". A headless *session*
                        // never reaches here -- it has no handle at all, and the
                        // Valid() guard above answers for it.
                        return Vk::Surface(instance, VK_NULL_HANDLE);
                    },
                    [](const DrmTarget&) -> std::expected<Vk::Surface, ErrorCode> {
                        // Direct-to-display is built from the physical device, which
                        // this entry point is never called with: the caller takes
                        // Vk::CreateDisplaySurface instead. Reaching here means a DRM
                        // target was handed to the windowed path.
                        return std::unexpected(Vk::SurfaceCreationError::TTYSurfaceCreationFailed);
                    },
                }
    );
}

void AppendPlatformSurfaceExtensions(Vk::ExtensionBuilder& builder, const NativeSurfaceHandle& handle) noexcept {
    if (!handle.Valid()) {
        // A window that never opened asks for nothing, which is what a headless
        // session asks for too: no surface will be built, so no WSI is needed.
        return;
    }

    // Every window-system surface path needs these two alongside the platform
    // extension: the swapchain queries capabilities through the 2 variant, and
    // maintenance1 is what lets it survive a resize without a full rebuild.
    const auto requireWsi = [&builder]() noexcept -> void {
        builder.Require(VK_KHR_SURFACE_EXTENSION_NAME)
            .Require(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)
            .Require(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    };

    Visit(
        handle, Overloaded {
                    [&](const Win32Target&) {
                        requireWsi();
                        // Spelled out, not VK_KHR_WIN32_SURFACE_EXTENSION_NAME: that
                        // macro lives in vulkan_win32.h, which only VK_USE_PLATFORM_WIN32_KHR
                        // pulls in, and this project sets none of them (see the comment
                        // on the create-info structs above). The four names that are in
                        // vulkan_core.h are used as macros.
                        builder.Require("VK_KHR_win32_surface");
                    },
                    [&](const WaylandTarget&) {
                        requireWsi();
                        // GLFW connected to Wayland, so the driver has it: requiring it
                        // turns "compositor is Wayland, driver is not" into a named
                        // missing extension instead of a null surface later.
                        builder.Require("VK_KHR_wayland_surface");
                    },
                    [&](const X11Target&) {
                        requireWsi();
                        // Optional on purpose: an X11 driver that has only the xcb
                        // variant is not a broken driver, and failing instance creation
                        // over it would be. CreateSurfaceFromNative asks for the xlib
                        // entry point and reports unsupported if the loader has neither.
                        builder.Optional("VK_KHR_xlib_surface").Optional("VK_KHR_xcb_surface");
                    },
                    [&](const DrmTarget&) {
                        // Direct to display: VK_KHR_display builds the surface from the
                        // physical device, so the platform extension is the display one.
                        // This is the list the TTY backend asked for before the seam
                        // existed, unchanged.
                        requireWsi();
                        builder.Require(VK_KHR_DISPLAY_EXTENSION_NAME);
                    },
                    [](const auto&) {
                        // HeadlessTarget: a window with no native backend in this
                        // build; no WSI to ask for.
                        // CocoaTarget: macOS has no native Vulkan WSI, and a windowed
                        // session there presents through the host-blit plugin's own
                        // OpenGL window. Requesting surface extensions on macOS makes
                        // instance creation fail outright, so both ask for nothing.
                    },
                }
    );
}

} // namespace ZHLN
