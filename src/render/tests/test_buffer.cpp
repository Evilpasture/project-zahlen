// src/render/tests/test_buffer.cpp

#include "Allocator.hpp"
#include "HeadlessCtx.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern int s_passed, s_failed;
#define EXPECT(cond) do { \
    if (!(cond)) { std::printf("  FAIL: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); ++s_failed; } \
    else { ++s_passed; } \
} while(0)

void test_buffer() {
    std::printf("=== buffer ===\n");

    auto ctx = MakeHeadlessCtx();
    if (!ctx.valid()) {
        std::printf("  SKIP: no Vulkan device available\n");
        std::exit(77);
    }

    // Start a nested scope to control lifetime
    {
        ZHLN::Vk::Allocator allocator;
        EXPECT(allocator.Init(ctx.instance, ctx.physical.handle, ctx.device.handle));

        // Create a host-visible buffer
        auto buf = ZHLN::Vk::Buffer::Create(
            allocator.Get(), 256,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VMA_MEMORY_USAGE_CPU_TO_GPU);

        EXPECT(buf.Valid());
        
        {
            auto mapped = buf.Map();
            EXPECT(mapped.data != nullptr);
            uint32_t pattern = 0xDEADBEEF;
            std::memcpy(mapped.data, &pattern, sizeof(pattern));
        } 
        
    } // <--- Buffer and Allocator destructors run HERE, while Device is still alive.

    // Now it is safe to kill the context
    vkDestroyDevice(ctx.device.handle, nullptr);
    vkDestroyInstance(ctx.instance, nullptr);
}