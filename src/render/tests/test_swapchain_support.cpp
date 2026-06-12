// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


// src/render/tests/test_swapchain_support.cpp

#include "RenderCore.h"
#include <print>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>
#endif

extern int s_passed, s_failed;
#define EXPECT(cond) do { \
    if (!(cond)) { std::println("  FAIL: {}  ({}:{})", #cond, __FILE__, __LINE__); ++s_failed; } \
    else { ++s_passed; } \
} while(0)

#ifdef _WIN32
static HWND CreateHiddenWindow(HINSTANCE instance) {
    const wchar_t* kClassName = L"ZahlenSwapchainSupportTestClass";

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return nullptr;
    }

    HWND hwnd = CreateWindowExW(
        0,
        kClassName,
        L"",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        1,
        1,
        nullptr,
        nullptr,
        instance,
        nullptr);

    return hwnd;
}
#endif

void test_swapchain_support() {
    std::println("=== swapchain ===");

#ifdef _WIN32
    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };

    ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
    inst_desc.enable_validation = false;
    inst_desc.extension_count = static_cast<uint32_t>(sizeof(extensions) / sizeof(extensions[0]));
    inst_desc.extensions = extensions;

    VkInstance instance = ZHLN_CreateInstance(&inst_desc);
    if (instance == VK_NULL_HANDLE) {
        std::println("  SKIP: failed to create Vulkan instance");
        std::exit(77);
    }

    HINSTANCE module = GetModuleHandleW(nullptr);
    HWND hwnd = CreateHiddenWindow(module);
    if (hwnd == nullptr) {
        std::println("  SKIP: failed to create hidden Win32 window");
        vkDestroyInstance(instance, nullptr);
        std::exit(77);
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkWin32SurfaceCreateInfoKHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .hinstance = module,
        .hwnd = hwnd,
    };

    if (vkCreateWin32SurfaceKHR(instance, &surface_info, nullptr, &surface) != VK_SUCCESS) {
        std::println("  SKIP: failed to create Win32 surface");
        DestroyWindow(hwnd);
        vkDestroyInstance(instance, nullptr);
        std::exit(77);
    }

    ZHLN_DeviceSelectDesc sel = {
        .instance = instance,
        .surface = surface,
        .score_fn = nullptr,
        .score_userdata = nullptr,
    };
    ZHLN_PhysicalDeviceInfo physical = ZHLN_SelectPhysicalDevice(&sel);
    if (physical.handle == VK_NULL_HANDLE) {
        std::println("  SKIP: no physical device supports the test surface");
        vkDestroySurfaceKHR(instance, surface, nullptr);
        DestroyWindow(hwnd);
        vkDestroyInstance(instance, nullptr);
        std::exit(77);
    }

    ZHLN_SwapchainSupportDesc support_desc = {
        .physical = physical.handle,
        .surface = surface,
    };
    ZHLN_SwapchainSupport support = ZHLN_QuerySwapchainSupport(&support_desc);

    EXPECT(support.format_count > 0);
    EXPECT(support.present_mode_count > 0);
    EXPECT(support.capabilities.minImageCount > 0);
    EXPECT(support.capabilities.maxImageCount == 0 ||
           support.capabilities.maxImageCount >= support.capabilities.minImageCount);

    if (support.format_count > 0) {
        EXPECT(support.formats[0].format != VK_FORMAT_UNDEFINED);
    }

    for (uint32_t i = 0; i < support.present_mode_count; ++i) {
        VkPresentModeKHR present_mode = support.present_modes[i];
        const char* name = "UNKNOWN";
        bool known = false;
        if (present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR) { name = "IMMEDIATE"; known = true; }
        else if (present_mode == VK_PRESENT_MODE_MAILBOX_KHR) { name = "MAILBOX"; known = true; }
        else if (present_mode == VK_PRESENT_MODE_FIFO_KHR) { name = "FIFO"; known = true; }
        else if (present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) { name = "FIFO_RELAXED"; known = true; }
        else if (present_mode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR) { name = "SHARED_DEMAND_REFRESH"; known = true; }
        else if (present_mode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR) { name = "SHARED_CONTINUOUS_REFRESH"; known = true; }
        else if (present_mode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR) { name = "FIFO_LATEST_READY"; known = true; }

        std::println("  present mode[{}] = {} ({})", i, (unsigned)present_mode, name);
        EXPECT(known);
    }

    vkDestroySurfaceKHR(instance, surface, nullptr);
    DestroyWindow(hwnd);
    vkDestroyInstance(instance, nullptr);
#else
    std::println("  SKIP: swapchain support test is only implemented on Win32 in this repository");
    std::exit(77);
#endif
}
