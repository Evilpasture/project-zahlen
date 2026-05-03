// src/render/tests/test_utils.cpp
// Pure logic — no Vulkan device needed

#include "Utils.h"
#include "RenderCore.h"
#include <cstdio>
#include <cstdint>

extern int s_passed, s_failed;
#define EXPECT(cond) do { \
    if (!(cond)) { std::printf("  FAIL: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); ++s_failed; } \
    else { ++s_passed; } \
} while(0)

void test_utils() {
    std::printf("=== utils ===\n");

    // Clamp — float
    EXPECT(ZHLN_Clamp(0.5f, 0.0f, 1.0f) == 0.5f);
    EXPECT(ZHLN_Clamp(-1.0f, 0.0f, 1.0f) == 0.0f);
    EXPECT(ZHLN_Clamp(2.0f, 0.0f, 1.0f) == 1.0f);

    // Clamp — int32
    EXPECT(ZHLN_Clamp((int32_t)5, (int32_t)0, (int32_t)10) == 5);
    EXPECT(ZHLN_Clamp((int32_t)-1, (int32_t)0, (int32_t)10) == 0);
    EXPECT(ZHLN_Clamp((int32_t)11, (int32_t)0, (int32_t)10) == 10);

    // Clamp — double
    EXPECT(ZHLN_Clamp(0.5, 0.0, 1.0) == 0.5);
}

void test_errors() {
    std::printf("=== errors ===\n");

    EXPECT(ZHLN_VkResultString(VK_SUCCESS)                  != nullptr);
    EXPECT(ZHLN_VkResultString(VK_ERROR_OUT_OF_DATE_KHR)    != nullptr);
    EXPECT(ZHLN_VkResultString(VK_ERROR_DEVICE_LOST)        != nullptr);
    EXPECT(ZHLN_VkResultString(VK_ERROR_UNKNOWN)            != nullptr);
    // Unrecognized code should not return nullptr
    EXPECT(ZHLN_VkResultString((VkResult)-9999)             != nullptr);
}