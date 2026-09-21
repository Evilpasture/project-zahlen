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
#include <GLFW/glfw3.h>
#include <Zahlen/PresentationTarget.hpp>
#include <Zahlen/Window.hpp>
#include <string>

namespace ZHLN {
struct Window::Impl {
    GLFWwindow*         handle      = nullptr;
    WindowInputReceiver receiver    = {}; // Platform-neutral callbacks into ECS registry
    bool                is_tty      = false;
    bool                headless    = false;
    bool                is_running  = true;  // Managed internally in headless mode
    bool                quitProcess = false; // Super/Ctrl+Q; Engine closes the primary window
    bool                superDown   = false; // Super key events often never reach the client on Hyprland
    void*               tty_context = nullptr;
    uint32_t            width       = 0;
    uint32_t            height      = 0;
    std::string         localClipboard; // TTY / headless stand-in for the OS clipboard
    // What the renderer is handed: the platform descriptor for this window,
    // built once the window exists (see Window::RebuildNativeSurface). Empty
    // for a window that has none, which is what makes "unsupported" a value
    // rather than a crash.
    NativeSurfaceHandle surface;
};
} // namespace ZHLN
