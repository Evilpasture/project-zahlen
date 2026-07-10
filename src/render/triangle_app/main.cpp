// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "demo_utils/DemoWindow.hpp"

auto main() -> int {
	using namespace ZHLN;
	// 1. OS Window Creation
	auto win = Demo::InitWindow(1280, 720, "ZHLN - Triangle Demo");

	// 2. Vulkan Instance & Surface Setup
	auto inst_exts = Demo::GetRequiredInstanceExtensions();
#ifdef __APPLE__
	inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
	auto inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
	inst_desc.extensions = inst_exts.data();
	inst_desc.extension_count = static_cast<uint32_t>(inst_exts.size());

	auto features = Vk::FeatureChainBuilder(VK_NULL_HANDLE)
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

	auto ctx = Vk::Context::Create(inst_desc, {}, dev_desc);
	if (!ctx) {
		return -1;
	}
	Vk::Surface surface(ctx.Instance(), Demo::CreateSurface(ctx.Instance(), win));
	Vk::Allocator allocator;
	auto _ = allocator.Init(ctx);
	// 3. Double-Buffered Sync and Setup (Using triple-buffered pools)
	Vk::PresentationContext pres;
	auto _ = pres.Init(ctx, allocator, surface.Get(), win.width, win.height, true);

	auto sync = Vk::FrameSync<3>::Create(ctx.Device());
	auto pools = Vk::CommandPools<3>::Create(
		ctx.Device(), {.queue_family = ctx.PhysicalInfo().graphics_family, .buffers_per_pool = 1});

	// 4. Fluid Pipeline Generation
	auto shaders = Vk::ShaderStages::FromFiles(ctx.Device(), "triangle.hlsl.VSMain.spv",
											   "triangle.hlsl.PSMain.spv", "VSMain", "PSMain");
	if (!shaders.Valid()) {
		return -1;
	}

	auto layout_res = Vk::PipelineLayoutBuilder(ctx.Device()).Build();
	if (!layout_res) {
		return -1;
	}

	auto pipeline = Vk::PipelineBuilder{}
						.Shaders(shaders)
						.Layout(layout_res->Get()) // <-- Accessed safely via pointer semantics
						.ColorFormats({pres.swapchain.Get().format})
						.NoDepth()
						.CullNone()
						.Build(ctx.Device());
	if (!pipeline) {
		return -1;
	}
	// 5. Rebuild callback
	auto rebuild = [&]() {
		auto _ = pres.Rebuild(win.width, win.height);
		win.resized = false;
	};

	uint32_t frame_index = 0;
	win.resized = true;

	// 6. Concise Render Loop
	while (win.running) {
		Demo::ProcessEvents(win);
		if (win.width == 0 || win.height == 0) {
			continue;
		}
		if (win.resized) {
			rebuild();
		}
		if (!pres.swapchain.Valid() || pres.swapchain.Get().extent.width == 0) {
			continue;
		}

		// Group active Vulkan targets into a tidy descriptor structure
		Vk::DrawFrameDesc<3> frameDesc = {.ctx = ctx,
										  .swapchain = pres.swapchain,
										  .sync = sync,
										  .pools = pools,
										  .presentSemaphores = pres.presentSemaphores};

		// Execute frame boundary synchronization, recording, and presentation smoothly
		Vk::DrawFrame(
			frameDesc, frame_index,
			[&](VkCommandBuffer cmd, uint32_t image_index) {
				// Construct a strongly-typed swapchain layout tracker
				Vk::TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> swapImage = {
					.handle = pres.swapchain.Get().images[image_index],
					.view = pres.swapchain.Get().views[image_index],
					.extent = pres.swapchain.Get().extent,
					.aspect = VK_IMAGE_ASPECT_COLOR_BIT,
					.format = pres.swapchain.Get().format};

				// Chain transitions and render-pass execution seamlessly
				auto swap_att =
					Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, swapImage);

				Vk::DynamicPass(swap_att.extent)
					.AddColor(swap_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
							  {.r = 0.01f, .g = 0.01f, .b = 0.02f, .a = 1.0f})
					.Execute(cmd, [&]() {
						vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->Get());
						vkCmdDraw(cmd, 3, 1, 0, 0);
					});

				auto _ = Vk::Transition<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, swap_att);
			},
			rebuild);
	}

	vkDeviceWaitIdle(ctx.Device());
	Demo::DestroyWindow(win);
	return 0;
}
