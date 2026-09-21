// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/core/Extensions.cpp

#include "Extensions.hpp"

#include <Zahlen/Log.hpp>
#include "NativeSurfaceInternal.hpp"
#include "RenderCore.h"

namespace ZHLN::Vk {

// Private extension-builder error (Tier 1): declared at file scope in this
// translation unit so no header exposes it.
enum class ExtensionBuilderError : uint8_t {
    MissingRequiredExtension ZHLN_ANNOTATION(ZHLN::Description<"A required Vulkan extension is missing">{}) = 1,
};

// ExtensionResult Implementation

ExtensionResult::ExtensionResult(std::vector<std::string>&& strings) noexcept: _strings(std::move(strings)) {
    RebuildPointers();
}

ExtensionResult::ExtensionResult(ExtensionResult&& other) noexcept: _strings(std::move(other._strings)) {
    RebuildPointers();
}

auto ExtensionResult::operator=(ExtensionResult&& other) noexcept -> ExtensionResult& {
    if (this != &other) {
        _strings = std::move(other._strings);
        RebuildPointers();
    }
    return *this;
}

void ExtensionResult::RebuildPointers() noexcept {
    _ptrs.clear();
    _ptrs.reserve(_strings.size());
    _views.clear();
    _views.reserve(_strings.size());
    for (const auto& s: _strings) {
        _ptrs.push_back(s.c_str());
        _views.emplace_back(s);
    }
}

// ExtensionBuilder Implementation

ExtensionBuilder::ExtensionBuilder(std::vector<std::string>&& available) noexcept: _available(std::move(available)) {
}

auto ExtensionBuilder::ForDevice(VkPhysicalDevice physical) noexcept -> ExtensionBuilder {
    return ExtensionBuilder(TemplatedDetail::ExtensionNames(EnumerateDeviceExtensions(physical)));
}

auto ExtensionBuilder::ForInstance() noexcept -> ExtensionBuilder {
    // EnumerateInstanceExtensions acquires the loader. An empty list makes
    // every Require() report missing instead of touching a NULL dispatch pointer.
    return ExtensionBuilder(TemplatedDetail::ExtensionNames(EnumerateInstanceExtensions()));
}

auto ExtensionBuilder::Require(std::string_view name) noexcept -> ExtensionBuilder& {
    if (IsSupported(name)) {
        if (const auto* matched = FindAvailable(name)) {
            _active.push_back(*matched);
        }
    } else {
        _missingRequired.emplace_back(name);
    }
    return *this;
}

auto ExtensionBuilder::Optional(std::string_view name) noexcept -> ExtensionBuilder& {
    if (IsSupported(name)) {
        if (const auto* matched = FindAvailable(name)) {
            _active.push_back(*matched);
        }
    }
    return *this;
}

auto ExtensionBuilder::OptionalGroup(std::initializer_list<std::string_view> names, bool condition) noexcept -> ExtensionBuilder& {
    if (!condition) {
        return *this;
    }

    bool all_supported = true;
    for (auto name: names) {
        if (!IsSupported(name)) {
            all_supported = false;
            break;
        }
    }

    if (all_supported) {
        for (auto name: names) {
            if (const auto* matched = FindAvailable(name)) {
                _active.push_back(*matched);
            }
        }
    }
    return *this;
}

auto ExtensionBuilder::Build() noexcept -> std::expected<ExtensionResult, ZHLN::ErrorCode> {
    if (!_missingRequired.empty()) {
        // Name the culprits: the fatal-error path prints only the error
        // enumerator, which says nothing about which extension the driver
        // lacks (and says even less on builds compiled with
        // ZHLN_NO_ANNOTATION_EXTRACT, where the Description text is gone).
        for (const auto& name: _missingRequired) {
            ZHLN::Log("[Extensions] Required Vulkan extension not supported by this driver: {}", name);
        }
        return std::unexpected(ExtensionBuilderError::MissingRequiredExtension);
    }
    return ExtensionResult(std::move(_active));
}

[[nodiscard]] bool ExtensionBuilder::IsSupported(std::string_view name) const noexcept {
    return std::ranges::contains(_available, name);
}

auto ExtensionBuilder::FindAvailable(std::string_view name) const noexcept -> const std::string* {
    auto it = std::ranges::find(_available, name);
    return it != _available.end() ? &(*it) : nullptr;
}

// The presentation bridge's consumer side. One arm per platform descriptor the
// window subsystem can publish; the arm says which WSI that descriptor's surface
// is created through, and nothing else. See
// src/vulkan/presentation/Surface.cpp for the matching vkCreate*SurfaceKHR calls
// -- the two have to agree, and they are the only two places that know.
void AppendPlatformSurfaceExtensions(ExtensionBuilder& builder, const NativeSurfaceHandle& handle) noexcept {
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
        handle,
        Overloaded {
            [&](const Win32Target&) {
                requireWsi();
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
                // over it would be. Surface.cpp asks for the xlib entry point
                // and reports unsupported if the loader has neither.
                builder.Optional("VK_KHR_xlib_surface").Optional("VK_KHR_xcb_surface");
            },
            [&](const DrmTarget&) {
                // Direct to display: VK_KHR_display builds the surface from the
                // physical device, so the platform extension is the display one.
                // This is the list the TTY backend asked for before the bridge
                // existed, unchanged.
                requireWsi();
                builder.Require(VK_KHR_DISPLAY_EXTENSION_NAME);
            },
            [](const auto&) {
                // HeadlessTarget: no WSI in this session.
                // CocoaTarget: macOS has no native Vulkan WSI, and a windowed
                // session there presents through the host-blit plugin's own
                // OpenGL window. Requesting surface extensions on macOS makes
                // instance creation fail outright, so both ask for nothing.
            },
        }
    );
}

} // namespace ZHLN::Vk
