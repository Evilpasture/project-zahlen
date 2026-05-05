#include "Allocator.hpp"
#include "RenderCore.hpp"
#include "TextureUtils.hpp" // <--- NEW: Assuming this contains GenerateTVInterruptTexture
#include "demo_utils/DemoWindow.hpp"
#include "math.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print> // For error messages
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

static VkImageView CreateDepthImageView(VkDevice device, VkImage image, VkFormat format) noexcept {
	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
					   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
		.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
							 .baseMipLevel = 0,
							 .levelCount = 1,
							 .baseArrayLayer = 0,
							 .layerCount = 1},
	};
	VkImageView view = VK_NULL_HANDLE;
	vkCreateImageView(device, &view_info, nullptr, &view);
	return view;
}

int main() {
	// 1. OS Window Creation
	ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(800, 600, "ZHLN Engine - Cube");

	// 2. Context Setup
	ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
	auto required_exts = ZHLN::Demo::GetRequiredInstanceExtensions();
	required_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	required_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
	required_exts.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

#ifdef __APPLE__
	required_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif
	inst_desc.extensions = required_exts.data();
	inst_desc.extension_count = static_cast<uint32_t>(required_exts.size());

	VkPhysicalDeviceVulkan13Features feat13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.pNext = nullptr,
		.synchronization2 = VK_TRUE,
		.dynamicRendering = VK_TRUE};
	VkPhysicalDeviceVulkan12Features feat12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &feat13,
		.bufferDeviceAddress = VK_TRUE};
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swap_maint = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
		.pNext = nullptr,
		.swapchainMaintenance1 = VK_TRUE};
	feat13.pNext = &swap_maint;
	VkPhysicalDeviceFeatures2 feat2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
									   .pNext = &feat12};

#ifdef __APPLE__
	const char* dev_exts[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME,
		"VK_KHR_portability_subset" // <-- MANDATORY FOR MAC
	};
	const uint32_t dev_ext_count = 4;
#else
	const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME};
	const uint32_t dev_ext_count = 3;
#endif

	ZHLN_DeviceDesc dev_desc = {.physical = nullptr,
								.extensions = dev_exts,
								.extension_count = dev_ext_count, // Use the count variable
								.features = &feat2,
								.enable_validation = true};

	auto ctx = ZHLN::Vk::Context::Create(
		inst_desc, {VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, nullptr}, dev_desc);
	if (!ctx)
		return -1;

	// 3. Surface & Allocator
	VkSurfaceKHR raw_surface = ZHLN::Demo::CreateSurface(ctx.Instance(), win);
	ZHLN::Vk::Surface surface(ctx.Instance(), raw_surface);

	ZHLN::Vk::Allocator allocator;
	if (!allocator.Init(ctx))
		return -1;

	ZHLN::Vk::Swapchain swapchain(ctx.Device(), {});
	auto sync = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
	auto pools =
		ZHLN::Vk::CommandPools<3>::Create(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	ZHLN::Vk::SemaphorePool present_semaphores;

	// =========================================================================
	// NEW: 4. Cube Texture Creation & Upload (using TextureUtils.hpp)
	// =========================================================================
	const uint32_t TEX_W = 256; // Smaller texture for simple cube
	const uint32_t TEX_H = 256;
	static const auto cube_pixels =
		ZHLN::Texture::GenerateMarbleCrisp<TEX_W, TEX_H>(); // Or GenerateTestTexture

	VkImageCreateInfo tex_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM, // Match texture format
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
	if (!cube_texture_image.Valid()) {
		std::println(stderr, "FATAL: Failed to create cube texture image.");
		return -1;
	}

	ZHLN::Vk::CommandPool setupPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	if (!setupPool.Allocate(1))
		return -1; // Added missing error check
	VkCommandBuffer setupCmd = setupPool[0];

	ZHLN_BeginCommandBuffer(setupCmd);
	ZHLN::Vk::Buffer stagingBuffer =
		ZHLN::Vk::Buffer::Create(allocator.Get(), cube_pixels.size() * sizeof(uint32_t),
								 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	if (!stagingBuffer.Valid()) {
		std::println(stderr, "FATAL: Failed to create staging buffer for cube texture.");
		return -1;
	}
	memcpy(stagingBuffer.Map().data, cube_pixels.data(), cube_pixels.size() * sizeof(uint32_t));

	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
		setupCmd, cube_texture_image.Handle());

	ZHLN_BufferImageCopyDesc copyDesc = {.buffer = stagingBuffer.Handle(),
										 .image = cube_texture_image.Handle(),
										 .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
										 .width = TEX_W,
										 .height = TEX_H,
										 .buffer_offset = 0,
										 .mip_level = 0,
										 .base_array_layer = 0};
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
	vkQueueWaitIdle(ctx.GraphicsQueue()); // Wait for upload

	// Image View & Sampler for the Cube Texture
	VkImageViewCreateInfo cube_view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = cube_texture_image.Handle(),
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}};
	VkImageView cube_texture_view;
	if (vkCreateImageView(ctx.Device(), &cube_view_info, nullptr, &cube_texture_view) !=
		VK_SUCCESS) {
		std::println(stderr, "FATAL: Failed to create cube texture image view.");
		return -1;
	}

	VkSamplerCreateInfo cube_sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
											 .magFilter = VK_FILTER_LINEAR,
											 .minFilter = VK_FILTER_LINEAR,
											 .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
											 .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
											 .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT};
	VkSampler cube_sampler;
	if (vkCreateSampler(ctx.Device(), &cube_sampler_info, nullptr, &cube_sampler) != VK_SUCCESS) {
		std::println(stderr, "FATAL: Failed to create cube texture sampler.");
		return -1;
	}

	// =========================================================================
	// NEW: 5. Descriptor Sets for Cube Texture
	// =========================================================================
	// Bindings match HLSL: [[vk::binding(0, 0)]] Texture2D cubeTexture; [[vk::binding(1, 0)]]
	// SamplerState cubeSampler;
	VkDescriptorSetLayoutBinding cube_bindings[2] = {
		{.binding = 0,
		 .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
		 .descriptorCount = 1,
		 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
		{.binding = 1,
		 .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
		 .descriptorCount = 1,
		 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
	VkDescriptorSetLayoutCreateInfo cube_layout_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = cube_bindings};
	VkDescriptorSetLayout cube_desc_layout;
	if (vkCreateDescriptorSetLayout(ctx.Device(), &cube_layout_info, nullptr, &cube_desc_layout) !=
		VK_SUCCESS) {
		std::println(stderr, "FATAL: Failed to create cube descriptor set layout.");
		return -1;
	}

	// One descriptor set for the cube
	VkDescriptorPoolSize cube_pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
											   {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
	VkDescriptorPoolCreateInfo cube_pool_info = {.sType =
													 VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
												 .maxSets = 1,
												 .poolSizeCount = 2,
												 .pPoolSizes = cube_pool_sizes};
	VkDescriptorPool cube_desc_pool;
	if (vkCreateDescriptorPool(ctx.Device(), &cube_pool_info, nullptr, &cube_desc_pool) !=
		VK_SUCCESS) {
		std::println(stderr, "FATAL: Failed to create cube descriptor pool.");
		return -1;
	}

	VkDescriptorSetAllocateInfo cube_alloc_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = cube_desc_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &cube_desc_layout};
	VkDescriptorSet cube_descriptor_set;
	if (vkAllocateDescriptorSets(ctx.Device(), &cube_alloc_info, &cube_descriptor_set) !=
		VK_SUCCESS) {
		std::println(stderr, "FATAL: Failed to allocate cube descriptor set.");
		return -1;
	}

	VkDescriptorImageInfo cube_image_info = {
		.imageView = cube_texture_view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkDescriptorImageInfo cube_sampler_desc_info = {.sampler = cube_sampler};

	VkWriteDescriptorSet cube_writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
											.dstSet = cube_descriptor_set,
											.dstBinding = 0,
											.descriptorCount = 1,
											.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
											.pImageInfo = &cube_image_info},
										   {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
											.dstSet = cube_descriptor_set,
											.dstBinding = 1,
											.descriptorCount = 1,
											.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
											.pImageInfo = &cube_sampler_desc_info}};
	vkUpdateDescriptorSets(ctx.Device(), 2, cube_writes, 0, nullptr);

	// =========================================================================
	// OLD: 6. Pipeline Setup (Now uses Descriptor Set Layout)
	// =========================================================================
	auto vert_code = LoadSpirv("cube.hlsl.VSMain.spv"); // <--- Corrected shader names
	auto frag_code = LoadSpirv("cube.hlsl.PSMain.spv"); // <--- Corrected shader names
	if (vert_code.empty() || frag_code.empty()) {
		std::println(stderr, "FATAL: Failed to load cube shader SPIR-V.");
		return -1;
	}

	// Pass the explicit HLSL entry points
	ZHLN_ShaderDesc v_desc = {vert_code.data(), vert_code.size() * sizeof(uint32_t), "VSMain"};
	ZHLN_ShaderDesc f_desc = {frag_code.data(), frag_code.size() * sizeof(uint32_t), "PSMain"};
	auto shaders = ZHLN::Vk::ShaderStages::Create(ctx.Device(), v_desc, f_desc);
	if (!shaders.Valid()) {
		std::println(stderr, "FATAL: Failed to create cube shader stages.");
		return -1;
	}

	VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(Mat4)};

	// Provide the cube_desc_layout here!
	ZHLN_PipelineLayoutDesc layout_desc = {
		.set_layouts = &cube_desc_layout, // <--- NEW: Provide descriptor set layout
		.set_layout_count = 1,			  // <--- NEW: One descriptor set
		.push_constants = &push_range,
		.push_constant_count = 1};
	ZHLN::Vk::PipelineLayout layout(ctx.Device(),
									ZHLN_CreatePipelineLayout(ctx.Device(), &layout_desc));
	if (!layout.Valid()) {
		std::println(stderr, "FATAL: Failed to create cube pipeline layout.");
		return -1;
	}

	ZHLN_GraphicsPipelineDesc pipe_desc = {.stages = const_cast<ZHLN_ShaderStages*>(shaders.Get()),
										   .layout = layout.Get(),
										   .vertex_bindings = nullptr,
										   .vertex_attributes = nullptr,
										   .vertex_binding_count = 0,
										   .vertex_attribute_count = 0,
										   .color_format = VK_FORMAT_B8G8R8A8_SRGB,
										   .depth_format = VK_FORMAT_D32_SFLOAT,
										   .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
										   .polygon_mode = VK_POLYGON_MODE_FILL,
										   .cull_mode = VK_CULL_MODE_BACK_BIT,
										   .front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE,
										   .depth_test = true,
										   .depth_write = true,
										   .blend_enable = false};

	ZHLN::Vk::Pipeline pipeline(ctx.Device(),
								ZHLN_CreateGraphicsPipeline(ctx.Device(), &pipe_desc));
	if (!pipeline.Valid()) {
		std::println(stderr, "FATAL: Failed to create cube graphics pipeline.");
		return -1;
	}

	ZHLN::Vk::Image depth_image;
	VkImageView depth_view = VK_NULL_HANDLE;
	bool depth_initialized = false;

	auto rebuild = [&] -> bool {
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
									 .old_swapchain = VK_NULL_HANDLE};
		if (!swapchain.Rebuild(s_desc))
			return false;

		present_semaphores.Rebuild(ctx.Device(), swapchain.Get().image_count);

		if (depth_view != VK_NULL_HANDLE) {
			vkDestroyImageView(ctx.Device(), depth_view, nullptr);
			depth_view = VK_NULL_HANDLE;
		}

		VkImageCreateInfo img_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = VK_FORMAT_D32_SFLOAT,
			.extent = {win.width, win.height, 1},
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.queueFamilyIndexCount = 0,
			.pQueueFamilyIndices = nullptr,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		depth_image = ZHLN::Vk::Image::Create(allocator.Get(), img_info, VMA_MEMORY_USAGE_GPU_ONLY);
		if (depth_image.Handle() == VK_NULL_HANDLE)
			return false;

		depth_view = CreateDepthImageView(ctx.Device(), depth_image.Handle(), VK_FORMAT_D32_SFLOAT);
		return depth_view != VK_NULL_HANDLE;
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

			if (depth_view != VK_NULL_HANDLE && !depth_initialized) {
				ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED,
										   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
					cmd, depth_image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
				depth_initialized = true;
			}

			ZHLN_RenderPassDesc pass = {
				.target_view = swapchain.Get().views[image_index],
				.depth_view = depth_view,
				.extent = swapchain.Get().extent,
				.clear_color = {0.05f, 0.05f, 0.08f, 1.0f},
				.clear_depth = 1.0f,
			};

			{
				ZHLN::Vk::ScopedRendering render(cmd, pass);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
				// Bind descriptor sets for the texture
				vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.Get(), 0, 1,
										&cube_descriptor_set, 0, nullptr); // <--- NEW
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

		VkCommandBufferSubmitInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											  .pNext = nullptr,
											  .commandBuffer = cmd,
											  .deviceMask = 0};
		VkSemaphoreSubmitInfo wait_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
										   .pNext = nullptr,
										   .semaphore = frame_sync.image_available,
										   .value = 0,
										   .stageMask =
											   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
										   .deviceIndex = 0};
		VkSemaphoreSubmitInfo signal_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
											 .pNext = nullptr,
											 .semaphore = present_semaphores[image_index],
											 .value = 0,
											 .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
											 .deviceIndex = 0};

		VkSubmitInfo2 submit = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.pNext = nullptr,
			.flags = 0,
			.waitSemaphoreInfoCount = 1,
			.pWaitSemaphoreInfos = &wait_info,
			.commandBufferInfoCount = 1,
			.pCommandBufferInfos = &cmd_info,
			.signalSemaphoreInfoCount = 1,
			.pSignalSemaphoreInfos = &signal_info,
		};
		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, frame_sync.in_flight);

		ZHLN_PresentDesc pres = {.present_queue = ctx.PresentQueue(),
								 .swapchain = swapchain.Get().handle,
								 .render_finished = present_semaphores[image_index],
								 .image_index = image_index};
		if (ZHLN_PresentFrame(&pres) != ZHLN_FrameResult_Ok)
			win.resized = true;

		frame_index = (frame_index + 1) % 3;
	}

	vkDeviceWaitIdle(ctx.Device());

	if (depth_view != VK_NULL_HANDLE)
		vkDestroyImageView(ctx.Device(), depth_view, nullptr);
	// NEW: Destroy cube texture resources
	vkDestroySampler(ctx.Device(), cube_sampler, nullptr);
	vkDestroyImageView(ctx.Device(), cube_texture_view, nullptr);
	vkDestroyDescriptorPool(ctx.Device(), cube_desc_pool, nullptr);
	vkDestroyDescriptorSetLayout(ctx.Device(), cube_desc_layout, nullptr);
	// Note: cube_texture_image is RAII, so its destructor will handle vmaDestroyImage

	ZHLN::Demo::DestroyWindow(win);
	return 0;
}