// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


// src/render/tests/test_image.cpp
#include "Allocator.hpp"
#include "RenderCore.hpp"

#include <print>


extern int s_passed, s_failed;
#define EXPECT(cond)                                                                               \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			std::println("  FAIL: {}", #cond);                                                    \
			++s_failed;                                                                            \
		} else {                                                                                   \
			++s_passed;                                                                            \
		}                                                                                          \
	} while (0)

void test_image() {
	std::println("=== image transitions (Leak-Free) ===");

	// 1. Context manages Instance, PhysicalDevice, and Device handles.
	VkPhysicalDeviceVulkan13Features features13 = {};
	features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	features13.synchronization2 = VK_TRUE;

	VkPhysicalDeviceVulkan12Features features12 = {};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	features12.pNext = &features13;
	features12.bufferDeviceAddress = VK_TRUE;

	VkPhysicalDeviceFeatures2 features2 = {};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext = &features12;

	ZHLN_DeviceDesc dev_desc = {.physical = nullptr,
						 .extensions = nullptr,
						 .extension_count = 0,
						 .features = &features2,
						 .enable_validation = true};

	auto ctx = ZHLN::Vk::Context::Create(ZHLN_DEFAULT_INSTANCE_DESC, {}, dev_desc);
	if (!ctx)
		return;

	// 2. Allocator manages VmaAllocator handle.
	ZHLN::Vk::Allocator allocator;
	EXPECT(allocator.Init(ctx));

	// 3. CommandPool manages VkCommandPool handle.
	ZHLN::Vk::CommandPool pool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	EXPECT(pool.Allocate(1));
	VkCommandBuffer cmd = pool[0];

	// 4. Image manages VkImage + VmaAllocation handles.
	VkImageCreateInfo img_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
								  .pNext = nullptr,
								  .flags = 0,
								  .imageType = VK_IMAGE_TYPE_2D,
								  .format = VK_FORMAT_R8G8B8A8_UNORM,
								  .extent = {.width = 1, .height = 1, .depth = 1},
								  .mipLevels = 1,
								  .arrayLayers = 1,
								  .samples = VK_SAMPLE_COUNT_1_BIT,
								  .tiling = VK_IMAGE_TILING_OPTIMAL,
								  .usage =
									  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
								  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
								  .queueFamilyIndexCount = 0, // Added: Must come after sharingMode
								  .pQueueFamilyIndices = nullptr, // Added: Must come after count
								  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	auto image = ZHLN::Vk::Image::Create(allocator.Get(), img_info, VMA_MEMORY_USAGE_GPU_ONLY);

	// 5. Use the Helper
	ZHLN_BeginCommandBuffer(cmd);
	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED,
							   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, image.Handle());
	ZHLN_EndCommandBuffer(cmd);

	std::println("  Recorded transition for image handle: {}", (void*)image.Handle());
	EXPECT(image.Handle() != VK_NULL_HANDLE);

	// --- NO MANUAL DESTROY CALLS NEEDED ---
	// image destructs -> vmaDestroyImage
	// pool destructs  -> vkDestroyCommandPool
	// allocator destructs -> vmaDestroyAllocator
	// ctx destructs   -> vkDestroyDevice, vkDestroyInstance
}