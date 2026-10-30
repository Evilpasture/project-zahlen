// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Platform.hpp"
#include <Zahlen/Window/Window.hpp>
#include <chrono>
#include <thread>

#include <GLFW/glfw3.h>
#include <Zahlen/Core/Platform.hpp>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#if defined(_MSC_VER) // MSVC autolink; see the same guard in Platform.hpp
#pragma comment(lib, "Shcore.lib")
#endif
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace ZHLN {

auto GetPID() noexcept -> int {
#ifdef _WIN32
    return static_cast<int>(::GetCurrentProcessId());
#else
    return ::getpid();
#endif
}

void HaltThread() noexcept {
#if defined(_WIN32)
    ::Sleep(INFINITE);
#else
    ::pause();
#endif
}

} // namespace ZHLN

namespace ZHLN::Platform {

void SetHighPriority() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

void Init() {
#ifdef _WIN32
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif
}

void FocusWindow(Window& window) {
#ifdef _WIN32
    auto* glfwHandle = static_cast<GLFWwindow*>(window.GetNativeHandle());
    if (!glfwHandle)
        return;

    HWND hwnd = glfwGetWin32Window(glfwHandle);
    if (hwnd) {
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
#else
    auto* glfwHandle = static_cast<GLFWwindow*>(window.GetNativeHandle());
    if (glfwHandle) {
        glfwFocusWindow(glfwHandle);
    }
#endif
}

float GetDisplayScale(Window& window) {
#ifdef _WIN32
    auto* glfwHandle = static_cast<GLFWwindow*>(window.GetNativeHandle());
    if (!glfwHandle)
        return 1.0f;

    HWND hwnd = glfwGetWin32Window(glfwHandle);
    return static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
#else
    auto* glfwHandle = static_cast<GLFWwindow*>(window.GetNativeHandle());
    if (glfwHandle) {
        float xscale, yscale;
        glfwGetWindowContentScale(glfwHandle, &xscale, &yscale);
        return xscale;
    }
    return 1.0f;
#endif
}

void Sleep(uint32_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}
