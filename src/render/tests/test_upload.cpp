// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "Allocator.hpp"
#include "HeadlessCtx.hpp"
#include <print>

extern int s_passed, s_failed;
#define EXPECT(cond) do { if (!(cond)) { std::println(stderr, "  FAIL: {}", #cond); ++s_failed; } else { ++s_passed; } } while(0)

void test_upload() {
    std::println("=== upload ===");
    auto ctx = MakeHeadlessCtx();
    if (!ctx) std::exit(77);

    ZHLN::Vk::Allocator allocator;
    EXPECT(allocator.Init(ctx));

    auto gpu_buf = ZHLN::Vk::Buffer::Create(allocator.Get(), 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    ZHLN::Vk::CommandPool pool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
    EXPECT(pool.Allocate(1));
    VkCommandBuffer cmd = pool[0];

    uint32_t data = 0xABCDEFF0;
    ZHLN_BeginCommandBuffer(cmd);
    auto staging = ZHLN::Vk::UploadToBuffer(allocator.Get(), cmd, gpu_buf, &data, sizeof(data));
    ZHLN_EndCommandBuffer(cmd);

    EXPECT(staging.Valid());
    
    // NO manual destroys needed! 
}