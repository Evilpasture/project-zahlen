#include <filesystem>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include "RenderCore.hpp"

#include <fstream>
#include <vector>

// ----------------------------------------------------------------------------
// Window State
// ----------------------------------------------------------------------------
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
// Helpers
// ----------------------------------------------------------------------------
[[nodiscard]] auto LoadSpirv(const std::filesystem::path& path) -> std::vector<uint32_t> {
	if (!std::filesystem::exists(path)) return {};
	std::ifstream file(path, std::ios::ate | std::ios::binary);
	if (!file.is_open()) return {};
	const size_t fileSize = static_cast<size_t>(file.tellg());
	std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));
	return buffer;
}

int main() {
	// 1. Win32 Window
	const HINSTANCE hInstance = GetModuleHandleW(nullptr);
	const WNDCLASSEXW wc = {
		.cbSize = sizeof(WNDCLASSEXW),
		.style = CS_OWNDC,
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hCursor = LoadCursor(nullptr, IDC_ARROW),
		.lpszClassName = L"ZHLNTriangle"
	};
	RegisterClassExW(&wc);
	const HWND hwnd = CreateWindowExW(0, L"ZHLNTriangle", L"ZHLN Engine - Triangle",
									  WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, (int)g_width,
									  (int)g_height, nullptr, nullptr, hInstance, nullptr);

	// 2. Vulkan Context Setup
	ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
	const char* inst_exts[] = {
		VK_KHR_SURFACE_EXTENSION_NAME, 
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
		VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
		VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME
	};
	inst_desc.extensions = inst_exts;
	inst_desc.extension_count = 5;

	// Vulkan 1.3 Features
	VkPhysicalDeviceVulkan13Features feat13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	feat13.synchronization2 = VK_TRUE;
	feat13.dynamicRendering = VK_TRUE;

	VkPhysicalDeviceVulkan12Features feat12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &feat13 };
	feat12.bufferDeviceAddress = VK_TRUE;

	VkPhysicalDeviceFeatures2 feat2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &feat12 };

	// Device Extensions (Including Mutable Format for Overlay support)
	const char* dev_exts[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME
	};

	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swap_maint = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
		.swapchainMaintenance1 = VK_TRUE
	};
	feat13.pNext = &swap_maint;

	ZHLN_DeviceSelectDesc sel_desc = { .instance = VK_NULL_HANDLE, .surface = VK_NULL_HANDLE };
	ZHLN_DeviceDesc dev_desc = { 
		.extensions = dev_exts, .extension_count = 3, .features = &feat2, .enable_validation = true 
	};

	auto ctx = ZHLN::Vk::Context::Create(inst_desc, sel_desc, dev_desc);
	if (!ctx) return -1;

	// 3. Surface (RAII)
	VkSurfaceKHR raw_surface;
	VkWin32SurfaceCreateInfoKHR surf_info = {
		.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
		.hinstance = hInstance,
		.hwnd = hwnd
	};
	if (vkCreateWin32SurfaceKHR(ctx.Instance(), &surf_info, nullptr, &raw_surface) != VK_SUCCESS) return -1;
	ZHLN::Vk::Surface surface(ctx.Instance(), raw_surface);

	// 4. Resources
	ZHLN::Vk::Swapchain swapchain(ctx.Device(), {});
	auto sync = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
	auto pools = ZHLN::Vk::CommandPools<3>::Create(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	ZHLN::Vk::SemaphorePool present_semaphores; // NEW RAII POOL

	// 5. Pipeline
	auto vert_code = LoadSpirv("triangle.vert.spv");
	auto frag_code = LoadSpirv("triangle.frag.spv");
	ZHLN_ShaderDesc v_desc = {vert_code.data(), vert_code.size() * 4};
	ZHLN_ShaderDesc f_desc = {frag_code.data(), frag_code.size() * 4};
	auto shaders = ZHLN::Vk::ShaderStages::Create(ctx.Device(), v_desc, f_desc);

	ZHLN_PipelineLayoutDesc layout_desc = {0};
	ZHLN::Vk::PipelineLayout layout(ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &layout_desc));

	ZHLN_GraphicsPipelineDesc pipe_desc = {
		.stages = const_cast<ZHLN_ShaderStages*>(shaders.Get()),
		.layout = layout.Get(),
		.color_format = VK_FORMAT_B8G8R8A8_SRGB,
		.cull_mode = VK_CULL_MODE_NONE,
	};
	ZHLN::Vk::Pipeline pipeline(ctx.Device(), ZHLN_CreateGraphicsPipeline(ctx.Device(), &pipe_desc));

	// 6. Loop
	uint32_t frame_index = 0;
	g_resized = true;

	while (g_running) {
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		if (g_width == 0 || g_height == 0) { Sleep(10); continue; }

		auto rebuild_cb = [&]() {
			vkDeviceWaitIdle(ctx.Device());
			ZHLN_Device raw_dev = {ctx.Device(), ctx.GraphicsQueue(), ctx.PresentQueue()};
			ZHLN_PhysicalDeviceInfo raw_phys = ctx.PhysicalInfo();

			ZHLN_SwapchainDesc s_desc = {
				.device = &raw_dev, .physical = &raw_phys, .surface = surface.Get(),
				.width = g_width, .height = g_height, .vsync = true
			};

			swapchain.Rebuild(s_desc);
			present_semaphores.Rebuild(ctx.Device(), swapchain.Get().image_count); // RAII REBUILD
			g_resized = false;
		};

		if (g_resized) rebuild_cb();

		auto record_cb = [&](VkCommandBuffer cmd, uint32_t image_index) {
			VkImage img = swapchain.Get().images[image_index];
			ZHLN::Vk::TransitionLayout(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			
			ZHLN_RenderPassDesc pass = {
				.target_view = swapchain.Get().views[image_index],
				.extent = swapchain.Get().extent,
				.clear_color = {0.01f, 0.01f, 0.02f, 1.0f}
			};
			{
				ZHLN::Vk::ScopedRendering render(cmd, pass);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
				vkCmdDraw(cmd, 3, 1, 0, 0);
			}
			ZHLN::Vk::TransitionLayout(cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		};

		const ZHLN_FrameSync& frame_sync = sync[frame_index];
		ZHLN_CommandPool& pool = pools[frame_index];
		VkCommandBuffer cmd = pools.Cmd(frame_index);

		ZHLN_WaitAndResetFrame(ctx.Device(), frame_sync.in_flight, &pool);

		uint32_t image_index = 0;
		ZHLN_AcquireDesc acq = { .swapchain = swapchain.Get().handle, .image_available = frame_sync.image_available, .timeout_ns = UINT64_MAX };
		auto acq_res = ZHLN_AcquireImage(ctx.Device(), &acq, &image_index);

		if (acq_res == ZHLN_FrameResult_OutOfDate) { rebuild_cb(); continue; }

		ZHLN_BeginCommandBuffer(cmd);
		record_cb(cmd, image_index);
		ZHLN_EndCommandBuffer(cmd);

		VkCommandBufferSubmitInfo cmd_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd };
		VkSemaphoreSubmitInfo wait_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = frame_sync.image_available, .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT };
		VkSemaphoreSubmitInfo signal_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = present_semaphores[image_index], .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT };

		VkSubmitInfo2 submit = {
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
			.waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait_info,
			.commandBufferInfoCount = 1, .pCommandBufferInfos = &cmd_info,
			.signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal_info,
		};

		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, frame_sync.in_flight);

		ZHLN_PresentDesc pres = { 
			.present_queue = ctx.PresentQueue(), .swapchain = swapchain.Get().handle, 
			.render_finished = present_semaphores[image_index], .image_index = image_index 
		};
		
		if (ZHLN_PresentFrame(&pres) != ZHLN_FrameResult_Ok) rebuild_cb();

		frame_index = (frame_index + 1) % 3;
	}

	vkDeviceWaitIdle(ctx.Device());
	DestroyWindow(hwnd);
	return 0;
}