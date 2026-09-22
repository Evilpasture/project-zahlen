// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Platform.hpp"
#include <Zahlen/FileSystem/MappedFile.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Window.hpp>
#include <chrono>
#include <thread>

// 1. Always include the base GLFW (and Vulkan if needed) for all platforms.
// volk.h comes first: it defines VK_NO_PROTOTYPES and preloads the Vulkan
// headers, so GLFW_INCLUDE_VULKAN below cannot leak real loader prototypes
// into a TU that later pulls in volk.h (which refuses that mix). This matters
// on Clang builds, where the engine PCH (and its volk.h) is force-included
// only after the TU's own headers have already been scanned.
#include <volk.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
// 2. Win32-specific plumbing (internally handles ifdef logic)
#include <Zahlen/Core/Platform.hpp>

#ifdef _WIN32

// 3. Only expose Win32-specific native access here
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#pragma comment(lib, "Shcore.lib")
#else
#include <dlfcn.h>
#endif

#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace ZHLN::Platform {

// MappedFile now lives in zahlen_filesystem (FS::MappedFile). Keep these as
// thin wrappers for backward compatibility — engine code that still calls
// Platform::OpenMappedFile continues to work.
MappedFile OpenMappedFile(const char* path) {
    return FS::OpenMappedFile(path);
}

void CloseMappedFile(MappedFile& file) {
    FS::CloseMappedFile(file);
}

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

void* LoadSharedLibrary(const char* path) noexcept {
#if defined(_WIN32)
    void* handle = static_cast<void*>(LoadLibraryA(path));
    if (!handle) {
        ZHLN::Log("[Platform] LoadLibraryA failed for '{}'. Error code: {}", path, GetLastError());
    }
    return handle;
#else
    void* handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        const char* err = dlerror();
        ZHLN::Log("[Platform] dlopen failed for '{}': {}", path, err ? err : "unknown error");
    }
    return handle;
#endif
}

void* GetSymbolAddress(void* handle, const char* symbol) noexcept {
    if (handle == nullptr || symbol == nullptr) {
        return nullptr;
    }
#if defined(_WIN32)
    void* addr = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), symbol));
    if (!addr) {
        ZHLN::Log("[Platform] GetProcAddress failed for symbol '{}'. Error code: {}", symbol, GetLastError());
    }
    return addr;
#else
    dlerror(); // Clear existing errors
    void*       addr = dlsym(handle, symbol);
    const char* err  = dlerror();
    if (err != nullptr) {
        ZHLN::Log("[Platform] dlsym failed for symbol '{}': {}", symbol, err);
    }
    return addr;
#endif
}

void UnloadSharedLibrary(void* handle) noexcept {
    if (handle == nullptr) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

} // namespace ZHLN::Platform
