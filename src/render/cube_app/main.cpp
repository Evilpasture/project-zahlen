#include <filesystem>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include "Allocator.hpp"
#include "RenderCore.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <vector>

static bool g_running = true;
static bool g_resized = false;
static uint32_t g_width = 800;
static uint32_t g_height = 600;

auto CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
	if (msg == WM_CLOSE) {
		g_running = false;
		return 0;
	}
	if (msg == WM_SIZE) {
		g_width = LOWORD(lp);
		g_height = HIWORD(lp);
		g_resized = true;
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

// ----------------------------------------------------------------------------
// Column-Major, Column-Vector Math Library (P * V * M)
// ----------------------------------------------------------------------------
struct Mat4 {
	// data[column * 4 + row]
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
			for (int k = 0; k < 4; ++k) {
				sum += a.data[k * 4 + r] * b.data[c * 4 + k];
			}
			result.data[c * 4 + r] = sum;
		}
	}
	return result;
}

static Mat4 Perspective(float fov, float aspect, float znear, float zfar) noexcept {
	const float f = 1.0f / std::tan(fov * 0.5f);
	Mat4 m{};
	m.data[0 * 4 + 0] = f / aspect;
	m.data[1 * 4 + 1] = -f; // Vulkan Y-Flip (inverts the Y axis for Clip Space)
	m.data[2 * 4 + 2] = zfar / (znear - zfar);
	m.data[2 * 4 + 3] = -1.0f;
	m.data[3 * 4 + 2] = (znear * zfar) / (znear - zfar);
	m.data[3 * 4 + 3] = 0.0f;
	return m;
}

static Mat4 Translate(float x, float y, float z) noexcept {
	Mat4 m = Identity();
	m.data[3 * 4 + 0] = x;
	m.data[3 * 4 + 1] = y;
	m.data[3 * 4 + 2] = z;
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
	const std::array<float, 3> f = {
		center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]
	};
	const float f_len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
	const std::array<float, 3> f_norm = {f[0] / f_len, f[1] / f_len, f[2] / f_len};

	const std::array<float, 3> s = {
		f_norm[1] * up[2] - f_norm[2] * up[1],
		f_norm[2] * up[0] - f_norm[0] * up[2],
		f_norm[0] * up[1] - f_norm[1] * up[0]
	};
	const float s_len = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
	const std::array<float, 3> s_norm = {s[0] / s_len, s[1] / s_len, s[2] / s_len};

	const std::array<float, 3> u = {
		s_norm[1] * f_norm[2] - s_norm[2] * f_norm[1],
		s_norm[2] * f_norm[0] - s_norm[0] * f_norm[2],
		s_norm[0] * f_norm[1] - s_norm[1] * f_norm[0]
	};

	Mat4 m = Identity();
	m.data[0 * 4 + 0] = s_norm[0]; m.data[1 * 4 + 0] = s_norm[1]; m.data[2 * 4 + 0] = s_norm[2];
	m.data[0 * 4 + 1] = u[0];      m.data[1 * 4 + 1] = u[1];      m.data[2 * 4 + 1] = u[2];
	m.data[0 * 4 + 2] = -f_norm[0];m.data[1 * 4 + 2] = -f_norm[1];m.data[2 * 4 + 2] = -f_norm[2];
	
	m.data[3 * 4 + 0] = -(s_norm[0] * eye[0] + s_norm[1] * eye[1] + s_norm[2] * eye[2]);
	m.data[3 * 4 + 1] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
	m.data[3 * 4 + 2] =  (f_norm[0] * eye[0] + f_norm[1] * eye[1] + f_norm[2] * eye[2]);
	return m;
}

// ----------------------------------------------------------------------------
// CCW Cube Data
// ----------------------------------------------------------------------------
static const std::array<std::array<float, 3>, 24> positions = {
	// Face 0: Front (+Z)
	std::array<float, 3>{-1.0f, -1.0f,  1.0f}, // 0: BL
	std::array<float, 3>{ 1.0f, -1.0f,  1.0f}, // 1: BR
	std::array<float, 3>{ 1.0f,  1.0f,  1.0f}, // 2: TR
	std::array<float, 3>{-1.0f,  1.0f,  1.0f}, // 3: TL

	// Face 1: Back (-Z)
	std::array<float, 3>{ 1.0f, -1.0f, -1.0f}, // 4: BL (from observer)
	std::array<float, 3>{-1.0f, -1.0f, -1.0f}, // 5: BR
	std::array<float, 3>{-1.0f,  1.0f, -1.0f}, // 6: TR
	std::array<float, 3>{ 1.0f,  1.0f, -1.0f}, // 7: TL

	// Face 2: Top (+Y)
	std::array<float, 3>{-1.0f,  1.0f,  1.0f}, // 8: BL
	std::array<float, 3>{ 1.0f,  1.0f,  1.0f}, // 9: BR
	std::array<float, 3>{ 1.0f,  1.0f, -1.0f}, // 10: TR
	std::array<float, 3>{-1.0f,  1.0f, -1.0f}, // 11: TL

	// Face 3: Bottom (-Y)
	std::array<float, 3>{-1.0f, -1.0f, -1.0f}, // 12: BL
	std::array<float, 3>{ 1.0f, -1.0f, -1.0f}, // 13: BR
	std::array<float, 3>{ 1.0f, -1.0f,  1.0f}, // 14: TR
	std::array<float, 3>{-1.0f, -1.0f,  1.0f}, // 15: TL

	// Face 4: Right (+X)
	std::array<float, 3>{ 1.0f, -1.0f,  1.0f}, // 16: BL
	std::array<float, 3>{ 1.0f, -1.0f, -1.0f}, // 17: BR
	std::array<float, 3>{ 1.0f,  1.0f, -1.0f}, // 18: TR
	std::array<float, 3>{ 1.0f,  1.0f,  1.0f}, // 19: TL

	// Face 5: Left (-X)
	std::array<float, 3>{-1.0f, -1.0f, -1.0f}, // 20: BL
	std::array<float, 3>{-1.0f, -1.0f,  1.0f}, // 21: BR
	std::array<float, 3>{-1.0f,  1.0f,  1.0f}, // 22: TR
	std::array<float, 3>{-1.0f,  1.0f, -1.0f}, // 23: TL
};

// Perfectly consistent CCW winding order for all 6 faces
static const std::array<int, 36> cube_indices = {
	0,  1,  2,  2,  3,  0,  // Front
	4,  5,  6,  6,  7,  4,  // Back
	8,  9,  10, 10, 11, 8,  // Top
	12, 13, 14, 14, 15, 12, // Bottom
	16, 17, 18, 18, 19, 16, // Right
	20, 21, 22, 22, 23, 20  // Left
};

// ----------------------------------------------------------------------------
// Vulkan App
// ----------------------------------------------------------------------------
static std::vector<uint32_t> LoadSpirv(const std::filesystem::path& path) {
	if (!std::filesystem::exists(path)) return {};
	std::ifstream file(path, std::ios::ate | std::ios::binary);
	if (!file.is_open()) return {};
	const size_t file_size = static_cast<size_t>(file.tellg());
	std::vector<uint32_t> buffer(file_size / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(file_size));
	return buffer;
}

static VkImageView CreateDepthImageView(VkDevice device, VkImage image, VkFormat format) noexcept {
	VkImageViewCreateInfo view_info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = format,
		.subresourceRange = {
			.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
			.baseMipLevel = 0,
			.levelCount = 1,
			.baseArrayLayer = 0,
			.layerCount = 1,
		},
	};
	VkImageView view = VK_NULL_HANDLE;
	vkCreateImageView(device, &view_info, nullptr, &view);
	return view;
}

int main() {
	const HINSTANCE hInstance = GetModuleHandleW(nullptr);
	const WNDCLASSEXW wc = {
		.cbSize = sizeof(WNDCLASSEXW), .style = CS_OWNDC, .lpfnWndProc = WndProc,
		.hInstance = hInstance, .hCursor = LoadCursor(nullptr, IDC_ARROW), .lpszClassName = L"ZHLNCube",
	};
	RegisterClassExW(&wc);
	const HWND hwnd = CreateWindowExW(0, L"ZHLNCube", L"ZHLN Engine - Cube", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
									  100, 100, static_cast<int>(g_width), static_cast<int>(g_height), nullptr,
									  nullptr, hInstance, nullptr);

	ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
	const char* inst_exts[] = {
		VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
		VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
	};
	inst_desc.extensions = inst_exts;
	inst_desc.extension_count = _countof(inst_exts);

	VkPhysicalDeviceVulkan13Features feat13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	feat13.synchronization2 = VK_TRUE;
	feat13.dynamicRendering = VK_TRUE;

	VkPhysicalDeviceVulkan12Features feat12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &feat13};
	feat12.bufferDeviceAddress = VK_TRUE;

	VkPhysicalDeviceFeatures2 feat2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &feat12};

	const char* dev_exts[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME,
	};

	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swap_maint = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR, .swapchainMaintenance1 = VK_TRUE,
	};
	feat13.pNext = &swap_maint;

	ZHLN_DeviceSelectDesc sel_desc = {.instance = VK_NULL_HANDLE, .surface = VK_NULL_HANDLE};
	ZHLN_DeviceDesc dev_desc = {.extensions = dev_exts, .extension_count = _countof(dev_exts), .features = &feat2, .enable_validation = true};

	auto ctx = ZHLN::Vk::Context::Create(inst_desc, sel_desc, dev_desc);
	if (!ctx) return -1;

	VkSurfaceKHR raw_surface;
	VkWin32SurfaceCreateInfoKHR surf_info = {
		.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, .hinstance = hInstance, .hwnd = hwnd,
	};
	if (vkCreateWin32SurfaceKHR(ctx.Instance(), &surf_info, nullptr, &raw_surface) != VK_SUCCESS) return -1;
	ZHLN::Vk::Surface surface(ctx.Instance(), raw_surface);

	ZHLN::Vk::Allocator allocator;
	if (!allocator.Init(ctx)) return -1;

	ZHLN::Vk::Swapchain swapchain(ctx.Device(), {});
	auto sync = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
	auto pools = ZHLN::Vk::CommandPools<3>::Create(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	ZHLN::Vk::SemaphorePool present_semaphores;

	auto vert_code = LoadSpirv("cube.vert.spv");
	auto frag_code = LoadSpirv("cube.frag.spv");
	if (vert_code.empty() || frag_code.empty()) return -1;

	ZHLN_ShaderDesc v_desc = {vert_code.data(), vert_code.size() * sizeof(uint32_t)};
	ZHLN_ShaderDesc f_desc = {frag_code.data(), frag_code.size() * sizeof(uint32_t)};
	auto shaders = ZHLN::Vk::ShaderStages::Create(ctx.Device(), v_desc, f_desc);
	if (!shaders.Valid()) return -1;

	VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(Mat4),
	};
	ZHLN_PipelineLayoutDesc layout_desc = {
		.push_constants = &push_range, .push_constant_count = 1,
	};
	ZHLN::Vk::PipelineLayout layout(ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &layout_desc));
	if (!layout.Valid()) return -1;

	ZHLN_GraphicsPipelineDesc pipe_desc{};
	pipe_desc.stages = const_cast<ZHLN_ShaderStages*>(shaders.Get());
	pipe_desc.layout = layout.Get();
	pipe_desc.color_format = VK_FORMAT_B8G8R8A8_SRGB;
	pipe_desc.depth_format = VK_FORMAT_D32_SFLOAT;
	pipe_desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	pipe_desc.cull_mode = VK_CULL_MODE_BACK_BIT;
	
	pipe_desc.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	
	pipe_desc.depth_test = true;
	pipe_desc.depth_write = true;

	ZHLN::Vk::Pipeline pipeline(ctx.Device(), ZHLN_CreateGraphicsPipeline(ctx.Device(), &pipe_desc));
	if (!pipeline.Valid()) return -1;

	ZHLN::Vk::Image depth_image;
	VkImageView depth_view = VK_NULL_HANDLE;
	bool depth_initialized = false;

	auto rebuild = [&] {
		vkDeviceWaitIdle(ctx.Device());

		depth_initialized = false;
		ZHLN_Device raw_dev = {ctx.Device(), ctx.GraphicsQueue(), ctx.PresentQueue()};
		ZHLN_PhysicalDeviceInfo raw_phys = ctx.PhysicalInfo();
		ZHLN_SwapchainDesc s_desc = {
			.device = &raw_dev, .physical = &raw_phys, .surface = surface.Get(),
			.width = g_width, .height = g_height, .vsync = true,
		};
		if (!swapchain.Rebuild(s_desc)) return false;

		present_semaphores.Rebuild(ctx.Device(), swapchain.Get().image_count);

		if (depth_view != VK_NULL_HANDLE) {
			vkDestroyImageView(ctx.Device(), depth_view, nullptr);
			depth_view = VK_NULL_HANDLE;
		}

		VkImageCreateInfo img_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_D32_SFLOAT,
			.extent = {g_width, g_height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		depth_image = ZHLN::Vk::Image::Create(allocator.Get(), img_info, VMA_MEMORY_USAGE_GPU_ONLY);
		if (depth_image.Handle() == VK_NULL_HANDLE) return false;

		depth_view = CreateDepthImageView(ctx.Device(), depth_image.Handle(), VK_FORMAT_D32_SFLOAT);
		return depth_view != VK_NULL_HANDLE;
	};

	auto when = std::chrono::high_resolution_clock::now();
	g_resized = true;
	uint32_t frame_index = 0;

	while (g_running) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (g_width == 0 || g_height == 0) { Sleep(10); continue; }
		if (g_resized && !rebuild()) break;

		const auto now = std::chrono::high_resolution_clock::now();
		const float elapsed = std::chrono::duration<float>(now - when).count();
		
		const Mat4 model = Multiply(RotateY(elapsed * 0.75f), RotateX(elapsed * 0.45f));
		const Mat4 view = LookAt({2.5f, 2.0f, 2.5f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
		const Mat4 proj = Perspective(1.0472f, static_cast<float>(g_width) / static_cast<float>(g_height), 0.1f, 10.0f);
		
		// Column-Vector Math: P * V * M
		const Mat4 mvp = Multiply(proj, Multiply(view, model));

		auto record_cb = [&](VkCommandBuffer cmd, uint32_t image_index) {
			VkImage img = swapchain.Get().images[image_index];
			ZHLN::Vk::TransitionLayout(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

			if (depth_view != VK_NULL_HANDLE && !depth_initialized) {
				ZHLN::Vk::TransitionLayout(cmd, depth_image.Handle(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
				depth_initialized = true;
			}

			ZHLN_RenderPassDesc pass = {
				.target_view = swapchain.Get().views[image_index], .depth_view = depth_view, .extent = swapchain.Get().extent,
				.clear_color = {0.05f, 0.05f, 0.08f, 1.0f}, .clear_depth = 1.0f,
			};
			
			{
				ZHLN::Vk::ScopedRendering render(cmd, pass);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());

				// Upload Column-Major matrix directly (no transpose!)
				ZHLN::Vk::Push(cmd, layout.Get(), VK_SHADER_STAGE_VERTEX_BIT, mvp);

				vkCmdDraw(cmd, static_cast<uint32_t>(cube_indices.size()), 1, 0, 0);
			}
			
			ZHLN::Vk::TransitionLayout(cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		};

		const ZHLN_FrameSync& frame_sync = sync[frame_index];
		ZHLN_CommandPool& pool = pools[frame_index];
		VkCommandBuffer cmd = pools.Cmd(frame_index);

		ZHLN_WaitAndResetFrame(ctx.Device(), frame_sync.in_flight, &pool);

		uint32_t image_index = 0;
		ZHLN_AcquireDesc acq = {.swapchain = swapchain.Get().handle, .image_available = frame_sync.image_available, .timeout_ns = UINT64_MAX};
		if (ZHLN_AcquireImage(ctx.Device(), &acq, &image_index) == ZHLN_FrameResult_OutOfDate) {
			g_resized = true; continue;
		}

		ZHLN_BeginCommandBuffer(cmd);
		record_cb(cmd, image_index);
		ZHLN_EndCommandBuffer(cmd);

		VkCommandBufferSubmitInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd};
		VkSemaphoreSubmitInfo wait_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = frame_sync.image_available, .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
		VkSemaphoreSubmitInfo signal_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = present_semaphores[image_index], .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT};

		VkSubmitInfo2 submit = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait_info,
			.commandBufferInfoCount = 1, .pCommandBufferInfos = &cmd_info,
			.signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal_info,
		};
		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, frame_sync.in_flight);

		ZHLN_PresentDesc pres = {.present_queue = ctx.PresentQueue(), .swapchain = swapchain.Get().handle, .render_finished = present_semaphores[image_index], .image_index = image_index};
		if (ZHLN_PresentFrame(&pres) != ZHLN_FrameResult_Ok) g_resized = true;

		frame_index = (frame_index + 1) % 3;
	}

	if (depth_view != VK_NULL_HANDLE) vkDestroyImageView(ctx.Device(), depth_view, nullptr);
	vkDeviceWaitIdle(ctx.Device());
	DestroyWindow(hwnd);
	return 0;
}