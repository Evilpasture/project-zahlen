// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


// src/render/tests/test_utils.cpp
// Pure logic — no Vulkan device needed

#include "RenderCore.h"
#include "Utils.hpp"
#include <print>
#include <cstdint>

extern int s_passed, s_failed;
#define EXPECT(cond) do { \
    if (!(cond)) { std::println("  FAIL: {}  ({}:{})", #cond, __FILE__, __LINE__); ++s_failed; } \
    else { ++s_passed; } \
} while(0)

void test_utils() {
    std::println("=== utils ===");

    // Clamp — float
    EXPECT(ZHLN::Clamp(0.5f, 0.0f, 1.0f) == 0.5f);
    EXPECT(ZHLN::Clamp(-1.0f, 0.0f, 1.0f) == 0.0f);
    EXPECT(ZHLN::Clamp(2.0f, 0.0f, 1.0f) == 1.0f);

    // Clamp — int32
    EXPECT(ZHLN::Clamp((int32_t)5, (int32_t)0, (int32_t)10) == 5);
    EXPECT(ZHLN::Clamp((int32_t)-1, (int32_t)0, (int32_t)10) == 0);
    EXPECT(ZHLN::Clamp((int32_t)11, (int32_t)0, (int32_t)10) == 10);

    // Clamp — double
    EXPECT(ZHLN::Clamp(0.5, 0.0, 1.0) == 0.5);
}

void test_errors() {
    std::println("=== errors ===");

    EXPECT(ZHLN_VkResultString(VK_SUCCESS)                  != nullptr);
    EXPECT(ZHLN_VkResultString(VK_ERROR_OUT_OF_DATE_KHR)    != nullptr);
    EXPECT(ZHLN_VkResultString(VK_ERROR_DEVICE_LOST)        != nullptr);
    EXPECT(ZHLN_VkResultString(VK_ERROR_UNKNOWN)            != nullptr);
    // Unrecognized code should not return nullptr
    EXPECT(ZHLN_VkResultString((VkResult)-9999)             != nullptr);
}