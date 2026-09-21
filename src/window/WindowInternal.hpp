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
// The state behind the facade. Window is what a client holds and what
// <Zahlen/Window.hpp> declares; the renderer is handed the PresentationTarget
// this composes, through Window::GetPresentationTarget().
//
// Composition rather than inheritance, and no longer for the reason it used to
// be: PresentationTarget is a concrete value type now, so there is no base class
// to complete at the point of declaration. Holding it as a member is what gives
// it the stable address the destination registry keys on, and it is what lets
// the window swap its native descriptor in place on a reconnect without the
// renderer's key moving.
struct Window::Impl {
    // A desktop window and nothing else. The headless and KMS/DRM sessions that
    // used to be flags here are their own PlatformHost backends now (see
    // PlatformHost.cpp), so there is no mode to branch on and no TTY context to
    // hold.
    GLFWwindow*         handle      = nullptr;
    WindowInputReceiver receiver    = {};    // Platform-neutral callbacks into ECS registry
    bool                quitProcess = false; // Super/Ctrl+Q; Engine closes the primary window
    bool                superDown   = false; // Super key events often never reach the client on Hyprland
    std::string         localClipboard;      // fallback when the OS clipboard is unavailable
    // What the renderer is handed: this window's extent, its close hook and the
    // platform descriptor for it. Installed once the window exists and its
    // descriptor is rebuilt in place after that (see Window::RebuildNativeSurface).
    PresentationTarget target;
};
} // namespace ZHLN
