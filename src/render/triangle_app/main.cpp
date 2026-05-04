#include "RenderCore.hpp"
#include "demo_utils/DemoWindow.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
[[nodiscard]] static auto LoadSpirv(const std::filesystem::path& path) -> std::vector<uint32_t> {
	if (!std::filesystem::exists(path))
		return {};
	std::ifstream file(path, std::ios::ate | std::ios::binary);
	if (!file.is_open())
		return {};
	const size_t fileSize = static_cast<size_t>(file.tellg());
	std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));
	return buffer;
}

int main() {
	// 1. OS Window Creation (Platform Agnostic)
	ZHLN::Demo::WindowState win = ZHLN::Demo::InitWindow(800, 600, "ZHLN Engine - Triangle");

	// 2. Vulkan Context Setup
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

	ZHLN_DeviceSelectDesc sel_desc = {.instance = VK_NULL_HANDLE, .surface = VK_NULL_HANDLE};

	auto ctx = ZHLN::Vk::Context::Create(inst_desc, sel_desc, dev_desc);
	if (!ctx)
		return -1;

	// 3. Surface Creation (Platform Agnostic)
	VkSurfaceKHR raw_surface = ZHLN::Demo::CreateSurface(ctx.Instance(), win);
	ZHLN::Vk::Surface surface(ctx.Instance(), raw_surface);

	// 4. Resources
	ZHLN::Vk::Swapchain swapchain(ctx.Device(), {});
	auto sync = ZHLN::Vk::FrameSync<3>::Create(ctx.Device());
	auto pools =
		ZHLN::Vk::CommandPools<3>::Create(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	ZHLN::Vk::SemaphorePool present_semaphores;

	// 5. Pipeline
	auto vert_code = LoadSpirv("triangle.vert.spv");
	auto frag_code = LoadSpirv("triangle.frag.spv");
	ZHLN_ShaderDesc v_desc = {vert_code.data(), vert_code.size() * 4};
	ZHLN_ShaderDesc f_desc = {frag_code.data(), frag_code.size() * 4};
	auto shaders = ZHLN::Vk::ShaderStages::Create(ctx.Device(), v_desc, f_desc);

	ZHLN_PipelineLayoutDesc layout_desc = {.set_layouts = nullptr,
										   .set_layout_count = 0,
										   .push_constants = nullptr,
										   .push_constant_count = 0};
	ZHLN::Vk::PipelineLayout layout(ctx.Device(),
									ZHLN_CreatePipelineLayout(ctx.Device(), &layout_desc));

	ZHLN_GraphicsPipelineDesc pipe_desc = {.stages = const_cast<ZHLN_ShaderStages*>(shaders.Get()),
										   .layout = layout.Get(),
										   .vertex_binding_count = 0,
										   .vertex_bindings = nullptr,
										   .vertex_attribute_count = 0,
										   .vertex_attributes = nullptr,
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

	// 6. Loop
	uint32_t frame_index = 0;
	win.resized = true;

	while (win.running) {
		ZHLN::Demo::ProcessEvents(win);

		if (win.width == 0 || win.height == 0)
			continue;

		auto rebuild_cb = [&]() {
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
		};

		if (win.resized)
			rebuild_cb();

		auto record_cb = [&](VkCommandBuffer cmd, uint32_t image_index) {
			VkImage img = swapchain.Get().images[image_index];
			ZHLN::Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED,
									   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, img);

			ZHLN_RenderPassDesc pass = {.target_view = swapchain.Get().views[image_index],
										.depth_view = VK_NULL_HANDLE,
										.extent = swapchain.Get().extent,
										.clear_color = {0.01f, 0.01f, 0.02f, 1.0f},
										.clear_depth = 1.0f};
			{
				ZHLN::Vk::ScopedRendering render(cmd, pass);
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
				vkCmdDraw(cmd, 3, 1, 0, 0);
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
	ZHLN::Demo::DestroyWindow(win);
	return 0;
}