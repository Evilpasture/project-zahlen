// src/render/tests/main.cpp

#include "HeadlessCtx.hpp"
#include "RenderCore.h"

#include <array>
#include <cstdlib>
#include <print>
#include <string>
#include <string_view>

// ----------------------------------------------------------------------------
// Minimal test framework — no dependencies
// ----------------------------------------------------------------------------

int s_passed = 0;
int s_failed = 0;

#define EXPECT(cond)                                                                               \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::println(stderr, "  FAIL: {}  ({}:{})", #cond, __FILE__, __LINE__);                \
			++s_failed;                                                                            \
		} else {                                                                                   \
			++s_passed;                                                                            \
		}                                                                                          \
	} while (0)

static void PrintSummary() {
	std::println("\n{} passed, {} failed", s_passed, s_failed);
}

// ----------------------------------------------------------------------------
// Forward declarations
// ----------------------------------------------------------------------------

void test_utils();
void test_errors();
void test_device();
void test_buffer();
void test_sync();
void test_pipeline();
void test_swapchain_support();
void test_upload();
void test_raii();
void test_image();

// ----------------------------------------------------------------------------
// Headless Vulkan fixture
// ----------------------------------------------------------------------------

ZHLN::Vk::Context MakeHeadlessCtx() {
	ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
	inst_desc.enable_validation = true;

	ZHLN_DeviceSelectDesc sel = {
        .instance = VK_NULL_HANDLE, // Context::Create fills this in automatically
        .surface = VK_NULL_HANDLE,
        .score_fn = nullptr,
        .score_userdata = nullptr
    };

	VkPhysicalDeviceVulkan13Features features13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.pNext = nullptr,
		.synchronization2 = VK_TRUE,
	};

	VkPhysicalDeviceVulkan12Features features12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &features13,
		.bufferDeviceAddress = VK_TRUE,
	};

	VkPhysicalDeviceFeatures2 features2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &features12,
		.features = {}, 
	};

	ZHLN_DeviceDesc dev_desc = {
        .physical = nullptr, // Context::Create fills this in automatically
        .extensions = nullptr,
        .extension_count = 0,
        .features = &features2,
        .enable_validation = true
    };

	return ZHLN::Vk::Context::Create(inst_desc, sel, dev_desc);
}

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------

int main(int argc, char* argv[]) {
	if (argc < 2) {
		std::println("Usage: zahlen_render_tests <suite>");
		std::println("Suites: utils errors device buffer sync pipeline swapchain all");
		return 1;
	}

	const std::string_view suite{argv[1]};

	if (suite == "utils") {
		test_utils();
	} else if (suite == "errors") {
		test_errors();
	} else if (suite == "device") {
		test_device();
	} else if (suite == "buffer") {
		test_buffer();
	} else if (suite == "sync") {
		test_sync();
	} else if (suite == "pipeline") {
		test_pipeline();
	} else if (suite == "swapchain") {
		test_swapchain_support();
	} else if (suite == "upload") {
		test_upload();
	} else if (suite == "raii") {
		test_raii();
	} else if (suite == "image") {
		test_image();
	} else if (suite == "all") {
		constexpr auto suites =
			std::to_array<std::string_view>({"utils", "errors", "device", "buffer", "sync",
											 "pipeline", "swapchain", "upload", "raii", "image"});

		int final_code = 0;
		for (auto s : suites) {
			std::println("\n=== running suite: {} ===", s);

			// Modern string formatting
			std::string command = std::format("\"{}\" {}", argv[0], s);

			int code = std::system(command.c_str());

			if (code == 77) {
				std::println("  SKIP: {}", s);
			} else if (code != 0) {
				final_code = code;
				std::println("  FAIL: suite {} returned {}", s, code);
			}
		}
		return final_code;
	} else {
		std::println("Unknown suite: {}", suite);
		return 1;
	}

	PrintSummary();
	return (s_failed > 0) ? 1 : 0;
}