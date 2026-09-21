// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/window/WindowInternal.hpp
#pragma once
// No Vulkan here, and no volk.h ahead of GLFW: this subsystem does not know a
// VkSurfaceKHR exists. The window publishes what the OS gave it as an opaque
// NativeSurfaceHandle (see NativeSurfaceInternal.hpp) and src/vulkan/ turns that
// into a surface, which is also why GLFW_INCLUDE_VULKAN is gone -- there is no
// loader-prototype ordering left to get wrong.
#include "NativeSurfaceInternal.hpp"
#include "PresentationTarget.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Window.hpp>
#include <string>

namespace ZHLN {
// The presentation target behind the facade. Window is what a client holds and
// what <Zahlen/Window.hpp> declares; this is what the renderer is actually
// given, reached through Window::GetPresentationTarget(). Deriving here rather
// than on Window itself is the point: a base class has to be complete where the
// class is declared, so a Window that inherited IPresentationTarget would drag
// this subsystem's private header back into the public one. Composition keeps
// the seam on this side of the line.
//
// The overrides are declared here and defined out of line in Window.cpp, which
// is this header's only translation unit -- that gives Impl a key function, so
// its vtable is emitted once instead of weakly (-Wweak-vtables).
struct Window::Impl: IPresentationTarget {
    GLFWwindow*         handle   = nullptr;
    WindowInputReceiver receiver = {}; // Platform-neutral callbacks into ECS registry
    bool                is_tty   = false;
    bool                headless = false;
    // mutable: Close() is const on IPresentationTarget, and this is the one
    // field it writes.
    mutable bool is_running  = true;  // Managed internally in headless mode
    bool         quitProcess = false; // Super/Ctrl+Q; Engine closes the primary window
    bool         superDown   = false; // Super key events often never reach the client on Hyprland
    void*        tty_context = nullptr;
    uint32_t     width       = 0;
    uint32_t     height      = 0;
    std::string  localClipboard; // TTY / headless stand-in for the OS clipboard
    // What the renderer is handed: the platform descriptor for this window,
    // built once the window exists (see Window::RebuildNativeSurface). Empty
    // for a window that has none, which is what makes "unsupported" a value
    // rather than a crash.
    NativeSurfaceHandle surface;

    // --- IPresentationTarget
    [[nodiscard]] auto GetFramebufferExtent() const noexcept -> Extent2D override;
    void               SetFramebufferExtent(uint32_t width, uint32_t height) noexcept override;
    [[nodiscard]] auto GetNativeSurface() const noexcept -> const NativeSurfaceHandle& override;
    [[nodiscard]] auto IsHeadless() const noexcept -> bool override;
    [[nodiscard]] auto IsTTY() const noexcept -> bool override;
    void               Close() const noexcept override;
};
} // namespace ZHLN
