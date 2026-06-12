// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "Features.hpp"
#include "PipelineBuilder.hpp"
#include "RenderCore.hpp"
#include "demo_utils/DemoWindow.hpp"

#include <filesystem>
#include <vector>

auto main() -> int {
	// 1. OS Window Creation
	ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(800, 600, "ZHLN Engine - Modern Triangle");
	if (!win.os_window)
		return -1;

	// 2. Instance & Device Feature Setup (Modernized via FeatureFactory)
	ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
	auto inst_exts = ZHLN::Demo::GetRequiredInstanceExtensions();
	inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	inst_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

	bool has_maint1 =
		ZHLN::Vk::IsInstanceExtensionSupported(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
	if (has_maint1) {
		inst_exts.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
	}

#ifdef __APPLE__
	inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
	inst_desc.extensions = inst_exts.data();
	inst_desc.extension_count = static_cast<uint32_t>(inst_exts.size());

	// Compile-time feature chain building
	auto features =
		ZHLN::Vk::FeatureChainBuilder()
			.Require<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>([has_maint1](auto& f) {
				f.swapchainMaintenance1 = has_maint1 ? VK_TRUE : VK_FALSE;
			})
			.Require<VkPhysicalDeviceVulkan13Features>([](auto& f) {
				f.synchronization2 = VK_TRUE;
				f.dynamicRendering = VK_TRUE;
			})
			.Require<VkPhysicalDeviceVulkan12Features>(
				[](auto& f) { f.bufferDeviceAddress = VK_TRUE; })
			.Require<VkPhysicalDeviceFeatures2>([](auto& _) {})
			.Build();

	std::vector<const char*> dev_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	if (has_maint1) {
		dev_exts.push_back(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
		dev_exts.push_back(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
	}
#ifdef __APPLE__
	dev_exts.push_back("VK_KHR_portability_subset");
#endif

	ZHLN_DeviceDesc dev_desc = {.extensions = dev_exts.data(),
								.extension_count = static_cast<uint32_t>(dev_exts.size()),
								.features = features.GetRoot(),
								.enable_validation = true};

	auto ctx = ZHLN::Vk::Context::Create(inst_desc, {VK_NULL_HANDLE, VK_NULL_HANDLE}, dev_desc);
	if (!ctx)
		return -1;

	// 3. Surface & RAII Resources
	VkSurfaceKHR raw_surface = ZHLN::Demo::CreateSurface(ctx.Instance(), win);
	ZHLN::Vk::Surface surface(ctx.Instance(), raw_surface);

	ZHLN::Vk::Swapchain swapchain(ctx.Device(), {});
	auto sync = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
	auto pools = ZHLN::Vk::CommandPools<3>::Create(
		ctx.Device(), {.queue_family = ctx.PhysicalInfo().graphics_family, .buffers_per_pool = 1});
	ZHLN::Vk::SemaphorePool present_semaphores;

	// 4. Pipeline Setup
	auto shaders = ZHLN::Vk::ShaderStages::FromFiles(
		ctx.Device(), "triangle.hlsl.VSMain.spv", "triangle.hlsl.PSMain.spv", "VSMain", "PSMain");

	if (!shaders.Valid())
		return -1;

	ZHLN_PipelineLayoutDesc layout_desc = {};
	ZHLN::Vk::PipelineLayout layout(ctx.Device(),
									ZHLN_CreatePipelineLayout(ctx.Device(), &layout_desc));

	auto pipeline = ZHLN::Vk::PipelineBuilder{}
						.Shaders(shaders)
						.Layout(layout.Get())
						.ColorFormats({VK_FORMAT_B8G8R8A8_SRGB})
						.NoDepth()
						.CullNone()
						.Build(ctx.Device());

	if (!pipeline.Valid())
		return -1;

	// 5. Rebuild Helper
	auto rebuild = [&]() {
		vkDeviceWaitIdle(ctx.Device());
		ZHLN_Device raw_dev = {ctx.Device(), ctx.GraphicsQueue(), ctx.PresentQueue()};
		ZHLN_PhysicalDeviceInfo raw_phys = ctx.PhysicalInfo();
		ZHLN_SwapchainDesc s_desc = {.device = &raw_dev,
									 .physical = &raw_phys,
									 .surface = surface.Get(),
									 .width = win.width,
									 .height = win.height,
									 .vsync = true,
									 .old_swapchain = swapchain.Get().handle};
		swapchain.Rebuild(s_desc);
		present_semaphores.Rebuild(ctx.Device(), swapchain.Get().image_count);
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
		if (!swapchain.Valid() || swapchain.Get().extent.width == 0)
			continue;

		const ZHLN_FrameSync& frame_sync = sync[frame_index];
		ZHLN_CommandPool& pool = pools[frame_index];
		VkCommandBuffer cmd = pools.Cmd(frame_index);

		ZHLN_WaitAndResetFrame(ctx.Device(), frame_sync.in_flight, &pool);

		uint32_t image_index = 0;
		ZHLN_AcquireDesc acq = {.swapchain = swapchain.Get().handle,
								.image_available = frame_sync.image_available,
								.timeout_ns = UINT64_MAX};
		if (ZHLN_AcquireImage(ctx.Device(), &acq, &image_index) == ZHLN_FrameResult_OutOfDate) {
			win.resized = true;
			continue;
		}

		ZHLN_BeginCommandBuffer(cmd);

		// --- MODERN TYPE-SAFE IMAGE LAYOUT STATE ---
		ZHLN::Vk::TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> swap_u = {
			.handle = swapchain.Get().images[image_index],
			.view = swapchain.Get().views[image_index],
			.extent = swapchain.Get().extent,
			.aspect = VK_IMAGE_ASPECT_COLOR_BIT};

		// Transition the image's layout; statically tracked by the compiler
		auto swap_att = ZHLN::Vk::Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, swap_u);

		// --- MODERN FLUENT RENDER PASS EXECUTION ---
		ZHLN::Vk::DynamicPass(
			swapchain.Get().extent) // Compile-time deduced starting state via CTAD [1]
			.AddColor(swap_att, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
					  {0.01f, 0.01f, 0.02f, 1.0f})
			.Execute(cmd, [&]() {
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
				vkCmdDraw(cmd, 3, 1, 0, 0);
			});

		// Transition image state back to present source
		auto swap_pres = ZHLN::Vk::Transition<VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, swap_att);

		ZHLN_EndCommandBuffer(cmd);

		ZHLN_FrameSubmitDesc submitDesc = {.graphicsQueue = ctx.GraphicsQueue(),
										   .presentQueue = ctx.PresentQueue(),
										   .cmd = cmd,
										   .imageAvailable = frame_sync.image_available,
										   .renderFinished = present_semaphores[image_index],
										   .inFlight = frame_sync.in_flight,
										   .swapchain = swapchain.Get().handle,
										   .imageIndex = image_index};

		if (ZHLN::Vk::SubmitAndPresent(submitDesc) != ZHLN_FrameResult_Ok) {
			win.resized = true;
		}
		frame_index = (frame_index + 1) % 3;
	}

	vkDeviceWaitIdle(ctx.Device());
	ZHLN::Demo::DestroyWindow(win);
	return 0;
}
