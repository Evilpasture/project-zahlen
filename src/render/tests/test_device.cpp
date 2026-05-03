// src/render/tests/test_device.cpp

#include "HeadlessCtx.hpp"
#include "RenderCore.h"
#include <cstdio>
#include <cstdlib>

extern int s_passed, s_failed;
#define EXPECT(cond) do { \
    if (!(cond)) { std::printf("  FAIL: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); ++s_failed; } \
    else { ++s_passed; } \
} while(0)

void test_device() {
    std::printf("=== device ===\n");

    auto ctx = MakeHeadlessCtx();
    if (!ctx.valid()) {
        std::printf("  SKIP: no Vulkan device available\n");
        std::exit(77);
    }

    EXPECT(ctx.instance                    != VK_NULL_HANDLE);
    EXPECT(ctx.physical.handle             != VK_NULL_HANDLE);
    EXPECT(ctx.physical.has_graphics       == true);
    EXPECT(ctx.physical.graphics_family    != UINT32_MAX);
    EXPECT(ctx.device.handle               != VK_NULL_HANDLE);
    EXPECT(ctx.device.graphics_queue       != VK_NULL_HANDLE);

    // Properties should be populated
    EXPECT(ctx.physical.properties.properties.deviceName[0] != '\0');

    std::printf("  Device: %s\n",
                ctx.physical.properties.properties.deviceName);

    vkDestroyDevice(ctx.device.handle, nullptr);
    vkDestroyInstance(ctx.instance, nullptr);
}