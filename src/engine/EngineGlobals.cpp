// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/EngineGlobals.cpp
#include "EngineGlobals.hpp"

#include <GLFW/glfw3.h>
#include <cstdint>
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
        pRENDERDOC_GetAPI R_GetAPI = (pRENDERDOC_GetAPI) GetProcAddress(mod, "RENDERDOC_GetAPI");
        if (R_GetAPI) {
            R_GetAPI(eRENDERDOC_API_Version_1_5_0, (void**) &s_RDocAPI);
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

// --- PROCESS-GLOBAL JOLT REGISTRATION ---
//
// JPH::Factory::sInstance and the registered type list are process state, not
// engine state. Acquisition was already guarded, but release was not: the first
// engine destroyed called JPH::UnregisterTypes() and deleted the factory out
// from under every other engine in the process. That is one of the things that
// made a second engine unusable, and it blocks running more than one physics
// world. Refcounted: first in registers, last out unregisters.
namespace {

std::mutex s_JoltRegistrationMutex;
uint32_t   s_JoltRegistrations = 0;

std::mutex s_GlfwMutex;
uint32_t   s_GlfwUsers  = 0;
bool       s_GlfwInited = false;

} // namespace

void AcquireJoltRegistration() {
    const std::lock_guard lock(s_JoltRegistrationMutex);
    const uint32_t previous = s_JoltRegistrations;
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
    const std::lock_guard lock(s_JoltRegistrationMutex);
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

// GLFW is process-global the same way. Extra windows on one engine (and a
// second windowed engine) must not glfwTerminate() while another window still
// needs it. First in inits, last out terminates.
auto AcquireGlfw() -> bool {
    const std::lock_guard lock(s_GlfwMutex);
    if (s_GlfwInited) {
        ++s_GlfwUsers;
        return true;
    }
    if (!glfwInit()) {
        return false;
    }
    s_GlfwInited = true;
    s_GlfwUsers  = 1;
    return true;
}

void ReleaseGlfw() {
    const std::lock_guard lock(s_GlfwMutex);
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

} // namespace ZHLN
