// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PresentationTarget.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Window.hpp>
#include <string>

namespace ZHLN {
struct Window::Impl {
    GLFWwindow*                        handle           = nullptr;
    WindowInputReceiver                receiver         = {};
    CursorMode                         cursorMode       = CursorMode::System;
    bool                               mouseCaptured    = false;
    bool                               quitProcess      = false;
    bool                               superDown        = false;
    std::array<bool, kMaxGamepads>     gamepadConnected = {};
    std::string                        localClipboard;
    PresentationTarget                 target;
};
}
