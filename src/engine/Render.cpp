#include "Allocator.hpp"
#include "DescriptorLayout.hpp"
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

// NEW: Threading Support
#include <atomic>
#include <threading/TaskSystem.hpp>

namespace ZHLN::Vk {
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

ZHLN_REFLECT_VERTEX(::ZHLN::Vertex, position, normal, tangent, uv, color);

using GlobalBindlessLayout =
	Vk::DescriptorLayout<Vk::BindlessSampledImageSlot<0, 4096>, Vk::SamplerSlot<1>>;

struct NativeMesh {
	Vk::Buffer buffer;
	uint32_t vertexCount;
};

struct NativeMaterial {
	Vk::Pipeline pipeline;
	Vk::PipelineLayout layout;
};

// NEW: Internal Draw Command struct to queue up draw calls
struct DrawCommand {
	NativeMaterial* material;
	NativeMesh* mesh;
	JPH::Mat44 transform;
	uint32_t textureIndex;
};

// NEW: Worker Pool state for Secondary Command Buffers
struct WorkerCmdContext {
	Vk::CommandPool pools[2]; // One for each frame in flight
	ZHLN::Atomic<uint32_t> cmdCount[2];
};

struct RenderContext::Impl {
	Window& window;
	Vk::Context ctx;
	Vk::Allocator allocator;
	Vk::Surface surface;

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

	// NEW: Threading data
	std::vector<DrawCommand> drawQueue;
	std::vector<WorkerCmdContext> workerCmds;

	Impl(Window& win) : window(win) {}
	bool depth_ready = false;
	VkDescriptorPool uiPool = VK_NULL_HANDLE;

	Vk::DescriptorSetLayout bindlessLayout;
	Vk::DescriptorPool bindlessPool;
	VkDescriptorSet bindlessSet = VK_NULL_HANDLE;
	Vk::Sampler globalSampler;

	uint32_t nextTextureIndex = 0;
	std::vector<Vk::Image> textureImages;
	std::vector<Vk::ImageView> textureViews;

	void InitBindless() {
		bindlessLayout = GlobalBindlessLayout::CreateLayout(ctx.Device());
		bindlessPool = GlobalBindlessLayout::CreatePool(ctx.Device(), 1);
		bindlessSet =
			GlobalBindlessLayout::Allocate(ctx.Device(), bindlessPool.Get(), bindlessLayout.Get());

		VkSamplerCreateInfo samplerInfo = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.anisotropyEnable = VK_TRUE;
		samplerInfo.maxAnisotropy =
			ctx.PhysicalInfo().properties.properties.limits.maxSamplerAnisotropy;

		VkSampler rawSampler;
		if (vkCreateSampler(ctx.Device(), &samplerInfo, nullptr, &rawSampler) != VK_SUCCESS) {
			ZHLN::Panic("Failed to create Global Sampler");
		}
		globalSampler = Vk::Sampler(ctx.Device(), rawSampler);

		GlobalBindlessLayout::Write(ctx.Device(), bindlessSet, Vk::SkipWrite{},
									Vk::SamplerWrite{globalSampler.Get()});
	}

	void SetupUI(GLFWwindow* window) {
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

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui_ImplGlfw_InitForVulkan(window, true);

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
		// ENABLE BINDLESS FEATURES
		.descriptorIndexing = VK_TRUE,
		.shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
		.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
		.descriptorBindingPartiallyBound = VK_TRUE,
		.runtimeDescriptorArray = VK_TRUE,
		.bufferDeviceAddress = VK_TRUE};

	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swap_maint = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
		.pNext = nullptr,
		.swapchainMaintenance1 = VK_TRUE};
	feat13.pNext = &swap_maint;

	VkPhysicalDeviceFeatures2 feat2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
									   .pNext = &feat12,
									   .features = {.samplerAnisotropy = VK_TRUE}};

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
	_impl->InitBindless();

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

	// NEW: Initialize Worker pools based on the detected CPU threads
	uint32_t workerCount = TaskSystem::GetWorkerCount();
	// Fallback if TaskSystem wasn't initialized yet
	if (workerCount == 0) {
		ZHLN::Log(
			"WARNING: RenderContext initialized before TaskSystem. Falling back to 1 worker.");
		workerCount = 1;
	}

	_impl->workerCmds.resize(workerCount);
	for (auto& w : _impl->workerCmds) {
		for (int i = 0; i < 2; ++i) {
			w.pools[i] =
				Vk::CommandPool(_impl->ctx.Device(), _impl->ctx.PhysicalInfo().graphics_family);
			// Pre-allocate secondary command buffers for chunking
			if (!w.pools[i].AllocateSecondary(256)) {
				ZHLN::Panic(
					"FATAL: Failed to pre-allocate secondary command buffers for worker threads. "
					"Check if your GPU supports {} command buffers per pool.",
					256);
			}
		}
	}
}

RenderContext::~RenderContext() {
	if (_impl && _impl->ctx.Device()) {
		vkDeviceWaitIdle(_impl->ctx.Device());

		// 1. Shut down ImGui (Frees the leaking buffers/images/fonts)
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();

		// 2. Destroy the raw Descriptor Pool we made for the UI
		if (_impl->uiPool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(_impl->ctx.Device(), _impl->uiPool, nullptr);
			_impl->uiPool = VK_NULL_HANDLE;
		}

		// 3. Clear engine resources
		_impl->meshes.clear();
		_impl->materials.clear();

		// C++ unique_ptr will now safely destroy the rest of the RAII members
		// (allocator, bindless pool, presentation, device, instance) automatically.
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
	auto* impl = _impl.get();

	// Ensure layout is valid
	if (!impl->bindlessLayout.Valid()) {
		ZHLN::Panic("Attempted to create material before Bindless was initialized!");
	}

	// 1. Define Push Constant Range using the Struct
	VkPushConstantRange pc_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(ZHLN::FrameConstants) // No more manual math!
	};

	// 2. Use the valid Bindless Layout
	VkDescriptorSetLayout layouts[] = {impl->bindlessLayout.Get()};
	ZHLN_PipelineLayoutDesc layout_desc = {.set_layouts = layouts,
										   .set_layout_count = 1,
										   .push_constants = &pc_range,
										   .push_constant_count = 1};

	auto layout = Vk::PipelineLayout(impl->ctx.Device(),
									 ZHLN_CreatePipelineLayout(impl->ctx.Device(), &layout_desc));

	// 3. Build Pipeline (existing code)
	auto pipeline = Vk::PipelineBuilder{}
						.Shaders(shaders)
						.Layout(layout.Get())
						.Vertex<Vertex>()
						.ColorFormat(VK_FORMAT_B8G8R8A8_SRGB)
						.DepthFormat(VK_FORMAT_D32_SFLOAT)
						.Build(impl->ctx.Device());

	auto mat = std::make_unique<NativeMaterial>(std::move(pipeline), std::move(layout));
	auto handle = static_cast<PipelineHandle>(reinterpret_cast<uintptr_t>(mat.get()));
	impl->materials.push_back(std::move(mat));

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

	for (auto& w : _impl->workerCmds) {
		w.cmdCount[_impl->frame_index].store(0, std::memory_order_relaxed);
	}

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

	// ========================================================================
	// NEW: Parallel Rendering Dispatch
	// ========================================================================
	if (!_impl->drawQueue.empty()) {
		uint32_t numChunks = (_impl->drawQueue.size() + 255) / 256;
		std::vector<VkCommandBuffer> secondaries(numChunks);

		ZHLN_SecondaryCmdDesc secDesc = {.color_format = _impl->presentation.swapchain.Get().format,
										 .depth_format = VK_FORMAT_D32_SFLOAT};

		TaskSystem::ParallelFor(
			_impl->drawQueue.size(), 256, [&](uint32_t start, uint32_t end, uint32_t chunkIdx) {
				uint32_t wIdx = TaskSystem::GetWorkerIndex();

				// Atomically grab a secondary command buffer from THIS thread's pool
				uint32_t localCmdIdx =
					_impl->workerCmds[wIdx].cmdCount[_impl->frame_index].fetch_add(
						1, std::memory_order_relaxed);
				VkCommandBuffer sec_cmd =
					_impl->workerCmds[wIdx].pools[_impl->frame_index][localCmdIdx];

				ZHLN_BeginSecondaryCommandBuffer(sec_cmd, &secDesc);

				// ================================================================
				// SETUP DYNAMIC STATE FOR SECONDARY BUFFER
				// ================================================================
				VkExtent2D extent = _impl->presentation.swapchain.Get().extent;
				const VkViewport viewport = {
					.x = 0.0f,
					.y = (float)extent.height, // Start at bottom
					.width = (float)extent.width,
					.height = -(float)extent.height, // Negative height for Y-flip
					.minDepth = 0.0f,
					.maxDepth = 1.0f,
				};
				const VkRect2D scissor = {{0, 0}, extent};

				vkCmdSetViewport(sec_cmd, 0, 1, &viewport);
				vkCmdSetScissor(sec_cmd, 0, 1, &scissor);

				// Record this chunk's draw calls
				for (uint32_t i = start; i < end; ++i) {
					const auto& cmd = _impl->drawQueue[i];
					vkCmdBindPipeline(sec_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
									  cmd.material->pipeline.Get());
					vkCmdBindDescriptorSets(sec_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
											cmd.material->layout.Get(), 0, 1, &_impl->bindlessSet,
											0, nullptr);

					JPH::Mat44 final_transform = _impl->current_view_proj * cmd.transform;
					FrameConstants constants = {.transform = final_transform,
												.textureIndex = cmd.textureIndex};

					vkCmdPushConstants(sec_cmd, cmd.material->layout.Get(),
									   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
									   sizeof(FrameConstants), &constants);

					VkDeviceSize offset = 0;
					VkBuffer vbo = cmd.mesh->buffer.Handle();
					vkCmdBindVertexBuffers(sec_cmd, 0, 1, &vbo, &offset);
					vkCmdDraw(sec_cmd, cmd.mesh->vertexCount, 1, 0, 0);
				}

				ZHLN_EndCommandBuffer(sec_cmd);
				secondaries[chunkIdx] = sec_cmd; // Store in the thread-safe ordered list
			});

		// Execute all chunks at once!
		Vk::ExecuteCommands(_impl->current_cmd, secondaries);
		_impl->drawQueue.clear();
	}

	// Finalize scene rendering
	ZHLN_EndRendering(_impl->current_cmd);

	// ========================================================================
	// UI & Present Logic
	// ========================================================================
	ImGui::Render();

	const VkRenderingAttachmentInfo colorAttachment = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		.pNext = nullptr,
		.imageView = _impl->presentation.swapchain.Get().views[_impl->current_image_index],
		.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
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

uint32_t RenderContext::CreateTexture(const void* data, uint32_t width, uint32_t height) {
	auto* impl = _impl.get();
	const VkDevice device = impl->ctx.Device();
	const size_t imageSize = static_cast<size_t>(width) * height * 4;

	// 1. Create the GPU Image
	VkImageCreateInfo imgInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
								 .imageType = VK_IMAGE_TYPE_2D,
								 .format = VK_FORMAT_R8G8B8A8_UNORM,
								 .extent = {width, height, 1},
								 .mipLevels = 1,
								 .arrayLayers = 1,
								 .samples = VK_SAMPLE_COUNT_1_BIT,
								 .tiling = VK_IMAGE_TILING_OPTIMAL,
								 .usage =
									 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
								 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};

	auto gpuImage = Vk::Image::Create(impl->allocator.Get(), imgInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	// 2. Staging & Synchronous Upload
	Vk::CommandPool tempPool(device, impl->ctx.PhysicalInfo().graphics_family);
	if (!tempPool.Allocate(1)) {
		ZHLN::Panic("Allocation failed for texture");
	}
	VkCommandBuffer cmd = tempPool[0];

	ZHLN_BeginCommandBuffer(cmd);

	// Create staging buffer and copy CPU data
	auto staging = Vk::Buffer::Create(impl->allocator.Get(), imageSize,
									  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	std::memcpy(staging.Map().data, data, imageSize);

	// Transition: Undefined -> Transfer Dst
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
		cmd, gpuImage.Handle());

	// Copy
	ZHLN_BufferImageCopyDesc copyRegion = {.buffer = staging.Handle(),
										   .image = gpuImage.Handle(),
										   .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
										   .width = width,
										   .height = height};
	ZHLN_CmdCopyBufferToImage(cmd, &copyRegion);

	// Transition: Transfer Dst -> Shader Read
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, gpuImage.Handle());

	ZHLN_EndCommandBuffer(cmd);

	// Submit and block (Init-time upload)
	VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
										 .commandBuffer = cmd};
	VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
							.commandBufferInfoCount = 1,
							.pCommandBufferInfos = &subInfo};
	vkQueueSubmit2(impl->ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(impl->ctx.GraphicsQueue());

	// 3. Create View and Register in Bindless Set
	auto gpuView = Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle());

	uint32_t index = impl->nextTextureIndex++;

	VkDescriptorImageInfo bindlessUpdate = {
		.sampler = VK_NULL_HANDLE, // Not needed for SAMPLED_IMAGE
		.imageView = gpuView.Get(),
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

	VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
								  .dstSet = impl->bindlessSet,
								  .dstBinding = 0,
								  .dstArrayElement = index,
								  .descriptorCount = 1,
								  .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
								  .pImageInfo = &bindlessUpdate};

	vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

	// 4. Track resources to prevent destruction
	impl->textureImages.push_back(std::move(gpuImage));
	impl->textureViews.push_back(std::move(gpuView));

	return index;
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
		.clear_depth = depth,
		.use_secondaries = true};

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

	// Just push to the queue!
	impl->drawQueue.push_back({reinterpret_cast<NativeMaterial*>(material.pipeline),
							   reinterpret_cast<NativeMesh*>(mesh.vertexBuffer), transform,
							   material.textureIndex});
}
} // namespace Renderer
} // namespace ZHLN