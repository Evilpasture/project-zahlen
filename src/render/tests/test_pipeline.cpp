// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


// src/render/tests/test_pipeline.cpp

#include "HeadlessCtx.hpp"
#include "RenderCore.h"
#include <print>
#include <cstdlib>

extern int s_passed, s_failed;
#define EXPECT(cond) do { \
    if (!(cond)) { std::println("  FAIL: {}  ({}:{})", #cond, __FILE__, __LINE__); ++s_failed; } \
    else { ++s_passed; } \
} while(0)

// Minimal valid SPIR-V — OpNop shader, accepted by the validator
// for module creation tests without needing shader files on disk
static const uint32_t k_NopSpirv[] = {
    0x07230203, // Magic
    0x00010300, // Version 1.3
    0x00000000, // Generator
    0x00000001, // Bound
    0x00000000, // Schema
    0x00020011, // OpCapability Shader
    0x00000001,
    0x0003000E, // OpMemoryModel Logical GLSL450
    0x00000000,
    0x00000001,
};

void test_pipeline() {
    std::printf("=== pipeline ===\n");

    auto ctx = MakeHeadlessCtx();
    if (!ctx.Valid()) {
        std::printf("  SKIP: no Vulkan device available\n");
        std::exit(77);
    }

    // Empty pipeline layout
    ZHLN_PipelineLayoutDesc layout_desc = {};
    VkPipelineLayout layout = ZHLN_CreatePipelineLayout(ctx.Device(), &layout_desc);
    EXPECT(layout != VK_NULL_HANDLE);

    // Shader module creation — invalid bytecode should fail cleanly
    ZHLN_ShaderDesc bad_desc = { .code = nullptr, .size = 0 };
    VkShaderModule bad = ZHLN_CreateShaderModule(ctx.Device(), &bad_desc);
    EXPECT(bad == VK_NULL_HANDLE);

    // Misaligned size should also fail
    static const uint32_t dummy[4] = {0x07230203, 0, 0, 0};
    ZHLN_ShaderDesc misaligned = { .code = dummy, .size = 7 }; // not % 4
    VkShaderModule mis = ZHLN_CreateShaderModule(ctx.Device(), &misaligned);
    EXPECT(mis == VK_NULL_HANDLE);

    ZHLN_DestroyPipelineLayout(ctx.Device(), layout);
}