#include "Allocator.hpp"
#include "RenderCore.hpp"
#include "demo_utils/DemoWindow.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

// ----------------------------------------------------------------------------
// Column-Major, Column-Vector Math Library (P * V * M)
// ----------------------------------------------------------------------------
struct Mat4 {
	std::array<float, 16> data;
};

static Mat4 Identity() noexcept {
	Mat4 m{};
	m.data[0 * 4 + 0] = 1.0f;
	m.data[1 * 4 + 1] = 1.0f;
	m.data[2 * 4 + 2] = 1.0f;
	m.data[3 * 4 + 3] = 1.0f;
	return m;
}

static Mat4 Multiply(const Mat4& a, const Mat4& b) noexcept {
	Mat4 result{};
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k)
				sum += a.data[k * 4 + r] * b.data[c * 4 + k];
			result.data[c * 4 + r] = sum;
		}
	}
	return result;
}

static Mat4 Perspective(float fov, float aspect, float znear, float zfar) noexcept {
	const float f = 1.0f / std::tan(fov * 0.5f);
	Mat4 m{};
	m.data[0 * 4 + 0] = f / aspect;
	m.data[1 * 4 + 1] = -f; // Vulkan Y-Flip
	m.data[2 * 4 + 2] = zfar / (znear - zfar);
	m.data[2 * 4 + 3] = -1.0f;
	m.data[3 * 4 + 2] = (znear * zfar) / (znear - zfar);
	m.data[3 * 4 + 3] = 0.0f;
	return m;
}

static Mat4 RotateX(float radians) noexcept {
	const float s = std::sin(radians);
	const float c = std::cos(radians);
	Mat4 m = Identity();
	m.data[1 * 4 + 1] = c;
	m.data[1 * 4 + 2] = s;
	m.data[2 * 4 + 1] = -s;
	m.data[2 * 4 + 2] = c;
	return m;
}

static Mat4 RotateY(float radians) noexcept {
	const float s = std::sin(radians);
	const float c = std::cos(radians);
	Mat4 m = Identity();
	m.data[0 * 4 + 0] = c;
	m.data[0 * 4 + 2] = -s;
	m.data[2 * 4 + 0] = s;
	m.data[2 * 4 + 2] = c;
	return m;
}

static Mat4 LookAt(const std::array<float, 3>& eye, const std::array<float, 3>& center,
				   const std::array<float, 3>& up) noexcept {
	const std::array<float, 3> f = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
	const float f_len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	const std::array<float, 3> f_norm = {f[0] / f_len, f[1] / f_len, f[2] / f_len};

	const std::array<float, 3> s = {f_norm[1] * up[2] - f_norm[2] * up[1],
									f_norm[2] * up[0] - f_norm[0] * up[2],
									f_norm[0] * up[1] - f_norm[1] * up[0]};
	const float s_len = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
	const std::array<float, 3> s_norm = {s[0] / s_len, s[1] / s_len, s[2] / s_len};

	const std::array<float, 3> u = {s_norm[1] * f_norm[2] - s_norm[2] * f_norm[1],
									s_norm[2] * f_norm[0] - s_norm[0] * f_norm[2],
									s_norm[0] * f_norm[1] - s_norm[1] * f_norm[0]};

	Mat4 m = Identity();
	m.data[0 * 4 + 0] = s_norm[0];
	m.data[1 * 4 + 0] = s_norm[1];
	m.data[2 * 4 + 0] = s_norm[2];
	m.data[0 * 4 + 1] = u[0];
	m.data[1 * 4 + 1] = u[1];
	m.data[2 * 4 + 1] = u[2];
	m.data[0 * 4 + 2] = -f_norm[0];
	m.data[1 * 4 + 2] = -f_norm[1];
	m.data[2 * 4 + 2] = -f_norm[2];

	m.data[3 * 4 + 0] = -(s_norm[0] * eye[0] + s_norm[1] * eye[1] + s_norm[2] * eye[2]);
	m.data[3 * 4 + 1] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
	m.data[3 * 4 + 2] = (f_norm[0] * eye[0] + f_norm[1] * eye[1] + f_norm[2] * eye[2]);
	return m;
}

// ----------------------------------------------------------------------------
// CCW Cube Data
// ----------------------------------------------------------------------------
static const std::array<int, 36> cube_indices = {
	0,	1,	2,	2,	3,	0,	// Front
	4,	5,	6,	6,	7,	4,	// Back
	8,	9,	10, 10, 11, 8,	// Top
	12, 13, 14, 14, 15, 12, // Bottom
	16, 17, 18, 18, 19, 16, // Right
	20, 21, 22, 22, 23, 20	// Left
};

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

	const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME};
	ZHLN_DeviceDesc dev_desc = {.physical = nullptr,
								.extensions = dev_exts,
								.extension_count = 3,
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

	// 4. Pipeline Setup
	auto vert_code = LoadSpirv("cube.vert.spv");
	auto frag_code = LoadSpirv("cube.frag.spv");
	if (vert_code.empty() || frag_code.empty())
		return -1;

	ZHLN_ShaderDesc v_desc = {vert_code.data(), vert_code.size() * sizeof(uint32_t)};
	ZHLN_ShaderDesc f_desc = {frag_code.data(), frag_code.size() * sizeof(uint32_t)};
	auto shaders = ZHLN::Vk::ShaderStages::Create(ctx.Device(), v_desc, f_desc);

	VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(Mat4)};
	ZHLN_PipelineLayoutDesc layout_desc = {.set_layouts = nullptr,
										   .set_layout_count = 0,
										   .push_constants = &push_range,
										   .push_constant_count = 1};
	ZHLN::Vk::PipelineLayout layout(ctx.Device(),
									ZHLN_CreatePipelineLayout(ctx.Device(), &layout_desc));

	ZHLN_GraphicsPipelineDesc pipe_desc = {.stages = const_cast<ZHLN_ShaderStages*>(shaders.Get()),
										   .layout = layout.Get(),
										   .vertex_binding_count = 0,
										   .vertex_bindings = nullptr,
										   .vertex_attribute_count = 0,
										   .vertex_attributes = nullptr,
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

	ZHLN::Vk::Image depth_image;
	VkImageView depth_view = VK_NULL_HANDLE;
	bool depth_initialized = false;

	auto rebuild = [&] {
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

	// 5. Main Loop
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
			ZHLN::Vk::TransitionLayout(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
									   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

			if (depth_view != VK_NULL_HANDLE && !depth_initialized) {
				ZHLN::Vk::TransitionLayout(cmd, depth_image.Handle(), VK_IMAGE_LAYOUT_UNDEFINED,
										   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
										   VK_IMAGE_ASPECT_DEPTH_BIT);
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
				ZHLN::Vk::Push(cmd, layout.Get(), VK_SHADER_STAGE_VERTEX_BIT, mvp);
				vkCmdDraw(cmd, static_cast<uint32_t>(cube_indices.size()), 1, 0, 0);
			}

			ZHLN::Vk::TransitionLayout(cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
									   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
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

	if (depth_view != VK_NULL_HANDLE)
		vkDestroyImageView(ctx.Device(), depth_view, nullptr);
	vkDeviceWaitIdle(ctx.Device());
	ZHLN::Demo::DestroyWindow(win);
	return 0;
}