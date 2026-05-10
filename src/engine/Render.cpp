#include "Allocator.hpp"
#include "PipelineBuilder.hpp"
#include "PresentationContext.hpp"
#include "RenderCore.hpp"
#include "RenderGraph.hpp"
#include "RenderTarget.hpp"
#include "Vertex.hpp"
#include "imgui_impl_glfw.h"

#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <vector>

namespace ZHLN::Vk {
// Map Engine Semantic Types -> Hardware Vulkan Formats
template <> struct FormatOf<float[3]> {
	static constexpr auto value = VK_FORMAT_R32G32B32_SFLOAT;
};
template <> struct FormatOf<::ZHLN::Packed1010102> {
	static constexpr auto value = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
};
template <> struct FormatOf<::ZHLN::PackedHalf2> {
	static constexpr auto value = VK_FORMAT_R16G16_SFLOAT;
};
template <> struct FormatOf<::ZHLN::PackedRGBA8> {
	static constexpr auto value = VK_FORMAT_R8G8B8A8_UNORM;
};
} // namespace ZHLN::Vk

namespace ZHLN {

// Reflect the engine's Vertex type for the PipelineBuilder (Skip _padding)
ZHLN_REFLECT_VERTEX(::ZHLN::Vertex, position, normal, tangent, uv, color);

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

	// New Abstraction: Orchestrates Swapchain + Depth + Semaphores
	Vk::PresentationContext presentation;

	Vk::FrameSync<2> sync;
	Vk::CommandPools<2> pools;

	uint32_t frame_index = 0;
	bool resized = true;

	VkCommandBuffer current_cmd = VK_NULL_HANDLE;
	uint32_t current_image_index = 0;

	JPH::Mat44 current_view_proj;

	std::vector<std::unique_ptr<NativeMesh>> meshes;
	std::vector<std::unique_ptr<NativeMaterial>> materials;

	Impl(Window& win) : window(win) {}
	bool depth_ready = false;
	VkDescriptorPool uiPool = VK_NULL_HANDLE;

	void SetupUI(GLFWwindow* window) {
		// 1. Create a robust pool for ImGui.
		// We use the "Kitchen Sink" approach because different versions of ImGui
		// and different platforms may request different descriptor types.
		const VkDescriptorPoolSize pool_sizes[] = {
			{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
			{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
			{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
			{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}};

		const VkDescriptorPoolCreateInfo pool_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
			.maxSets = 1000,
			.poolSizeCount = (uint32_t)std::size(pool_sizes),
			.pPoolSizes = pool_sizes,
		};

		if (vkCreateDescriptorPool(ctx.Device(), &pool_info, nullptr, &uiPool) != VK_SUCCESS) {
			ZHLN::Panic("FATAL: Failed to create ImGui Descriptor Pool");
		}

		// 2. Standard ImGui Init
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui_ImplGlfw_InitForVulkan(window, true);

		// 3. Init Vulkan for Dynamic Rendering (Vulkan 1.3)
		VkFormat swapchainFormat = presentation.swapchain.Get().format;

		ImGui_ImplVulkan_InitInfo init_info = {
			.ApiVersion = VK_API_VERSION_1_3,
			.Instance = ctx.Instance(),
			.PhysicalDevice = ctx.Physical(),
			.Device = ctx.Device(),
			.QueueFamily = ctx.PhysicalInfo().graphics_family,
			.Queue = ctx.GraphicsQueue(),
			.DescriptorPool = uiPool,
			.MinImageCount = 2,
			.ImageCount = 2,
			.PipelineInfoMain =
				{
					.MSAASamples = VK_SAMPLE_COUNT_1_BIT,
					.PipelineRenderingCreateInfo =
						{
							.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
							.colorAttachmentCount = 1,
							.pColorAttachmentFormats = &swapchainFormat,
							.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
						},
				},
			.UseDynamicRendering = true,
		};
		ImGui_ImplVulkan_Init(&init_info);
	}
};

RenderContext::RenderContext(Window& window) : _impl(std::make_unique<Impl>(window)) {
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

	// Setup features using Vulkan 1.3 standards
	VkPhysicalDeviceVulkan13Features feat13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
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
		VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, "VK_KHR_portability_subset"};
#else
	const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
							  VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME};
#endif

	ZHLN_DeviceDesc dev_desc = {.physical = nullptr,
								.extensions = dev_exts,
								.extension_count =
									(uint32_t)(sizeof(dev_exts) / sizeof(const char*)),
								.features = &feat2,
								.enable_validation = true};

	ZHLN_DeviceSelectDesc select_desc = {.instance = VK_NULL_HANDLE,
										 .surface = VK_NULL_HANDLE,
										 .score_fn = nullptr,
										 .score_userdata = nullptr};

	_impl->ctx = Vk::Context::Create(inst_desc, select_desc, dev_desc);
	if (!_impl->ctx) {
		ZHLN::Panic("FATAL: Vulkan Context Creation Failed");
	}

	GLFWwindow* glfwWin = static_cast<GLFWwindow*>(window.GetNativeHandle());
	VkSurfaceKHR raw_surface;
	glfwCreateWindowSurface(_impl->ctx.Instance(), glfwWin, nullptr, &raw_surface);
	_impl->surface = Vk::Surface(_impl->ctx.Instance(), raw_surface);

	if (!_impl->allocator.Init(_impl->ctx)) {
		ZHLN::Panic("FATAL: Vulkan Memory Allocator (VMA) failed to initialize");
	}

	// Initialize presentation with current window size
	int w, h;
	glfwGetFramebufferSize(glfwWin, &w, &h);
	if (!_impl->presentation.Init(_impl->ctx, _impl->allocator, _impl->surface.Get(), (uint32_t)w,
								  (uint32_t)h)) {
		ZHLN::Panic("FATAL: Presentation Context initialization failed");
	}

	_impl->sync = Vk::FrameSync<2>::Create(_impl->ctx.Device());
	_impl->pools =
		Vk::CommandPools<2>::Create(_impl->ctx.Device(), _impl->ctx.PhysicalInfo().graphics_family);
	_impl->SetupUI(static_cast<GLFWwindow*>(window.GetNativeHandle()));
}

RenderContext::~RenderContext() {
	if (_impl->ctx.Device()) {
		vkDeviceWaitIdle(_impl->ctx.Device());
		_impl->meshes.clear();
		_impl->materials.clear();
		// PresentationContext, Sync, Pools, Allocator, and Context all self-destruct via RAII
	}
}

const char* RenderContext::GetRendererName() const {
	return "ZHLN (Modernized)";
}

void RenderContext::SetResolution([[maybe_unused]] const Extent2D& res) {
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

	auto mesh = std::make_unique<NativeMesh>(std::move(gpu_buf),
											 static_cast<uint32_t>(size / sizeof(Vertex)));
	auto handle = static_cast<BufferHandle>(reinterpret_cast<uintptr_t>(mesh.get()));
	_impl->meshes.push_back(std::move(mesh));
	return handle;
}

Material RenderContext::CreateMaterial(const PipelineDesc& desc) {
	// 1. Setup Shaders
	ZHLN_ShaderDesc v_desc = {(const uint32_t*)desc.vertexShaderData, desc.vertexShaderSize,
							  nullptr};
	ZHLN_ShaderDesc f_desc = {(const uint32_t*)desc.fragShaderData, desc.fragShaderSize, nullptr};
	auto shaders = Vk::ShaderStages::Create(_impl->ctx.Device(), v_desc, f_desc);

	// 2. Setup Layout
	VkPushConstantRange pc_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT, .offset = 0, .size = sizeof(JPH::Mat44)};
	ZHLN_PipelineLayoutDesc layout_desc = {.push_constants = &pc_range, .push_constant_count = 1};
	auto layout = Vk::PipelineLayout(_impl->ctx.Device(),
									 ZHLN_CreatePipelineLayout(_impl->ctx.Device(), &layout_desc));

	// 3. Use the new PipelineBuilder
	auto pipeline = Vk::PipelineBuilder{}
						.Shaders(shaders)
						.Layout(layout.Get())
						.Vertex<Vertex>() // Uses the ZHLN_REFLECT_VERTEX we defined at the top
						.ColorFormat(VK_FORMAT_B8G8R8A8_SRGB)
						.DepthFormat(VK_FORMAT_D32_SFLOAT)
						.Build(_impl->ctx.Device());

	auto mat = std::make_unique<NativeMaterial>(std::move(pipeline), std::move(layout));
	auto handle = static_cast<PipelineHandle>(reinterpret_cast<uintptr_t>(mat.get()));
	_impl->materials.push_back(std::move(mat));
	return Material{.pipeline = handle};
}

void RenderContext::BeginFrame() {
	if (_impl->resized) {
		int w, h;
		glfwGetFramebufferSize(static_cast<GLFWwindow*>(_impl->window.GetNativeHandle()), &w, &h);
		if (!_impl->presentation.Rebuild((uint32_t)w, (uint32_t)h)) {
			ZHLN::Panic("FATAL: Failed to rebuild swapchain/depth during resize");
		}
		_impl->depth_ready = false;
		_impl->resized = false;
	}

	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];
	ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, &_impl->pools[_impl->frame_index]);

	ZHLN_AcquireDesc acq = {_impl->presentation.swapchain.Get().handle, s.image_available,
							UINT64_MAX};
	if (ZHLN_AcquireImage(_impl->ctx.Device(), &acq, &_impl->current_image_index) ==
		ZHLN_FrameResult_OutOfDate) {
		_impl->resized = true;
		return;
	}

	_impl->current_cmd = _impl->pools.Cmd(_impl->frame_index);
	ZHLN_BeginCommandBuffer(_impl->current_cmd);
}

void RenderContext::EndFrame() {
	if (_impl->current_cmd == VK_NULL_HANDLE)
		return;

	// 1. Finalize engine rendering
	ZHLN_EndRendering(_impl->current_cmd);

	// 2. Render ImGui with LOAD_OP_LOAD
	ImGui::Render();

	const VkRenderingAttachmentInfo colorAttachment = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.imageView = _impl->presentation.swapchain.Get().views[_impl->current_image_index],
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, // <--- IMPORTANT: KEEP THE SCENE!
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	};

	const VkRenderingInfo renderingInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.renderArea = {{0, 0}, _impl->presentation.swapchain.Get().extent},
		.layerCount = 1,
		.colorAttachmentCount = 1,
		.pColorAttachments = &colorAttachment,
	};

	vkCmdBeginRendering(_impl->current_cmd, &renderingInfo);
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), _impl->current_cmd);
	vkCmdEndRendering(_impl->current_cmd);

	// 3. Normal transition to PRESENT
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(
		_impl->current_cmd, _impl->presentation.swapchain.Get().images[_impl->current_image_index]);

	ZHLN_EndCommandBuffer(_impl->current_cmd);

	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];

	ZHLN_FrameSubmitDesc submitDesc = {
		.graphicsQueue = _impl->ctx.GraphicsQueue(),
		.presentQueue = _impl->ctx.PresentQueue(),
		.cmd = _impl->current_cmd,
		.imageAvailable = s.image_available,
		.renderFinished = _impl->presentation.presentSemaphores[_impl->current_image_index],
		.inFlight = s.in_flight,
		.swapchain = _impl->presentation.swapchain.Get().handle,
		.imageIndex = _impl->current_image_index};

	if (Vk::SubmitAndPresent(submitDesc) != ZHLN_FrameResult_Ok) {
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

	// --- COLOR SYNC ---
	// Transition swapchain from UNDEFINED to COLOR_ATTACHMENT
	// Note: We use UNDEFINED -> COLOR because we are about to CLEAR it anyway.
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		impl->current_cmd, impl->presentation.swapchain.Get().images[impl->current_image_index]);

	// --- DEPTH SYNC ---
	if (!impl->depth_ready) {
		// FIRST TIME: Transition from UNDEFINED to DEPTH_OPTIMAL
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			impl->current_cmd, impl->presentation.depthTarget.image.Handle(),
			VK_IMAGE_ASPECT_DEPTH_BIT);
		impl->depth_ready = true;
	} else {
		// SUBSEQUENT FRAMES: Transition from DEPTH_OPTIMAL to DEPTH_OPTIMAL
		// This acts as a Execution Barrier (Wait for previous frame's depth write to finish)
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			impl->current_cmd, impl->presentation.depthTarget.image.Handle(),
			VK_IMAGE_ASPECT_DEPTH_BIT);
	}

	ZHLN_RenderPassDesc pass = {
		.target_view = impl->presentation.swapchain.Get().views[impl->current_image_index],
		.depth_view = impl->presentation.depthTarget.view.Get(),
		.extent = impl->presentation.swapchain.Get().extent,
		.clear_color = {color.GetX(), color.GetY(), color.GetZ(), color.GetW()},
		.clear_depth = depth};

	ZHLN_BeginRendering(impl->current_cmd, &pass);
}

void UpdateBuffer(RenderContext& ctx, [[maybe_unused]] BufferHandle buffer, const void* data,
				  size_t size) {
	if (size == sizeof(JPH::Mat44))
		ctx.GetImpl()->current_view_proj = *static_cast<const JPH::Mat44*>(data);
}

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const JPH::Mat44& transform) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE)
		return;

	auto* mat = reinterpret_cast<NativeMaterial*>(material.pipeline);
	auto* msh = reinterpret_cast<NativeMesh*>(mesh.vertexBuffer);

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