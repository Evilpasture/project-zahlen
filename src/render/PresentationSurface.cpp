// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "PresentationSurface.hpp"
#include "NativeSurfaceInternal.hpp"
#include "Rendering.hpp"
#include <Zahlen/Log.hpp>

namespace {


struct Win32SurfaceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    void*           hinstance;
    void*           hwnd;
};

struct WaylandSurfaceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    void*           display;
    void*           surface;
};

struct XlibSurfaceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    void*           dpy;
    unsigned long   window = 0;
};

struct MetalSurfaceCreateInfo {
    VkStructureType sType;
    const void*     pNext;
    VkFlags         flags;
    const void*     layer;
};

template <typename CreateInfo>
using CreateSurfaceFn = VkResult(VKAPI_PTR*)(VkInstance, const CreateInfo*, const VkAllocationCallbacks*, VkSurfaceKHR*);

template <typename CreateInfo>
[[nodiscard]] auto CreatePlatformSurface(VkInstance instance, const char* entryPoint, const char* extension, CreateInfo info) noexcept
    -> std::expected<ZHLN::Vk::Surface, ZHLN::ErrorCode> {
    const auto create = reinterpret_cast<CreateSurfaceFn<CreateInfo>>(vkGetInstanceProcAddr(instance, entryPoint));
    if (create == nullptr) {
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

}

namespace ZHLN {

auto CreateSurfaceFromNative(VkInstance instance, const NativeSurfaceHandle& handle) noexcept -> std::expected<Vk::Surface, ErrorCode> {
    if (!handle.Valid()) {
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
                        return Vk::Surface(instance, VK_NULL_HANDLE);
                    },
                    [](const DrmTarget&) -> std::expected<Vk::Surface, ErrorCode> {
                        return std::unexpected(Vk::SurfaceCreationError::TTYSurfaceCreationFailed);
                    },
                }
    );
}

void AppendPlatformSurfaceExtensions(Vk::ExtensionBuilder& builder, const NativeSurfaceHandle& handle) noexcept {
    if (!handle.Valid()) {
        return;
    }

    const auto requireWsi = [&builder]() noexcept -> void {
        builder.Require(VK_KHR_SURFACE_EXTENSION_NAME)
            .Require(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)
            .Require(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    };

    Visit(
        handle, Overloaded {
                    [&](const Win32Target&) {
                        requireWsi();
                        builder.Require("VK_KHR_win32_surface");
                    },
                    [&](const WaylandTarget&) {
                        requireWsi();
                        builder.Require("VK_KHR_wayland_surface");
                    },
                    [&](const X11Target&) {
                        requireWsi();
                        builder.Optional("VK_KHR_xlib_surface").Optional("VK_KHR_xcb_surface");
                    },
                    [&](const DrmTarget&) {
                        requireWsi();
                        builder.Require(VK_KHR_DISPLAY_EXTENSION_NAME);
                    },
                    [](const auto&) {
                    },
                }
    );
}

}
