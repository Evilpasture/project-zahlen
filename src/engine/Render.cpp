#include "Allocator.hpp"
#include "RenderCore.hpp"

#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <vector>

namespace ZHLN {

// Define internal structs to hide Vulkan types from the header
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

	uint32_t frame_index = 0;
	bool resized = true; // Force initial rebuild

	// Active Frame State
	VkCommandBuffer current_cmd = VK_NULL_HANDLE;
	uint32_t current_image_index = 0;

	JPH::Mat44 current_view_proj;

	// Resource Storage (Simplistic deletion tracking for now)
	std::vector<std::unique_ptr<NativeMesh>> meshes;
	std::vector<std::unique_ptr<NativeMaterial>> materials;

	Impl(Window& win) : window(win) {}
};

RenderContext::RenderContext(Window& window, const String32& preferredAPI)
	: _impl(std::make_unique<Impl>(window)) {
	// 1. Get extensions required by GLFW
	uint32_t glfwExtensionCount = 0;
	const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

	// Merge with our required extensions
	std::vector<const char*> inst_exts(glfwExtensions, glfwExtensions + glfwExtensionCount);
	inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	inst_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
	inst_exts.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

	ZHLN_InstanceDesc inst_desc = ZHLN_DEFAULT_INSTANCE_DESC;
	inst_desc.extensions = inst_exts.data();
	inst_desc.extension_count = static_cast<uint32_t>(inst_exts.size());

	// 2. Setup Device Features
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swap_maint = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
		.pNext = nullptr,
		.swapchainMaintenance1 = VK_TRUE};
	VkPhysicalDeviceVulkan13Features feat13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.pNext = &swap_maint,
		.synchronization2 = VK_TRUE,
		.dynamicRendering = VK_TRUE};

	VkPhysicalDeviceVulkan12Features feat12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &feat13,
		.bufferDeviceAddress = VK_TRUE};

	VkPhysicalDeviceFeatures2 feat2 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &feat12, .features = {}};

	const char* dev_exts[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME // Anti-overlay protection
	};

	ZHLN_DeviceDesc dev_desc = {.physical = nullptr, // Anchor
								.extensions = dev_exts,
								.extension_count = 3,
								.features = &feat2,
								.enable_validation = true};

	// 3. Create Context
	_impl->ctx = Vk::Context::Create(inst_desc, {VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, nullptr},
									 dev_desc);
	if (!_impl->ctx) {
		ZHLN::Log("FATAL: Vulkan Context Creation Failed\n");
		std::abort();
	}

	// 4. Create Surface via GLFW
	GLFWwindow* glfwWin = static_cast<GLFWwindow*>(window.GetNativeHandle());
	VkSurfaceKHR raw_surface;
	if (glfwCreateWindowSurface(_impl->ctx.Instance(), glfwWin, nullptr, &raw_surface) !=
		VK_SUCCESS) {
		std::abort();
	}
	_impl->surface = Vk::Surface(_impl->ctx.Instance(), raw_surface);

	// 5. Initialize Subsystems
	if (!_impl->allocator.Init(_impl->ctx)) {
		ZHLN::Log("FATAL: Vulkan Memory Allocator (VMA) failed to initialize\n");
		std::abort();
	}

	_impl->swapchain = Vk::Swapchain(_impl->ctx.Device(), {});

	_impl->sync = Vk::FrameSync<2>::Create(_impl->ctx.Device());
	if (!_impl->sync.Valid()) {
		ZHLN::Log("FATAL: Failed to create Vulkan Frame Sync primitives\n");
		std::abort();
	}

	_impl->pools =
		Vk::CommandPools<2>::Create(_impl->ctx.Device(), _impl->ctx.PhysicalInfo().graphics_family);
	if (!_impl->pools.Valid()) {
		ZHLN::Log("FATAL: Failed to create Vulkan Command Pools\n");
		std::abort();
	}

	_impl->present_semaphores = Vk::SemaphorePool(); // Initial state is fine
}

RenderContext::~RenderContext() {
	if (_impl->ctx.Device()) {
		vkDeviceWaitIdle(_impl->ctx.Device());
	}
}

const char* RenderContext::GetRendererName() const {
	return "Vulkan 1.3 (ZHLN)";
}

void RenderContext::SetResolution(const Extent2D& res) {
	_impl->resized = true;
}

// ----------------------------------------------------------------------------
// Resource Creation
// ----------------------------------------------------------------------------

BufferHandle RenderContext::CreateVertexBuffer(const void* data, size_t size) {
	// 1. Allocate GPU memory
	auto gpu_buf =
		Vk::Buffer::Create(_impl->allocator.Get(), size,
						   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
						   VMA_MEMORY_USAGE_GPU_ONLY);

	// 2. Perform Immediate Upload (Simplistic for now, using a temporary command buffer)
	VkCommandBuffer cmd = _impl->pools.Cmd(0); // Borrow a cmd buffer temporarily
	ZHLN_BeginCommandBuffer(cmd);
	auto staging = Vk::UploadToBuffer(_impl->allocator.Get(), cmd, gpu_buf, data, size);
	ZHLN_EndCommandBuffer(cmd);

	// Force wait for upload to complete (Slow but works for initial setup)
	VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
						   .pNext = nullptr,
						   .waitSemaphoreCount = 0,
						   .pWaitSemaphores = nullptr,
						   .pWaitDstStageMask = nullptr,
						   .commandBufferCount = 1,
						   .pCommandBuffers = &cmd,
						   .signalSemaphoreCount = 0,
						   .pSignalSemaphores = nullptr};
	vkQueueSubmit(_impl->ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(_impl->ctx.GraphicsQueue());

	auto* mesh = new NativeMesh{std::move(gpu_buf), 0};
	_impl->meshes.emplace_back(mesh);
	return static_cast<BufferHandle>(reinterpret_cast<uint64_t>(mesh));
}

BufferHandle RenderContext::CreateConstantBuffer(size_t size) {
	// We are migrating to Push Constants, so we don't strictly need this right now.
	return BufferHandle::Invalid;
}

Material RenderContext::CreateMaterial(const PipelineDesc& desc) {
	ZHLN_ShaderDesc v_desc = {static_cast<const uint32_t*>(desc.vertexShaderData),
							  desc.vertexShaderSize};
	ZHLN_ShaderDesc f_desc = {static_cast<const uint32_t*>(desc.fragShaderData),
							  desc.fragShaderSize};
	auto shaders = Vk::ShaderStages::Create(_impl->ctx.Device(), v_desc, f_desc);

	// Only 64 bytes needed since we multiply on the CPU
	VkPushConstantRange pc_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(JPH::Mat44)};

	ZHLN_PipelineLayoutDesc layout_desc = {.set_layouts = nullptr,
										   .set_layout_count = 0,
										   .push_constants = &pc_range,
										   .push_constant_count = 1};
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

	ZHLN_GraphicsPipelineDesc pipe_desc = {
		.stages = const_cast<ZHLN_ShaderStages*>(shaders.Get()),
		.layout = layout.Get(),
		.vertex_binding_count = 1,
		.vertex_bindings = &binding,
		.vertex_attribute_count = 2,
		.vertex_attributes = attrs,
		.color_format = VK_FORMAT_B8G8R8A8_SRGB,
		.depth_format = VK_FORMAT_UNDEFINED,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, // Added
		.polygon_mode = VK_POLYGON_MODE_FILL,			 // Added
		.cull_mode = VK_CULL_MODE_BACK_BIT,
		.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE, // Added
		.depth_test = false,						   // Added
		.depth_write = false,						   // Added
		.blend_enable = false						   // Added
	};
	auto pipeline = Vk::Pipeline(_impl->ctx.Device(),
								 ZHLN_CreateGraphicsPipeline(_impl->ctx.Device(), &pipe_desc));

	auto* mat = new NativeMaterial{std::move(pipeline), std::move(layout)};
	_impl->materials.emplace_back(mat);

	Material m;
	m.pipeline = static_cast<PipelineHandle>(reinterpret_cast<uint64_t>(mat));
	return m;
}

// ----------------------------------------------------------------------------
// Frame Execution
// ----------------------------------------------------------------------------

void RenderContext::BeginFrame() {
	auto rebuild_cb = [&]() {
		vkDeviceWaitIdle(_impl->ctx.Device());

		// 1. Get the actual window size from the OS/GLFW
		int w, h;
		GLFWwindow* glfwWin = static_cast<GLFWwindow*>(_impl->window.GetNativeHandle());
		glfwGetFramebufferSize(glfwWin, &w, &h);

		ZHLN_Device raw_dev = {_impl->ctx.Device(), _impl->ctx.GraphicsQueue(),
							   _impl->ctx.PresentQueue()};
		ZHLN_PhysicalDeviceInfo raw_phys = _impl->ctx.PhysicalInfo();

		ZHLN_SwapchainSupportDesc sdesc = {raw_phys.handle, _impl->surface.Get()};
		auto support = ZHLN_QuerySwapchainSupport(&sdesc);
		auto& caps = support.capabilities;

		// 2. This handles the 0xFFFFFFFF case where Vulkan doesn't know the size yet.
		uint32_t final_w, final_h;
		if (caps.currentExtent.width != 0xFFFFFFFF) {
			final_w = caps.currentExtent.width;
			final_h = caps.currentExtent.height;
		} else {
			final_w = JPH::Clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width);
			final_h =
				JPH::Clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height);
		}

		ZHLN_SwapchainDesc s_desc = {.device = &raw_dev,
									 .physical = &raw_phys,
									 .surface = _impl->surface.Get(),
									 .width = final_w,
									 .height = final_h,
									 .vsync = true,
									 .old_swapchain = VK_NULL_HANDLE};

		_impl->swapchain.Rebuild(s_desc);
		_impl->present_semaphores.Rebuild(_impl->ctx.Device(), _impl->swapchain.Get().image_count);
		_impl->resized = false;
	};

	if (_impl->resized)
		rebuild_cb();

	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];
	ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, &_impl->pools[_impl->frame_index]);

	ZHLN_AcquireDesc acq = {_impl->swapchain.Get().handle, s.image_available, UINT64_MAX};
	auto res = ZHLN_AcquireImage(_impl->ctx.Device(), &acq, &_impl->current_image_index);
	if (res == ZHLN_FrameResult_OutOfDate) {
		rebuild_cb();
		return; // Skip this frame
	}

	_impl->current_cmd = _impl->pools.Cmd(_impl->frame_index);
	ZHLN_BeginCommandBuffer(_impl->current_cmd);

	// Transition Swapchain Image
	VkImage img = _impl->swapchain.Get().images[_impl->current_image_index];
	Vk::TransitionLayout(_impl->current_cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
						 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void RenderContext::EndFrame() {
	if (_impl->current_cmd == VK_NULL_HANDLE)
		return; // Skipped frame

	// MUST End Rendering before inserting image barriers!
	ZHLN_EndRendering(_impl->current_cmd);

	// Transition Swapchain Image for Presentation
	VkImage img = _impl->swapchain.Get().images[_impl->current_image_index];
	Vk::TransitionLayout(_impl->current_cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	ZHLN_EndCommandBuffer(_impl->current_cmd);

	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];

	VkCommandBufferSubmitInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
										  .pNext = nullptr,
										  .commandBuffer = _impl->current_cmd,
										  .deviceMask = 0};
	VkSemaphoreSubmitInfo wait_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
									   .pNext = nullptr,
									   .semaphore = s.image_available,
									   .value = 0,
									   .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
									   .deviceIndex = 0};
	VkSemaphoreSubmitInfo signal_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
										 .pNext = nullptr,
										 .semaphore =
											 _impl->present_semaphores[_impl->current_image_index],
										 .value = 0,
										 .stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
										 .deviceIndex = 0};

	VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
							.pNext = nullptr,
							.flags = 0,
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

	if (ZHLN_PresentFrame(&pres) != ZHLN_FrameResult_Ok) {
		_impl->resized = true;
	}

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
		.depth_view = VK_NULL_HANDLE,
		.extent = impl->swapchain.Get().extent,
		.clear_color = {color.GetX(), color.GetY(), color.GetZ(), color.GetW()},
		.clear_depth = 1.0f // Added
	};

	// Note: For actual drawing, you need to BeginRendering, Draw, EndRendering.
	// If you just want to clear, doing Begin/End rendering with CLEAR ops is the Vulkan 1.3 way.
	ZHLN_BeginRendering(impl->current_cmd, &pass);
	// We will leave the RenderPass open here so Draw() can append commands.
	// In a real engine, Clear/Draw flow needs careful management of BeginRendering/EndRendering.
}

void UpdateBuffer(RenderContext& ctx, BufferHandle buffer, const void* data, size_t size) {
	// Intercept the matrix upload and cache it (Simulating the Constant Buffer)
	if (size == sizeof(JPH::Mat44)) {
		ctx.GetImpl()->current_view_proj = *static_cast<const JPH::Mat44*>(data);
	}
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

	// Multiply the cached ViewProj with the object's Model transform
	JPH::Mat44 final_transform = impl->current_view_proj * transform;
	vkCmdPushConstants(impl->current_cmd, mat->layout.Get(), VK_SHADER_STAGE_VERTEX_BIT, 0,
					   sizeof(JPH::Mat44), &final_transform);

	vkCmdDraw(impl->current_cmd, msh->vertexCount, 1, 0, 0);
}
} // namespace Renderer

} // namespace ZHLN