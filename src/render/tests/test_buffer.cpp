#include "Allocator.hpp"
#include "HeadlessCtx.hpp"
#include <print>
#include <cstdlib>
#include <cstring>

extern int s_passed, s_failed;
#define EXPECT(cond) do { if (!(cond)) { std::println(stderr, "  FAIL: {}  ({}:{})", #cond, __FILE__, __LINE__); ++s_failed; } else { ++s_passed; } } while(0)

void test_buffer() {
    std::println("=== buffer ===");
    auto ctx = MakeHeadlessCtx();
    if (!ctx) std::exit(77);

    ZHLN::Vk::Allocator allocator;
    EXPECT(allocator.Init(ctx));

    auto buf = ZHLN::Vk::Buffer::Create(
        allocator.Get(), 256,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VMA_MEMORY_USAGE_CPU_TO_GPU);

    EXPECT(buf.Valid());
    
    auto mapped = buf.Map();
    EXPECT(mapped.data != nullptr);
    uint32_t pattern = 0xDEADBEEF;
    std::memcpy(mapped.data, &pattern, sizeof(pattern));

    // NO vkDestroy... RAII handles Buffer -> Allocator -> Context
}