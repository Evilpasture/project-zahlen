// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "NativeSurfaceInternal.hpp"
#include "PresentationTarget.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Window.hpp>
#include <string>

namespace ZHLN {
struct Window::Impl {
    GLFWwindow*         handle      = nullptr;
    WindowInputReceiver receiver    = {};
    bool                quitProcess = false;
    bool                superDown   = false;
    std::string         localClipboard;
    PresentationTarget target;
};
}
