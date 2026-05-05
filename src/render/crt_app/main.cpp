#include "Allocator.hpp"
#include "RenderCore.hpp"
#include "demo_utils/DemoWindow.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

// Push Constants matching the HLSL struct
struct CRTPushConstants {
	float time;
	float scale;
	float res_x;
	float res_y;
	float background[4];
};

[[nodiscard]] static auto LoadSpirv(const std::filesystem::path& path) -> std::vector<uint32_t> {
	std::ifstream file(path, std::ios::ate | std::ios::binary);
	if (!file.is_open())
		return {};
	const size_t fileSize = static_cast<size_t>(file.tellg());
	std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));
	return buffer;
}

// Generates a Test Pattern (Grid with a colorful center)
static consteval std::vector<uint32_t> GenerateTestTexture(uint32_t width, uint32_t height) {
	std::vector<uint32_t> pixels(width * height);

	auto Pack = [](uint8_t r, uint8_t g, uint8_t b) -> uint32_t {
		return 0xFF000000 | (b << 16) | (g << 8) | r; // ABGR little-endian
	};

	const uint32_t cx = width / 2;
	const uint32_t cy = height / 2;
	const float radius = std::min(width, height) * 0.15625f; // ~80px at 512
	const uint32_t cornerSize = std::max(width, height) / 16;
	const uint32_t bandHalf = std::max(height / 16u, 1u);
	const uint32_t checkSize = std::max(width / 64u, 1u);

	for (uint32_t y = 0; y < height; ++y) {
		for (uint32_t x = 0; x < width; ++x) {
			float u = float(x) / float(width - 1);
			float v = float(y) / float(height - 1);

			// Base UV gradient
			uint8_t r = uint8_t(u * 255.f);
			uint8_t g = uint8_t(v * 255.f);
			uint8_t b = uint8_t((1.f - u * 0.5f - v * 0.5f) * 200.f + 55.f);

			// Corner markers (orientation test)
			bool inTL = x < cornerSize && y < cornerSize;
			bool inTR = x >= width - cornerSize && y < cornerSize;
			bool inBL = x < cornerSize && y >= height - cornerSize;
			bool inBR = x >= width - cornerSize && y >= height - cornerSize;

			if (inTL) {
				r = 255;
				g = 0;
				b = 0;
			} // Red
			else if (inTR) {
				r = 0;
				g = 255;
				b = 0;
			} // Green
			else if (inBL) {
				r = 0;
				g = 0;
				b = 255;
			} // Blue
			else if (inBR) {
				r = 255;
				g = 255;
				b = 0;
			} // Yellow

			// Sub-grid every 16px (subtle darkening)
			bool gridMajX = (x % 64 == 0);
			bool gridMajY = (y % 64 == 0);
			bool gridMinX = (x % 16 == 0);
			bool gridMinY = (y % 16 == 0);

			if ((gridMinX || gridMinY) && !gridMajX && !gridMajY) {
				r = uint8_t(std::max(0, int(r) - 20));
				g = uint8_t(std::max(0, int(g) - 20));
				b = uint8_t(std::max(0, int(b) - 20));
			}

			// Major grid: every 64px (black lines)
			if (gridMajX || gridMajY) {
				r = g = b = 0;
			}

			// Checkerboard band across center (texel-density / UV test)
			bool inBand = (y + bandHalf >= cy) && (y < cy + bandHalf);
			if (inBand && !gridMajX && !gridMajY) {
				bool check = ((x / checkSize) + ((y - (cy - bandHalf)) / checkSize)) % 2 == 0;
				r = g = b = check ? 255 : 0;
			}

			// Circle outline at center (distortion test)
			float dx = float(x) - float(cx);
			float dy = float(y) - float(cy);
			float dist = std::sqrt(dx * dx + dy * dy);
			if (std::abs(dist - radius) < 1.5f && !inBand) {
				r = g = b = 255;
			}

			// Center crosshair
			bool onH = std::abs(int(y) - int(cy)) <= 1 &&
					   std::abs(int(x) - int(cx)) < int(cornerSize * 0.75f);
			bool onV = std::abs(int(x) - int(cx)) <= 1 &&
					   std::abs(int(y) - int(cy)) < int(cornerSize * 0.75f);
			if (onH || onV) {
				r = g = b = 255;
			}

			pixels[y * width + x] = Pack(r, g, b);
		}
	}
	return pixels;
}

template <uint32_t Width, uint32_t Height>
static const std::array<uint32_t, Width * Height> GenerateTVInterruptTexture() {
	std::array<uint32_t, Width * Height> pixels{};

	auto Pack = [](uint8_t r, uint8_t g, uint8_t b) -> uint32_t {
		return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
	};

	// Standard 75% SMPTE colors
	const uint32_t colors[7] = {
		Pack(192, 192, 192), // Gray
		Pack(192, 192, 0),	 // Yellow
		Pack(0, 192, 192),	 // Cyan
		Pack(0, 192, 0),	 // Green
		Pack(192, 0, 192),	 // Magenta
		Pack(192, 0, 0),	 // Red
		Pack(0, 0, 192),	 // Blue
	};

	for (uint32_t y = 0; y < Height; ++y) {
		float v = float(y) / float(Height);
		for (uint32_t x = 0; x < Width; ++x) {
			float u = float(x) / float(Width);
			int barIndex = int(u * 7);
			if (barIndex > 6)
				barIndex = 6;

			uint32_t finalColor;

			if (v < 0.67f) {
				finalColor = colors[barIndex];
			} else if (v < 0.75f) {
				const uint32_t rev[7] = {colors[6], Pack(16, 16, 16), colors[4], Pack(16, 16, 16),
										 colors[2], Pack(16, 16, 16), colors[0]};
				finalColor = rev[barIndex];
			} else {
				if (u < 1.0f / 6.0f)
					finalColor = Pack(0, 33, 76); // I-signal blue
				else if (u < 2.0f / 6.0f)
					finalColor = Pack(255, 255, 255); // White
				else if (u < 3.0f / 6.0f)
					finalColor = Pack(50, 0, 106); // Q-signal purple
				else
					finalColor = Pack(16, 16, 16); // Black
			}

			pixels[y * Width + x] = finalColor;
		}
	}
	return pixels;
}

int main() {
	ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(1024, 768, "ZHLN - CRT Shader");

	// 1. Context Setup
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
		.synchronization2 = VK_TRUE,
		.dynamicRendering = VK_TRUE};
	VkPhysicalDeviceVulkan12Features feat12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &feat13,
		.bufferDeviceAddress = VK_TRUE};
	VkPhysicalDeviceFeatures2 feat2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
									   .pNext = &feat12};

#ifdef __APPLE__
	const char* dev_exts[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, "VK_KHR_portability_subset"};
	const uint32_t dev_ext_count = 4;
#else
	const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME};
	const uint32_t dev_ext_count = 3;
#endif

	ZHLN_DeviceDesc dev_desc = {.physical = nullptr,
								.extensions = dev_exts,
								.extension_count = dev_ext_count,
								.features = &feat2,
								.enable_validation = true};

	auto ctx = ZHLN::Vk::Context::Create(
		inst_desc, {.instance = VK_NULL_HANDLE, .surface = VK_NULL_HANDLE}, dev_desc);
	if (!ctx)
		return -1;

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
	// 2. Texture Creation & Upload
	// =========================================================================
	const uint32_t TEX_W = 512, TEX_H = 512;
	auto pixels = GenerateTVInterruptTexture<TEX_W, TEX_H>();

	VkImageCreateInfo img_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
								  .imageType = VK_IMAGE_TYPE_2D,
								  .format = VK_FORMAT_R8G8B8A8_UNORM,
								  .extent = {TEX_W, TEX_H, 1},
								  .mipLevels = 1,
								  .arrayLayers = 1,
								  .samples = VK_SAMPLE_COUNT_1_BIT,
								  .tiling = VK_IMAGE_TILING_OPTIMAL,
								  .usage =
									  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
								  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
								  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	auto textureImage =
		ZHLN::Vk::Image::Create(allocator.Get(), img_info, VMA_MEMORY_USAGE_GPU_ONLY);

	ZHLN::Vk::CommandPool setupPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	if (!setupPool.Allocate(1))
		return -1;
	VkCommandBuffer setupCmd = setupPool[0];

	ZHLN_BeginCommandBuffer(setupCmd);
	auto stagingBuffer =
		ZHLN::Vk::Buffer::Create(allocator.Get(), pixels.size() * 4,
								 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(stagingBuffer.Map().data, pixels.data(), pixels.size() * 4);

	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
		setupCmd, textureImage.Handle());

	ZHLN_BufferImageCopyDesc copyDesc = {.buffer = stagingBuffer.Handle(),
										 .image = textureImage.Handle(),
										 .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
										 .width = TEX_W,
										 .height = TEX_H,
										 .buffer_offset = 0,
										 .mip_level = 0,
										 .base_array_layer = 0};
	ZHLN::Vk::CopyBufferToImage(setupCmd, copyDesc);

	ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(setupCmd,
																		 textureImage.Handle());
	ZHLN_EndCommandBuffer(setupCmd);

	VkCommandBufferSubmitInfo setupCmdInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											  .commandBuffer = setupCmd};
	VkSubmitInfo2 setupSubmit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								 .commandBufferInfoCount = 1,
								 .pCommandBufferInfos = &setupCmdInfo};
	vkQueueSubmit2(ctx.GraphicsQueue(), 1, &setupSubmit, VK_NULL_HANDLE);
	vkQueueWaitIdle(ctx.GraphicsQueue()); // Wait for upload

	// Image View & Sampler
	VkImageViewCreateInfo view_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
									   .image = textureImage.Handle(),
									   .viewType = VK_IMAGE_VIEW_TYPE_2D,
									   .format = VK_FORMAT_R8G8B8A8_UNORM,
									   .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
															.levelCount = 1,
															.layerCount = 1}};
	VkImageView textureView;
	vkCreateImageView(ctx.Device(), &view_info, nullptr, &textureView);

	VkSamplerCreateInfo sampler_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
										.magFilter = VK_FILTER_LINEAR,
										.minFilter = VK_FILTER_LINEAR,
										.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
										.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
										.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT};
	VkSampler sampler;
	vkCreateSampler(ctx.Device(), &sampler_info, nullptr, &sampler);

	// =========================================================================
	// 3. Descriptor Sets
	// =========================================================================
	VkDescriptorSetLayoutBinding bindings[2] = {{.binding = 0,
												 .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
												 .descriptorCount = 1,
												 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
												{.binding = 1,
												 .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
												 .descriptorCount = 1,
												 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
	VkDescriptorSetLayoutCreateInfo layoutInfo = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = bindings};
	VkDescriptorSetLayout descLayout;
	vkCreateDescriptorSetLayout(ctx.Device(), &layoutInfo, nullptr, &descLayout);

	VkDescriptorPoolSize poolSizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
										 {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
	VkDescriptorPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
										   .maxSets = 1,
										   .poolSizeCount = 2,
										   .pPoolSizes = poolSizes};
	VkDescriptorPool descPool;
	vkCreateDescriptorPool(ctx.Device(), &poolInfo, nullptr, &descPool);

	VkDescriptorSetAllocateInfo allocInfo = {.sType =
												 VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
											 .descriptorPool = descPool,
											 .descriptorSetCount = 1,
											 .pSetLayouts = &descLayout};
	VkDescriptorSet descriptorSet;
	vkAllocateDescriptorSets(ctx.Device(), &allocInfo, &descriptorSet);

	VkDescriptorImageInfo imageInfo = {.imageView = textureView,
									   .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	VkDescriptorImageInfo samplerDescInfo = {.sampler = sampler};

	VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
									   .dstSet = descriptorSet,
									   .dstBinding = 0,
									   .descriptorCount = 1,
									   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
									   .pImageInfo = &imageInfo},
									  {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
									   .dstSet = descriptorSet,
									   .dstBinding = 1,
									   .descriptorCount = 1,
									   .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
									   .pImageInfo = &samplerDescInfo}};
	vkUpdateDescriptorSets(ctx.Device(), 2, writes, 0, nullptr);

	// =========================================================================
	// 4. Pipeline
	// =========================================================================
	auto vert_code = LoadSpirv("crt.hlsl.VSMain.spv");
	auto frag_code = LoadSpirv("crt.hlsl.PSMain.spv");

	// Pass the explicit HLSL entry points
	ZHLN_ShaderDesc v_desc = {vert_code.data(), vert_code.size() * 4, "VSMain"};
	ZHLN_ShaderDesc f_desc = {frag_code.data(), frag_code.size() * 4, "PSMain"};
	auto shaders = ZHLN::Vk::ShaderStages::Create(ctx.Device(), v_desc, f_desc);

	VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(CRTPushConstants)};
	ZHLN_PipelineLayoutDesc pLayoutDesc = {.set_layouts = &descLayout,
										   .set_layout_count = 1,
										   .push_constants = &push_range,
										   .push_constant_count = 1};
	ZHLN::Vk::PipelineLayout pipelineLayout(ctx.Device(),
											ZHLN_CreatePipelineLayout(ctx.Device(), &pLayoutDesc));

	ZHLN_GraphicsPipelineDesc pipe_desc = {.stages = const_cast<ZHLN_ShaderStages*>(shaders.Get()),
										   .layout = pipelineLayout.Get(),
										   .color_format = VK_FORMAT_B8G8R8A8_SRGB,
										   .depth_format = VK_FORMAT_UNDEFINED,
										   .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
										   .polygon_mode = VK_POLYGON_MODE_FILL,
										   .cull_mode = VK_CULL_MODE_NONE,
										   .front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE,
										   .depth_test = false,
										   .depth_write = false,
										   .blend_enable = false};
	ZHLN::Vk::Pipeline pipeline(ctx.Device(),
								ZHLN_CreateGraphicsPipeline(ctx.Device(), &pipe_desc));

	if (!pipeline.Valid()) {
		std::println(stderr, "FATAL: Failed to create graphics pipeline.");
		return -1;
	}

	// =========================================================================
	// 5. Render Loop
	// =========================================================================
	uint32_t frame_index = 0;
	win.resized = true;
	auto startTime = std::chrono::high_resolution_clock::now();

	while (win.running) {
		ZHLN::Demo::ProcessEvents(win);
		if (win.width == 0 || win.height == 0)
			continue;

		if (win.resized) {
			vkDeviceWaitIdle(ctx.Device());
			ZHLN_Device raw_dev = {ctx.Device(), ctx.GraphicsQueue(), ctx.PresentQueue()};
			ZHLN_PhysicalDeviceInfo raw_phys = ctx.PhysicalInfo();
			ZHLN_SwapchainDesc s_desc = {.device = &raw_dev,
										 .physical = &raw_phys,
										 .surface = surface.Get(),
										 .width = win.width,
										 .height = win.height,
										 .vsync = true,
										 .old_swapchain = VK_NULL_HANDLE};
			swapchain.Rebuild(s_desc);
			present_semaphores.Rebuild(ctx.Device(), swapchain.Get().image_count);
			win.resized = false;
		}

		auto now = std::chrono::high_resolution_clock::now();
		float time = std::chrono::duration<float>(now - startTime).count();

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
		VkImage img = swapchain.Get().images[image_index];
		ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED,
								   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, img);

		ZHLN_RenderPassDesc pass = {.target_view = swapchain.Get().views[image_index],
									.depth_view = VK_NULL_HANDLE,
									.extent = swapchain.Get().extent,
									.clear_color = {0, 0, 0, 1}};
		{
			ZHLN::Vk::ScopedRendering render(cmd, pass);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout.Get(), 0,
									1, &descriptorSet, 0, nullptr);

			CRTPushConstants pc = {.time = time,
								   .scale = 1.0f,
								   .res_x = (float)win.width,
								   .res_y = (float)win.height,
								   .background = {0, 0, 0, 1}};
			ZHLN::Vk::Push(cmd, pipelineLayout.Get(), VK_SHADER_STAGE_FRAGMENT_BIT, pc);

			vkCmdDraw(cmd, 3, 1, 0, 0); // Draw fullscreen triangle
		}
		ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(cmd, img);
		ZHLN_EndCommandBuffer(cmd);

		VkCommandBufferSubmitInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											  .commandBuffer = cmd};
		VkSemaphoreSubmitInfo wait_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
										   .semaphore = frame_sync.image_available,
										   .stageMask =
											   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
		VkSemaphoreSubmitInfo signal_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
											 .semaphore = present_semaphores[image_index],
											 .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT};
		VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								.waitSemaphoreInfoCount = 1,
								.pWaitSemaphoreInfos = &wait_info,
								.commandBufferInfoCount = 1,
								.pCommandBufferInfos = &cmd_info,
								.signalSemaphoreInfoCount = 1,
								.pSignalSemaphoreInfos = &signal_info};
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
	vkDestroySampler(ctx.Device(), sampler, nullptr);
	vkDestroyImageView(ctx.Device(), textureView, nullptr);
	vkDestroyDescriptorPool(ctx.Device(), descPool, nullptr);
	vkDestroyDescriptorSetLayout(ctx.Device(), descLayout, nullptr);
	ZHLN::Demo::DestroyWindow(win);
	return 0;
}