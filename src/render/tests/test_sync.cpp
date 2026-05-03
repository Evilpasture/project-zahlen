// src/render/tests/test_sync.cpp

#include "HeadlessCtx.hpp"
#include "RenderCore.h"
#include <print>
#include <cstdlib>

extern int s_passed, s_failed;
#define EXPECT(cond) do { \
    if (!(cond)) { std::println("  FAIL: {}  ({}:{})", #cond, __FILE__, __LINE__); ++s_failed; } \
    else { ++s_passed; } \
} while(0)

void test_sync() {
    std::println("=== sync ===");

    auto ctx = MakeHeadlessCtx();
    if (!ctx.Valid()) {
        std::printf("  SKIP: no Vulkan device available\n");
        std::exit(77);
    }

    // Create 2 frames of sync
    ZHLN_FrameSync frames[2] = {};
    ZHLN_FrameSyncDesc desc  = { .device = ctx.Device(), .frame_count = 2 };
    EXPECT(ZHLN_CreateFrameSync(&desc, frames));

    EXPECT(frames[0].image_available != VK_NULL_HANDLE);
    EXPECT(frames[0].render_finished != VK_NULL_HANDLE);
    EXPECT(frames[0].in_flight       != VK_NULL_HANDLE);
    EXPECT(frames[1].image_available != VK_NULL_HANDLE);

    // Fence starts signaled — wait should return immediately
    VkResult r = vkWaitForFences(ctx.Device(), 1,
                                 &frames[0].in_flight, VK_TRUE, 0);
    EXPECT(r == VK_SUCCESS);

    ZHLN_DestroyFrameSync(ctx.Device(), frames, 2);

    // After destroy, all handles should be null
    EXPECT(frames[0].image_available == VK_NULL_HANDLE);
    EXPECT(frames[0].in_flight       == VK_NULL_HANDLE);
}