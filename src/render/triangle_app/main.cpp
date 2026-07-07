// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "Features.hpp"
#include "PipelineBuilder.hpp"
#include "RenderCore.hpp"
#include "demo_utils/DemoWindow.hpp"

#include <vector>

auto main() -> int {
	// 1. OS Window Creation
	ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(800, 600, "ZHLN Engine - Modern Triangle");
	if (win.os_window == nullptr)
		return -1;

	// 2. Instance & Device Feature Setup
	ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
	auto inst_exts = ZHLN::Demo::GetRequiredInstanceExtensions();
#ifdef __APPLE__
	inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
	inst_desc.extensions = inst_exts.data();
	inst_desc.extension_count = static_cast<uint32_t>(inst_exts.size());

	auto features = ZHLN::Vk::FeatureChainBuilder()
						.Require<VkPhysicalDeviceVulkan13Features>([](auto& f) {
							f.synchronization2 = VK_TRUE;
							f.dynamicRendering = VK_TRUE;
						})
						.Build();

	std::vector<const char*> dev_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
#ifdef __APPLE__
	dev_exts.push_back("VK_KHR_portability_subset");
#endif

	ZHLN_DeviceDesc dev_desc = {.extensions = dev_exts.data(),
								.extension_count = static_cast<uint32_t>(dev_exts.size()),
								.features = features.GetRoot(),
								.enable_validation = true};

	auto ctx = ZHLN::Vk::Context::Create(inst_desc, {}, dev_desc);
	if (!ctx)
		return -1;

	// 3. Surface, Memory Allocator & Presentation Context
	ZHLN::Vk::Surface surface(ctx.Instance(), ZHLN::Demo::CreateSurface(ctx.Instance(), win));
	ZHLN::Vk::Allocator allocator;
	allocator.Init(ctx);

	ZHLN::Vk::PresentationContext pres;
	pres.Init(ctx, allocator, surface.Get(), win.width, win.height, true);

	auto sync = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
	auto pools = ZHLN::Vk::CommandPools<3>::Create(
		ctx.Device(), {.queue_family = ctx.PhysicalInfo().graphics_family, .buffers_per_pool = 1});

	// 4. Pipeline Setup
	auto shaders = ZHLN::Vk::ShaderStages::FromFiles(
		ctx.Device(), "triangle.hlsl.VSMain.spv", "triangle.hlsl.PSMain.spv", "VSMain", "PSMain");
	if (!shaders.Valid())
		return -1;

	// Refactored to use the framework's native layout builder
	ZHLN::Vk::PipelineLayout layout = ZHLN::Vk::PipelineLayoutBuilder(ctx.Device()).Build();

	auto pipelineRes = ZHLN::Vk::PipelineBuilder{}
						   .Shaders(shaders)
						   .Layout(layout.Get())
						   .ColorFormats({VK_FORMAT_B8G8R8A8_SRGB})
						   .NoDepth()
						   .CullNone()
						   .Build(ctx.Device());
	if (!pipelineRes)
		return -1;

	// 5. Minimal Rebuild Callback
	auto rebuild = [&]() {
		pres.Rebuild(win.width, win.height);
		win.resized = false;
	};

	// 6. Main Render Loop
	uint32_t frame_index = 0;
	win.resized = true;

	while (win.running) {
		ZHLN::Demo::ProcessEvents(win);
		if (win.width == 0 || win.height == 0)
			continue;
		if (win.resized)
			rebuild();
		if (!pres.swapchain.Valid() || pres.swapchain.Get().extent.width == 0)
			continue;

		ZHLN::Vk::DrawFrame(
			ctx, pres.swapchain, sync, pools, frame_index, pres.presentSemaphores,
			[&](VkCommandBuffer cmd, uint32_t image_index) {
				auto swap_att = ZHLN::Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
													 VK_IMAGE_LAYOUT_UNDEFINED>(
					cmd, {.handle = pres.swapchain.Get().images[image_index],
						  .view = pres.swapchain.Get().views[image_index],
						  .extent = pres.swapchain.Get().extent,
						  .aspect = VK_IMAGE_ASPECT_COLOR_BIT});

				ZHLN::Vk::DynamicPass(pres.swapchain.Get().extent)
					.AddColor(swap_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
							  ZHLN::Color4{.r = 0.01f, .g = 0.01f, .b = 0.02f, .a = 1.0f})
					.Execute(cmd, [&]() {
						vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineRes->Get());
						vkCmdDraw(cmd, 3, 1, 0, 0);
					});

				ZHLN::Vk::Transition<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, swap_att);
			},
			rebuild);
	}

	vkDeviceWaitIdle(ctx.Device());
	ZHLN::Demo::DestroyWindow(win);
	return 0;
}
