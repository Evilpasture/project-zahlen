// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "Allocator.hpp"
#include "DescriptorLayout.hpp"
#include "PipelineBuilder.hpp"
#include "RenderCore.hpp"
#include "SamplerBuilder.hpp"
#include "TextureUtils.hpp"
#include "demo_utils/DemoWindow.hpp"
#include "math.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <vector>

// ----------------------------------------------------------------------------
// Vulkan App
// ----------------------------------------------------------------------------
static std::vector<uint32_t> LoadSpirv(const std::filesystem::path& path) {
	if (!std::filesystem::exists(path))
		return {};
	std::ifstream file(path, std::ios::ate | std::ios::binary);
	if (!file.is_open())
		return {};
	const size_t file_size = static_cast<size_t>(file.tellg());
	std::vector<uint32_t> buffer(file_size / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(file_size));
	return buffer;
}

// Define the Descriptor Layout using the TMP Builder
using CubeLayout = ZHLN::Vk::DescriptorLayout<ZHLN::Vk::SampledImageSlot<0>, // Texture
											  ZHLN::Vk::SamplerSlot<1>		 // Sampler
											  >;

int main() {
	// 1. OS Window Creation
	ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(800, 600, "ZHLN Engine - Cube");
	if (!win.os_window) {
		std::println(stderr, "Failed to create OS window. Exiting.");
		return -1;
	}

	// 2. Context Setup
	ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
	auto inst_exts = ZHLN::Demo::GetRequiredInstanceExtensions();
	inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	inst_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);

	// Safely query and enable Maintenance 1 only if supported
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

	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swap_maint = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
		.swapchainMaintenance1 = VK_TRUE};

	VkPhysicalDeviceVulkan13Features feat13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.pNext = has_maint1 ? &swap_maint : nullptr, // Link only if available
		.synchronization2 = VK_TRUE,
		.dynamicRendering = VK_TRUE};

	VkPhysicalDeviceVulkan12Features feat12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &feat13,
		.bufferDeviceAddress = VK_TRUE};

	VkPhysicalDeviceFeatures2 feat2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
									   .pNext = &feat12};

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
								.features = &feat2,
								.enable_validation = true};

	auto ctx = ZHLN::Vk::Context::Create(inst_desc, {VK_NULL_HANDLE, VK_NULL_HANDLE}, dev_desc);
	if (!ctx) {
		std::println(stderr, "FATAL: Failed to create Vulkan Context.");
		return -1;
	}

	// 3. Surface & Allocator
	VkSurfaceKHR raw_surface = ZHLN::Demo::CreateSurface(ctx.Instance(), win);
	ZHLN::Vk::Surface surface(ctx.Instance(), raw_surface);

	ZHLN::Vk::Allocator allocator;
	if (!allocator.Init(ctx))
		return -1;

	ZHLN::Vk::Swapchain swapchain(ctx.Device(), {});
	auto sync = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
	auto pools = ZHLN::Vk::CommandPools<3>::Create(
		ctx.Device(), {.queue_family = ctx.PhysicalInfo().graphics_family, .buffers_per_pool = 1});
	ZHLN::Vk::SemaphorePool present_semaphores;

	// =========================================================================
	// 4. Cube Texture Creation & Upload
	// =========================================================================
	const uint32_t TEX_W = 256;
	const uint32_t TEX_H = 256;
	static const auto cube_pixels = ZHLN::Texture::GenerateGrassTexture<TEX_W, TEX_H>();

	VkImageCreateInfo tex_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = {TEX_W, TEX_H, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	ZHLN::Vk::Image cube_texture_image =
		ZHLN::Vk::Image::Create(allocator.Get(), tex_info, VMA_MEMORY_USAGE_GPU_ONLY);
	if (!cube_texture_image.Valid())
		return -1;

	ZHLN::Vk::CommandPool setupPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	if (!setupPool.Allocate(1))
		return -1;

	VkCommandBuffer setupCmd = setupPool[0];

	ZHLN_BeginCommandBuffer(setupCmd);

	ZHLN::Vk::Buffer stagingBuffer =
		ZHLN::Vk::Buffer::Create(allocator.Get(), cube_pixels.size() * sizeof(uint32_t),
								 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(stagingBuffer.Map().data, cube_pixels.data(), cube_pixels.size() * sizeof(uint32_t));

	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
		setupCmd, cube_texture_image.Handle());

	ZHLN_BufferImageCopyDesc copyDesc = {.buffer = stagingBuffer.Handle(),
										 .image = cube_texture_image.Handle(),
										 .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
										 .width = TEX_W,
										 .height = TEX_H};
	ZHLN::Vk::CopyBufferToImage(setupCmd, copyDesc);

	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		setupCmd, cube_texture_image.Handle());
	ZHLN_EndCommandBuffer(setupCmd);

	VkCommandBufferSubmitInfo setupCmdInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											  .commandBuffer = setupCmd};
	VkSubmitInfo2 setupSubmit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								 .commandBufferInfoCount = 1,
								 .pCommandBufferInfos = &setupCmdInfo};
	vkQueueSubmit2(ctx.GraphicsQueue(), 1, &setupSubmit, VK_NULL_HANDLE);
	vkQueueWaitIdle(ctx.GraphicsQueue());

	// Image View & Sampler via RAII Helpers
	auto cube_texture_view =
		ZHLN::Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(ctx.Device(), cube_texture_image.Handle());
	auto cube_sampler = ZHLN::Vk::SamplerBuilder{}.Linear().Repeat().Build(ctx.Device());

	// =========================================================================
	// 5. Descriptor Sets for Cube Texture via TMP Backend
	// =========================================================================
	auto cube_desc_layout = CubeLayout::CreateLayout(ctx.Device());
	auto cube_desc_pool = CubeLayout::CreatePool(ctx.Device(), 1);

	VkDescriptorSet cube_descriptor_set =
		CubeLayout::Allocate(ctx.Device(), cube_desc_pool.Get(), cube_desc_layout.Get());
	CubeLayout::Write(ctx.Device(), cube_descriptor_set,
					  ZHLN::Vk::ImageWrite{cube_texture_view.Get()},
					  ZHLN::Vk::SamplerWrite{cube_sampler.Get()});

	// =========================================================================
	// 6. Pipeline Setup
	// =========================================================================
	auto shaders = ZHLN::Vk::ShaderStages::FromFiles(ctx.Device(), "cube.hlsl.VSMain.spv",
													 "cube.hlsl.PSMain.spv", "VSMain", "PSMain");
	if (!shaders.Valid()) {
		std::println(stderr, "FATAL: Failed to create cube shader stages.");
		return -1;
	}

	VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(Mat4)};

	VkDescriptorSetLayout rawLayout = cube_desc_layout.Get();
	ZHLN_PipelineLayoutDesc layout_desc = {.set_layouts = &rawLayout,
										   .set_layout_count = 1,
										   .push_constants = &push_range,
										   .push_constant_count = 1};
	ZHLN::Vk::PipelineLayout layout(ctx.Device(),
									ZHLN_CreatePipelineLayout(ctx.Device(), &layout_desc));

	auto pipeline = ZHLN::Vk::PipelineBuilder{}
						.Shaders(shaders)
						.Layout(layout.Get())
						.ColorFormats({VK_FORMAT_B8G8R8A8_SRGB})
						.DepthFormat(VK_FORMAT_D32_SFLOAT)
						.DepthTest(true)
						.DepthWrite(true)
						.CullBack()
						.Build(ctx.Device());

	if (!pipeline.Valid()) {
		std::println(stderr, "FATAL: Failed to create cube graphics pipeline.");
		return -1;
	}

	ZHLN::Vk::Image depth_image;
	ZHLN::Vk::ImageView depth_view;
	bool depth_initialized = false;

	auto rebuild = [&]() -> bool {
		vkDeviceWaitIdle(ctx.Device());
		depth_initialized = false;

		ZHLN_Device raw_dev = {ctx.Device(), ctx.GraphicsQueue(), ctx.PresentQueue()};
		ZHLN_PhysicalDeviceInfo raw_phys = ctx.PhysicalInfo();
		ZHLN_SwapchainDesc s_desc = {.device = &raw_dev,
									 .physical = &raw_phys,
									 .surface = surface.Get(),
									 .width = win.width,
									 .height = win.height,
									 .vsync = true,
									 .old_swapchain = swapchain.Get().handle};
		if (!swapchain.Rebuild(s_desc))
			return false;

		present_semaphores.Rebuild(ctx.Device(), swapchain.Get().image_count);

		VkImageCreateInfo img_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = VK_FORMAT_D32_SFLOAT,
			.extent = {win.width, win.height, 1},
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		depth_image = ZHLN::Vk::Image::Create(allocator.Get(), img_info, VMA_MEMORY_USAGE_GPU_ONLY);
		depth_view = ZHLN::Vk::CreateView<VK_FORMAT_D32_SFLOAT>(ctx.Device(), depth_image.Handle());
		return depth_view.Valid();
	};

	auto when = std::chrono::high_resolution_clock::now();
	win.resized = true;
	uint32_t frame_index = 0;

	// 7. Main Loop
	while (win.running) {
		ZHLN::Demo::ProcessEvents(win);

		if (win.width == 0 || win.height == 0)
			continue;
		if (win.resized) {
			if (!rebuild())
				break;
			win.resized = false;
		}

		if (!swapchain.Valid() || swapchain.Get().extent.width == 0)
			continue;

		const auto now = std::chrono::high_resolution_clock::now();
		const float elapsed = std::chrono::duration<float>(now - when).count();

		const Mat4 model = Multiply(RotateY(elapsed * 0.75f), RotateX(elapsed * 0.45f));
		const Mat4 view = LookAt({2.5f, 2.0f, 2.5f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
		const Mat4 proj = Perspective(
			1.0472f, static_cast<float>(win.width) / static_cast<float>(win.height), 0.1f, 10.0f);
		const Mat4 mvp = Multiply(proj, Multiply(view, model));

		auto record_cb = [&](VkCommandBuffer cmd, uint32_t image_index) {
			VkImage img = swapchain.Get().images[image_index];
			ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED,
									   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, img);

			if (depth_view.Valid() && !depth_initialized) {
				ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED,
										   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
					cmd, depth_image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
				depth_initialized = true;
			}

			ZHLN_RenderPassDesc pass = {
				.target_views = {swapchain.Get().views[image_index]},
				.depth_view = depth_view.Get(),
				.extent = swapchain.Get().extent,
				.clear_color = {0.05f, 0.05f, 0.08f, 1.0f},
				.clear_depth = 1.0f,
			};

			{
				ZHLN::Vk::ScopedRendering render(cmd, pass);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());

				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.Get(), 0, 1,
										&cube_descriptor_set, 0, nullptr);

				ZHLN::Vk::Push(cmd, layout.Get(), VK_SHADER_STAGE_VERTEX_BIT, mvp);
				vkCmdDraw(cmd, static_cast<uint32_t>(cube_indices.size()), 1, 0, 0);
			}

			ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
									   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, img);
		};

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
		record_cb(cmd, image_index);
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

	// All RAII handles (Device, Instance, Views, Images, Pipelines, Pools, Allocator)
	// will cleanly self-destruct here without leaking or requiring explicit vkDestroy calls.
	ZHLN::Demo::DestroyWindow(win);
	return 0;
}