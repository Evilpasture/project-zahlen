// src/render/tests/main.cpp

#include "HeadlessCtx.hpp"
#include "RenderCore.h"
#include "RenderCore.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ----------------------------------------------------------------------------
// Minimal test framework — no dependencies
// ----------------------------------------------------------------------------

int s_passed = 0;
int s_failed = 0;

#define EXPECT(cond) do {                                           \
    if (!(cond)) {                                                  \
        std::printf("  FAIL: %s  (%s:%d)\n", #cond,               \
                    __FILE__, __LINE__);                            \
        ++s_failed;                                                 \
    } else {                                                        \
        ++s_passed;                                                 \
    }                                                               \
} while(0)

#define EXPECT_EQ(a, b) EXPECT((a) == (b))
#define EXPECT_NE(a, b) EXPECT((a) != (b))
#define EXPECT_TRUE(a)  EXPECT((a))
#define EXPECT_FALSE(a) EXPECT(!(a))

static void PrintSummary() {
    std::printf("\n%d passed, %d failed\n", s_passed, s_failed);
}

// ----------------------------------------------------------------------------
// Forward declarations — one per test file
// ----------------------------------------------------------------------------

void test_utils();
void test_errors();
void test_device();
void test_buffer();
void test_sync();
void test_pipeline();
void test_swapchain_support();

// ----------------------------------------------------------------------------
// Headless Vulkan fixture
// Creates an instance + device with no surface for GPU tests.
// Returns false if no suitable device exists — callers return 77 to skip.
// ----------------------------------------------------------------------------

HeadlessCtx MakeHeadlessCtx() {
    HeadlessCtx ctx;

    ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
    inst_desc.enable_validation = false; // no messenger needed in tests
    ctx.instance = ZHLN_CreateInstance(&inst_desc);
    if (ctx.instance == VK_NULL_HANDLE) return {};

    ZHLN_DeviceSelectDesc sel = {
        .instance = ctx.instance,
        .surface  = VK_NULL_HANDLE,   // headless
    };
    ctx.physical = ZHLN_SelectPhysicalDevice(&sel);
    if (ctx.physical.handle == VK_NULL_HANDLE) return {};

    ZHLN_DeviceDesc dev_desc = {
        .physical         = &ctx.physical,
        .extensions       = nullptr,
        .extension_count  = 0,
        .features         = nullptr,
        .enable_validation = false,
    };
    ctx.device = ZHLN_CreateDevice(&dev_desc);
    return ctx;
}

// ----------------------------------------------------------------------------
// Entry point — dispatches by suite name
// ----------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::printf("Usage: zahlen_render_tests <suite>\n");
        std::printf("Suites: utils errors device buffer sync pipeline swapchain all\n");
        return 1;
    }

    const char* suite = argv[1];

    if (std::strcmp(suite, "utils")    == 0) { test_utils();    }
    else if (std::strcmp(suite, "errors")   == 0) { test_errors();   }
    else if (std::strcmp(suite, "device")   == 0) { test_device();   }
    else if (std::strcmp(suite, "buffer")   == 0) { test_buffer();   }
    else if (std::strcmp(suite, "sync")     == 0) { test_sync();     }
    else if (std::strcmp(suite, "pipeline") == 0) { test_pipeline(); }
    else if (std::strcmp(suite, "swapchain") == 0) { test_swapchain_support(); }
    else if (std::strcmp(suite, "all") == 0) {
        const char* suites[] = {"utils", "errors", "device", "buffer", "sync", "pipeline", "swapchain"};
        int final_code = 0;
        for (const char* s : suites) {
            std::printf("\n=== running suite: %s ===\n", s);
            std::string command = std::string("\"") + argv[0] + "\" " + s;
            int code = std::system(command.c_str());
            if (code == 77)
                std::printf("  SKIP: %s\n", s);
            else if (code != 0) {
                final_code = code;
                std::printf("  FAIL: suite %s returned %d\n", s, code);
            }
        }
        return final_code;
    }
    else {
        std::printf("Unknown suite: %s\n", suite);
        return 1;
    }

    PrintSummary();
    return (s_failed > 0) ? 1 : 0;
}