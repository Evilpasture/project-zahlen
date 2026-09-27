// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <print>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace ZHLN {
bool LoadRenderDocLibrary() noexcept {
#if defined(_WIN32)
    HMODULE mod = LoadLibraryA("renderdoc.dll");
    if (!mod) {
        mod = LoadLibraryA("C:\\Program Files\\RenderDoc\\renderdoc.dll");
    }
    if (mod) {
        std::println("[RenderDoc] Core library loaded successfully.");
        return true;
    }
#elif defined(__linux__)
    void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_GLOBAL);
    if (mod == nullptr) {
        mod = dlopen("/usr/lib/librenderdoc.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (mod == nullptr) {
        mod = dlopen("/usr/lib/x86_64-linux-gnu/librenderdoc.so", RTLD_NOW | RTLD_GLOBAL);
    }
    if (mod != nullptr) {
        std::println("[RenderDoc] Core library loaded successfully.");
        return true;
    }
#endif
    std::println(
        stderr, "[RenderDoc] WARNING: Failed to locate RenderDoc library. Is it installed "
                "and in your system PATH?"
    );
    return false;
}
}
