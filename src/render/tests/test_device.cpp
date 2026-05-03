#include "HeadlessCtx.hpp"
#include <print>
#include <cstdlib>

extern int s_passed, s_failed;
#define EXPECT(cond) do { if (!(cond)) { std::println(stderr, "  FAIL: {}  ({}:{})", #cond, __FILE__, __LINE__); ++s_failed; } else { ++s_passed; } } while(0)

void test_device() {
    std::println("=== device ===");
    auto ctx = MakeHeadlessCtx();
    if (!ctx) { std::exit(77); }

    EXPECT(ctx.Instance() != VK_NULL_HANDLE);
    EXPECT(ctx.Physical() != VK_NULL_HANDLE);
    EXPECT(ctx.PhysicalInfo().has_graphics == true);
    EXPECT(ctx.Device() != VK_NULL_HANDLE);

    EXPECT(ctx.PhysicalInfo().properties.properties.deviceName[0] != '\0');
    std::println("  Device: {}", ctx.PhysicalInfo().properties.properties.deviceName);

    // NO vkDestroy... Context destructor handles it!
}