#include "Allocator.hpp"
#include "RenderCore.hpp"

#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <vector>

namespace ZHLN {

struct NativeMesh {
	Vk::Buffer buffer;
	uint32_t vertexCount;
};

struct NativeMaterial {
	Vk::Pipeline pipeline;
	Vk::PipelineLayout layout;
};

struct RenderContext::Impl {
	Window& window;
	Vk::Context ctx;
	Vk::Allocator allocator;
	Vk::Surface surface;
	Vk::Swapchain swapchain;
	Vk::FrameSync<2> sync;
	Vk::CommandPools<2> pools;
	Vk::SemaphorePool present_semaphores;

	// Depth Buffer Management
	Vk::Image depth_image;
	VkImageView depth_view = VK_NULL_HANDLE;

	uint32_t frame_index = 0;
	bool resized = true;

	VkCommandBuffer current_cmd = VK_NULL_HANDLE;
	uint32_t current_image_index = 0;

	JPH::Mat44 current_view_proj;

	std::vector<std::unique_ptr<NativeMesh>> meshes;
	std::vector<std::unique_ptr<NativeMaterial>> materials;

	Impl(Window& win) : window(win) {}
};

RenderContext::RenderContext(Window& window, const String32& preferredAPI)
	: _impl(std::make_unique<Impl>(window)) {

	uint32_t glfwExtensionCount = 0;
	const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

	std::vector<const char*> inst_exts(glfwExtensions, glfwExtensions + glfwExtensionCount);
	inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	inst_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
	inst_exts.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

#ifdef __APPLE__
	inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
#endif

	ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
	inst_desc.extensions = inst_exts.data();
	inst_desc.extension_count = static_cast<uint32_t>(inst_exts.size());

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

	VkPhysicalDeviceFeatures2 feat2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &feat12, .features = {}};

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

	_impl->ctx = Vk::Context::Create(inst_desc, {VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, nullptr},
									 dev_desc);
	if (!_impl->ctx) {
		ZHLN::Log("FATAL: Vulkan Context Creation Failed");
		std::abort();
	}

	GLFWwindow* glfwWin = static_cast<GLFWwindow*>(window.GetNativeHandle());
	VkSurfaceKHR raw_surface;
	if (glfwCreateWindowSurface(_impl->ctx.Instance(), glfwWin, nullptr, &raw_surface) !=
		VK_SUCCESS) {
		std::abort();
	}
	_impl->surface = Vk::Surface(_impl->ctx.Instance(), raw_surface);

	if (!_impl->allocator.Init(_impl->ctx)) {
		ZHLN::Log("FATAL: Vulkan Memory Allocator (VMA) failed to initialize");
		std::abort();
	}

	_impl->swapchain = Vk::Swapchain(_impl->ctx.Device(), {});
	_impl->sync = Vk::FrameSync<2>::Create(_impl->ctx.Device());
	_impl->pools =
		Vk::CommandPools<2>::Create(_impl->ctx.Device(), _impl->ctx.PhysicalInfo().graphics_family);
}

RenderContext::~RenderContext() {
	if (_impl->ctx.Device()) {
		vkDeviceWaitIdle(_impl->ctx.Device());
		if (_impl->depth_view != VK_NULL_HANDLE) {
			vkDestroyImageView(_impl->ctx.Device(), _impl->depth_view, nullptr);
		}
	}
}

const char* RenderContext::GetRendererName() const {
	return "Vulkan 1.3 (ZHLN)";
}
void RenderContext::SetResolution(const Extent2D& res) {
	_impl->resized = true;
}

BufferHandle RenderContext::CreateVertexBuffer(const void* data, size_t size) {
	auto gpu_buf =
		Vk::Buffer::Create(_impl->allocator.Get(), size,
						   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
						   VMA_MEMORY_USAGE_GPU_ONLY);

	VkCommandBuffer cmd = _impl->pools.Cmd(0);
	ZHLN_BeginCommandBuffer(cmd);
	auto staging = Vk::UploadToBuffer(_impl->allocator.Get(), cmd, gpu_buf, data, size);
	ZHLN_EndCommandBuffer(cmd);

	VkCommandBufferSubmitInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
										  .commandBuffer = cmd};
	VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
							.commandBufferInfoCount = 1,
							.pCommandBufferInfos = &cmd_info};
	vkQueueSubmit2(_impl->ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(_impl->ctx.GraphicsQueue());

	auto* mesh = new NativeMesh{std::move(gpu_buf), static_cast<uint32_t>(size / sizeof(Vertex))};
	_impl->meshes.emplace_back(mesh);
	return static_cast<BufferHandle>(reinterpret_cast<uint64_t>(mesh));
}

BufferHandle RenderContext::CreateConstantBuffer(size_t size) {
	return BufferHandle::Invalid;
}

Material RenderContext::CreateMaterial(const PipelineDesc& desc) {
	ZHLN_ShaderDesc v_desc = {static_cast<const uint32_t*>(desc.vertexShaderData),
							  desc.vertexShaderSize};
	ZHLN_ShaderDesc f_desc = {static_cast<const uint32_t*>(desc.fragShaderData),
							  desc.fragShaderSize};
	auto shaders = Vk::ShaderStages::Create(_impl->ctx.Device(), v_desc, f_desc);

	VkPushConstantRange pc_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(JPH::Mat44)};
	ZHLN_PipelineLayoutDesc layout_desc = {.push_constants = &pc_range, .push_constant_count = 1};
	auto layout = Vk::PipelineLayout(_impl->ctx.Device(),
									 ZHLN_CreatePipelineLayout(_impl->ctx.Device(), &layout_desc));

	VkVertexInputBindingDescription binding = {
		.binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
	VkVertexInputAttributeDescription attrs[2] = {{.location = 0,
												   .binding = 0,
												   .format = VK_FORMAT_R32G32B32_SFLOAT,
												   .offset = (uint32_t)offsetof(Vertex, position)},
												  {.location = 1,
												   .binding = 0,
												   .format = VK_FORMAT_R32G32B32A32_SFLOAT,
												   .offset = (uint32_t)offsetof(Vertex, color)}};

	ZHLN_GraphicsPipelineDesc pipe_desc = {.stages = const_cast<ZHLN_ShaderStages*>(shaders.Get()),
										   .layout = layout.Get(),
										   .vertex_bindings = &binding,
										   .vertex_attributes = attrs,
										   .vertex_binding_count = 1,
										   .vertex_attribute_count = 2,
										   .color_format = VK_FORMAT_B8G8R8A8_SRGB,
										   .depth_format =
											   VK_FORMAT_D32_SFLOAT, // DEPTH FORMAT ENABLED
										   .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
										   .polygon_mode = VK_POLYGON_MODE_FILL,
										   .cull_mode = VK_CULL_MODE_BACK_BIT, // CULLING ENABLED
										   .front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE,
										   .depth_test = true,	// DEPTH TEST ENABLED
										   .depth_write = true, // DEPTH WRITE ENABLED
										   .blend_enable = false};
	auto pipeline = Vk::Pipeline(_impl->ctx.Device(),
								 ZHLN_CreateGraphicsPipeline(_impl->ctx.Device(), &pipe_desc));

	auto* mat = new NativeMaterial{std::move(pipeline), std::move(layout)};
	_impl->materials.emplace_back(mat);
	return Material{.pipeline = static_cast<PipelineHandle>(reinterpret_cast<uint64_t>(mat))};
}

void RenderContext::BeginFrame() {
	auto rebuild_cb = [&]() {
		vkDeviceWaitIdle(_impl->ctx.Device());

		int w, h;
		glfwGetFramebufferSize(static_cast<GLFWwindow*>(_impl->window.GetNativeHandle()), &w, &h);

		ZHLN_Device raw_dev = {_impl->ctx.Device(), _impl->ctx.GraphicsQueue(),
							   _impl->ctx.PresentQueue()};
		ZHLN_PhysicalDeviceInfo raw_phys = _impl->ctx.PhysicalInfo();

		ZHLN_SwapchainSupportDesc sdesc = {raw_phys.handle, _impl->surface.Get()};
		auto caps = ZHLN_QuerySwapchainSupport(&sdesc).capabilities;

		uint32_t final_w =
			(caps.currentExtent.width != 0xFFFFFFFF)
				? caps.currentExtent.width
				: JPH::Clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width);
		uint32_t final_h =
			(caps.currentExtent.height != 0xFFFFFFFF)
				? caps.currentExtent.height
				: JPH::Clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height);

		ZHLN_SwapchainDesc s_desc = {.device = &raw_dev,
									 .physical = &raw_phys,
									 .surface = _impl->surface.Get(),
									 .width = final_w,
									 .height = final_h,
									 .vsync = true,
									 .old_swapchain = VK_NULL_HANDLE};
		_impl->swapchain.Rebuild(s_desc);
		_impl->present_semaphores.Rebuild(_impl->ctx.Device(), _impl->swapchain.Get().image_count);

		// Depth Buffer Rebuild
		if (_impl->depth_view) {
			vkDestroyImageView(_impl->ctx.Device(), _impl->depth_view, nullptr);
		}
		VkImageCreateInfo d_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
									.imageType = VK_IMAGE_TYPE_2D,
									.format = VK_FORMAT_D32_SFLOAT,
									.extent = {final_w, final_h, 1},
									.mipLevels = 1,
									.arrayLayers = 1,
									.samples = VK_SAMPLE_COUNT_1_BIT,
									.tiling = VK_IMAGE_TILING_OPTIMAL,
									.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
									.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
									.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
		_impl->depth_image =
			Vk::Image::Create(_impl->allocator.Get(), d_info, VMA_MEMORY_USAGE_GPU_ONLY);

		VkImageViewCreateInfo v_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = _impl->depth_image.Handle(),
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = VK_FORMAT_D32_SFLOAT,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1}};
		vkCreateImageView(_impl->ctx.Device(), &v_info, nullptr, &_impl->depth_view);

		_impl->resized = false;
	};

	if (_impl->resized)
		rebuild_cb();

	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];
	ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, &_impl->pools[_impl->frame_index]);

	ZHLN_AcquireDesc acq = {_impl->swapchain.Get().handle, s.image_available, UINT64_MAX};
	if (ZHLN_AcquireImage(_impl->ctx.Device(), &acq, &_impl->current_image_index) ==
		ZHLN_FrameResult_OutOfDate) {
		rebuild_cb();
		return;
	}

	_impl->current_cmd = _impl->pools.Cmd(_impl->frame_index);
	ZHLN_BeginCommandBuffer(_impl->current_cmd);

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		_impl->current_cmd, _impl->swapchain.Get().images[_impl->current_image_index]);

	// Transition Depth
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
		_impl->current_cmd, _impl->depth_image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
}

void RenderContext::EndFrame() {
	if (_impl->current_cmd == VK_NULL_HANDLE)
		return;

	ZHLN_EndRendering(_impl->current_cmd);
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(
		_impl->current_cmd, _impl->swapchain.Get().images[_impl->current_image_index]);
	ZHLN_EndCommandBuffer(_impl->current_cmd);

	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];
	VkCommandBufferSubmitInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
										  .commandBuffer = _impl->current_cmd};
	VkSemaphoreSubmitInfo wait_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
									   .semaphore = s.image_available,
									   .stageMask =
										   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
	VkSemaphoreSubmitInfo signal_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
										 .semaphore =
											 _impl->present_semaphores[_impl->current_image_index],
										 .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT};

	VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
							.waitSemaphoreInfoCount = 1,
							.pWaitSemaphoreInfos = &wait_info,
							.commandBufferInfoCount = 1,
							.pCommandBufferInfos = &cmd_info,
							.signalSemaphoreInfoCount = 1,
							.pSignalSemaphoreInfos = &signal_info};
	vkQueueSubmit2(_impl->ctx.GraphicsQueue(), 1, &submit, s.in_flight);

	ZHLN_PresentDesc pres = {.present_queue = _impl->ctx.PresentQueue(),
							 .swapchain = _impl->swapchain.Get().handle,
							 .render_finished =
								 _impl->present_semaphores[_impl->current_image_index],
							 .image_index = _impl->current_image_index};
	if (ZHLN_PresentFrame(&pres) != ZHLN_FrameResult_Ok)
		_impl->resized = true;

	_impl->frame_index = (_impl->frame_index + 1) % 2;
	_impl->current_cmd = VK_NULL_HANDLE;
}

namespace Renderer {
void Clear(RenderContext& ctx, const JPH::Vec4& color, float depth) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE)
		return;

	ZHLN_RenderPassDesc pass = {
		.target_view = impl->swapchain.Get().views[impl->current_image_index],
		.depth_view = impl->depth_view, // BIND DEPTH VIEW
		.extent = impl->swapchain.Get().extent,
		.clear_color = {color.GetX(), color.GetY(), color.GetZ(), color.GetW()},
		.clear_depth = depth};
	ZHLN_BeginRendering(impl->current_cmd, &pass);
}

void UpdateBuffer(RenderContext& ctx, BufferHandle buffer, const void* data, size_t size) {
	if (size == sizeof(JPH::Mat44))
		ctx.GetImpl()->current_view_proj = *static_cast<const JPH::Mat44*>(data);
}

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const JPH::Mat44& transform) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE)
		return;

	auto* mat = reinterpret_cast<NativeMaterial*>(static_cast<uint64_t>(material.pipeline));
	auto* msh = reinterpret_cast<NativeMesh*>(static_cast<uint64_t>(mesh.vertexBuffer));

	vkCmdBindPipeline(impl->current_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mat->pipeline.Get());

	VkDeviceSize offset = 0;
	VkBuffer buf = msh->buffer.Handle();
	vkCmdBindVertexBuffers(impl->current_cmd, 0, 1, &buf, &offset);

	JPH::Mat44 final_transform = impl->current_view_proj * transform;
	vkCmdPushConstants(impl->current_cmd, mat->layout.Get(), VK_SHADER_STAGE_VERTEX_BIT, 0,
					   sizeof(JPH::Mat44), &final_transform);

	vkCmdDraw(impl->current_cmd, msh->vertexCount, 1, 0, 0);
}
} // namespace Renderer

} // namespace ZHLN