// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/WindowInternal.hpp
#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <Zahlen/Window.hpp>

namespace ZHLN {
struct Window::Impl {
    GLFWwindow*         handle      = nullptr;
    WindowInputReceiver receiver    = {}; // Platform-neutral callbacks into ECS registry
    bool                is_tty      = false;
    bool                headless    = false;
    void*               tty_context = nullptr;
    uint32_t            width       = 0;
    uint32_t            height      = 0;
};
} // namespace ZHLN
