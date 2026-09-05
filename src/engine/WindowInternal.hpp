// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/WindowInternal.hpp
#pragma once
// Volk before GLFW's Vulkan: preloads the headers with VK_NO_PROTOTYPES so
// GLFW_INCLUDE_VULKAN cannot leak loader prototypes ahead of volk.h (which
// refuses that mix) in PCH-less Clang builds.
#include <volk.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <Zahlen/Window.hpp>
#include <string>

namespace ZHLN {
struct Window::Impl {
    GLFWwindow*         handle      = nullptr;
    WindowInputReceiver receiver    = {}; // Platform-neutral callbacks into ECS registry
    bool                is_tty      = false;
    bool                headless    = false;
    bool                is_running  = true; // Managed internally in headless mode
    void*               tty_context = nullptr;
    uint32_t            width       = 0;
    uint32_t            height      = 0;
    std::string         localClipboard; // TTY / headless stand-in for the OS clipboard
};
} // namespace ZHLN
