// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineGlobals.hpp"
#include <GLFW/glfw3.h>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <cstdint>
#include <cstdlib>
#include <mutex>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
// clang-format on
#include <Zahlen/Log.hpp>
#include <renderdoc_app.h>
#ifdef __linux__
#include <dlfcn.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace ZHLN {

static RENDERDOC_API_1_5_0* s_RDocAPI = nullptr;

void InitRenderDocAPI() {
#if defined(_WIN32)
    if (HMODULE mod = GetModuleHandleA("renderdoc.dll")) {
        // GetProcAddress returns a FARPROC, whose signature is not this one; the
        // hop through void* is the one the dlsym path below also makes, and the
        // one GCC accepts without -Wcast-function-type.
        auto R_GetAPI = reinterpret_cast<pRENDERDOC_GetAPI>(reinterpret_cast<void*>(GetProcAddress(mod, "RENDERDOC_GetAPI")));
        if (R_GetAPI != nullptr) {
            R_GetAPI(eRENDERDOC_API_Version_1_5_0, reinterpret_cast<void**>(&s_RDocAPI));
        }
    }
#elif defined(__linux__)
    if (void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD)) {
        auto R_GetAPI = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
        if (R_GetAPI != nullptr) {
            R_GetAPI(eRENDERDOC_API_Version_1_5_0, reinterpret_cast<void**>(&s_RDocAPI));
        }
    }
#endif
    if (s_RDocAPI != nullptr) {
        ZHLN::Log("[RenderDoc] In-App API successfully bound.");
    }
}

namespace {

ZHLN::Mutex s_JoltRegistrationMutex;
uint32_t    s_JoltRegistrations = 0;

ZHLN::Mutex s_GlfwMutex;
uint32_t    s_GlfwUsers  = 0;
bool        s_GlfwInited = false;

}

void AcquireJoltRegistration() {
    const MutexGuard lock(s_JoltRegistrationMutex);
    const uint32_t   previous = s_JoltRegistrations;
    ++s_JoltRegistrations;
    if (previous > 0) {
        return;
    }

    JPH::RegisterDefaultAllocator();
    JPH::Trace = JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = JoltAssertBridge;
#endif

    if (JPH::Factory::sInstance == nullptr) {
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

void ReleaseJoltRegistration() {
    const MutexGuard lock(s_JoltRegistrationMutex);
    if (s_JoltRegistrations == 0) {
        return;
    }
    --s_JoltRegistrations;
    if (s_JoltRegistrations > 0) {
        return;
    }

    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

auto AcquireGlfw() -> bool {
    const MutexGuard lock(s_GlfwMutex);
    if (s_GlfwInited) {
        ++s_GlfwUsers;
        return true;
    }


    glfwSetErrorCallback([](int error, const char* description) -> void {
        ZHLN::Log("[GLFW Error] Code {}: {}", error, description ? description : "(null)");
    });

    if constexpr (isLinux) {
        if (std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE") != nullptr || std::getenv("NOMAD_VULKAN_LAYER") != nullptr ||
            std::getenv("NGFX_INJECTION") != nullptr) {
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        }
    }

    if (!glfwInit()) {
        const char* desc = nullptr;
        const int   err  = glfwGetError(&desc);
        if (desc != nullptr) {
            ZHLN::Log("[GLFW] glfwInit failed: ({}) {}", err, desc);
        }
        return false;
    }
    s_GlfwInited = true;
    s_GlfwUsers  = 1;
    return true;
}

void ReleaseGlfw() {
    const MutexGuard lock(s_GlfwMutex);
    if (s_GlfwUsers == 0) {
        return;
    }
    --s_GlfwUsers;
    if (s_GlfwUsers > 0) {
        return;
    }
    if (s_GlfwInited) {
        glfwTerminate();
        s_GlfwInited = false;
    }
}

}
