// src/render/tests/test_raii.cpp
#include "RenderCore.hpp"
#include <print>

extern int s_passed, s_failed;
#define EXPECT(cond) do { if (!(cond)) { std::println("  FAIL: {}", #cond); ++s_failed; } else { ++s_passed; } } while(0)

void test_raii() {
    std::println("=== RAII lifecycle ===");

    ZHLN_InstanceDesc inst = ZHLN_DEFAULT_INSTANCE_DESC;
    inst.enable_validation = false;

    ZHLN_DeviceSelectDesc sel = { 
        .instance = VK_NULL_HANDLE, 
        .surface = VK_NULL_HANDLE,
        .score_fn = nullptr,
        .score_userdata = nullptr 
    };
    
    ZHLN_DeviceDesc dev = { 
        .physical = nullptr,
        .extensions = nullptr,
        .extension_count = 0,
        .features = nullptr,
        .enable_validation = false 
    };

    {
        // Test Context Move Semantics
        ZHLN::Vk::Context ctxA = ZHLN::Vk::Context::Create(inst, sel, dev);
        if (!ctxA.Valid()) return;

        VkDevice raw_handle = ctxA.Device();
        ZHLN::Vk::Context ctxB = std::move(ctxA);

        EXPECT(!ctxA.Valid());
        EXPECT(ctxB.Valid());
        EXPECT(ctxB.Device() == raw_handle);

        // Test Scoped Command Pool
        {
            auto pools = ZHLN::Vk::CommandPools<2>::Create(ctxB.Device(), ctxB.PhysicalInfo().graphics_family);
            EXPECT(pools.Valid());
            EXPECT(pools.Cmd(0) != VK_NULL_HANDLE);
            EXPECT(pools.Cmd(1) != VK_NULL_HANDLE);
        } // Pools destroyed here
    } // Context destroyed here (Instance & Device cleaned up)
    
    std::println("  RAII cleanup completed without crash");
}