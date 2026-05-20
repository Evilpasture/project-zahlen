#include "Allocator.hpp"
#include "DescriptorLayout.hpp"
#include "PipelineBuilder.hpp"
#include "Postprocessing.hpp"
#include "PresentationContext.hpp"
#include "RenderCore.hpp"
#include "RenderTarget.hpp"
#include "Resources.hpp"
#include "SamplerBuilder.hpp"
#include "Vertex.hpp"
#include "Zahlen/Profiler.hpp"
#include "engine/RenderState.hpp"

#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <bit>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <ranges>
#include <threading/TaskSystem.hpp>
#include <vector>

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

ZHLN_REFLECT_VERTEX(::ZHLN::Vertex, position, normal, tangent, uv, color);

namespace ZHLN {

using GlobalBindlessLayout =
	Vk::DescriptorLayout<Vk::BindlessSampledImageSlot<0, 4096>, Vk::SamplerSlot<1>>;

// TAA Layout: Current Color, History, Velocity, Sampler
using TAALayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, Vk::SampledImageSlot<1>,
									   Vk::SampledImageSlot<2>, Vk::SamplerSlot<3>>;

// Blit Layout: Input, Sampler
using BlitLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, Vk::SamplerSlot<1>>;

struct NativeMesh {
	Vk::Buffer buffer;
	uint32_t vertexCount;
};
struct NativeMaterial {
	Vk::Pipeline pipeline;
	Vk::PipelineLayout layout;
};

struct DrawCommand {
	NativeMaterial* material;
	NativeMesh* mesh;
	JPH::Mat44 transform;
	JPH::Mat44 prevTransform;
	uint32_t textureIndex;
};

struct WorkerCmdContext {
	std::array<Vk::CommandPool, 2> pools;
	std::array<ZHLN::Atomic<uint32_t>, 2> cmdCount{};
};
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
struct RenderContext::Impl {
  public:
	Window& window;
	Vk::Context ctx;
	Vk::Allocator allocator;
	Vk::Surface surface;
	Vk::PresentationContext presentation;
	Vk::FrameSync<2> sync;
	Vk::CommandPools<2> pools;

	// --- RENDER TARGETS ---
	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> sceneColor;
	Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT> velocityBuffer;

	// Double buffered target for TAA Accumulation
	DoubleBuffered<Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>> accumBuffers;

	// --- POST PROCESSING PASSES ---
	Vk::PostProcessPass<TAALayout> taaPass;
	Vk::PostProcessPass<BlitLayout> blitPass;

	Vk::Sampler defaultSampler;

	uint32_t frame_index = 0;
	bool resized = true;
	bool needsInitialClear = true;
	VkCommandBuffer current_cmd = VK_NULL_HANDLE;
	uint32_t current_image_index = 0;

	JPH::Mat44 current_view_proj{};
	JPH::Mat44 prev_view_proj{};

	std::vector<std::unique_ptr<NativeMesh>> meshes;
	std::vector<std::unique_ptr<NativeMaterial>> materials;
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

		VkSamplerCreateInfo samplerInfo = {
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.magFilter = VK_FILTER_LINEAR,
			.minFilter = VK_FILTER_LINEAR,
			.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
			.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
			.mipLodBias = 0,
			.anisotropyEnable = VK_TRUE,
			.maxAnisotropy = ctx.PhysicalInfo().properties.properties.limits.maxSamplerAnisotropy,
			.compareEnable = VK_FALSE,
			.compareOp = VK_COMPARE_OP_NEVER,
			.minLod = 0.0f,
			.maxLod = 0.0f,
			.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
			.unnormalizedCoordinates = VK_FALSE,
		};
		VkSampler rawSampler = nullptr;
		vkCreateSampler(ctx.Device(), &samplerInfo, nullptr, &rawSampler);
		globalSampler = Vk::Sampler(ctx.Device(), rawSampler);

		GlobalBindlessLayout::Write(ctx.Device(), bindlessSet, Vk::SkipWrite{},
									Vk::SamplerWrite{globalSampler.Get()});
	}

	void InitPostProcessing() {
		defaultSampler = Vk::SamplerBuilder{}.Linear().ClampToEdge().Build(ctx.Device());

		// 1. Setup TAA
		VkPushConstantRange taaPush = {
			.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float)};
		auto taaShaders =
			Vk::ShaderStages::Create(ctx.Device(),
									 {.code = Vk::AsSpirV(&ZHLN_Resource_TaaVertSpv[0]),
									  .size = ZHLN_Resource_TaaVertSpv_Len,
									  .entry_point = "VSMain"},
									 {.code = Vk::AsSpirV(&ZHLN_Resource_TaaFragSpv[0]),
									  .size = ZHLN_Resource_TaaFragSpv_Len,
									  .entry_point = "PSMain"});

		if (!taaPass.Build(ctx.Device(), taaShaders, {VK_FORMAT_R16G16B16A16_SFLOAT}, &taaPush,
						   1)) {
			ZHLN::Log("TAA pass build failure, continuing...");
		}

		// 2. Setup Blit
		auto blitShaders =
			Vk::ShaderStages::Create(ctx.Device(),
									 {.code = (const uint32_t*)ZHLN_Resource_BlitVertSpv,
									  .size = ZHLN_Resource_BlitVertSpv_Len,
									  .entry_point = "VSMain"},
									 {.code = (const uint32_t*)ZHLN_Resource_BlitFragSpv,
									  .size = ZHLN_Resource_BlitFragSpv_Len,
									  .entry_point = "PSMain"});

		if (!blitPass.Build(ctx.Device(), blitShaders, {VK_FORMAT_B8G8R8A8_SRGB})) {
			ZHLN::Log("Blit pass build failure, continuing...");
		}
	}

	void SetupUI(GLFWwindow* window) {
		const std::array pool_sizes = {
			VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1000},
			VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
								 .descriptorCount = 1000},
			VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1000},
			VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1000},
			VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
								 .descriptorCount = 1000},
			VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
								 .descriptorCount = 1000},
			VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
								 .descriptorCount = 1000},
			VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
								 .descriptorCount = 1000},
			VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
								 .descriptorCount = 1000},
			VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
								 .descriptorCount = 1000},
			VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
								 .descriptorCount = 1000}};
		const VkDescriptorPoolCreateInfo pool_info = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
			.pNext = nullptr,
			.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
			.maxSets = 1000,
			.poolSizeCount = (uint32_t)std::size(pool_sizes),
			.pPoolSizes = pool_sizes.data(),
		};
		vkCreateDescriptorPool(ctx.Device(), &pool_info, nullptr, &uiPool);
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
			.DescriptorPoolSize = 0,
			.MinImageCount = 2,
			.ImageCount = 2,
			.PipelineCache = VK_NULL_HANDLE,
			.PipelineInfoMain =
				{
					.RenderPass = VK_NULL_HANDLE,
					.Subpass = 0,
					.MSAASamples = VK_SAMPLE_COUNT_1_BIT,
					.ExtraDynamicStates{},

					.PipelineRenderingCreateInfo =
						{.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
						 .pNext = nullptr,
						 .viewMask = 0,
						 .colorAttachmentCount = 1,
						 .pColorAttachmentFormats = &swapchainFormat,
						 .depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
						 .stencilAttachmentFormat = VK_FORMAT_UNDEFINED},

				},

			.UseDynamicRendering = true,
			.Allocator = nullptr,
			.CheckVkResultFn = nullptr,
			.MinAllocationSize = 0,
			.CustomShaderVertCreateInfo{},
			.CustomShaderFragCreateInfo{},
		};
		ImGui_ImplVulkan_Init(&init_info);
	}
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

RenderContext::RenderContext(Window& window, const RenderConfig& cfg)
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
	const ZHLN_InstanceDesc inst_desc = {.app_name = "ZHLN Engine",
										 .version = VK_MAKE_API_VERSION(0, 1, 0, 0),
										 .extension_count = static_cast<uint32_t>(inst_exts.size()),
										 .severity_flags =
											 VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
											 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
										 .extensions = inst_exts.data(),
										 .enable_validation = cfg.enableValidation};
	VkPhysicalDeviceVulkan13Features feat13 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.pNext = nullptr,
		.synchronization2 = VK_TRUE,
		.dynamicRendering = VK_TRUE};
	VkPhysicalDeviceVulkan12Features feat12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &feat13,
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
								.extensions = &dev_exts[0],
								.extension_count =
									(uint32_t)(sizeof(dev_exts) / sizeof(const char*)),
								.features = &feat2,
								.enable_validation = cfg.enableValidation};
	ZHLN_DeviceSelectDesc select_desc = {.instance = VK_NULL_HANDLE,
										 .surface = VK_NULL_HANDLE,
										 .score_fn = nullptr,
										 .score_userdata = nullptr};

	_impl->ctx = Vk::Context::Create(inst_desc, select_desc, dev_desc);
	_impl->InitBindless();
	_impl->InitPostProcessing();

	auto* glfwWin = static_cast<GLFWwindow*>(window.GetNativeHandle());
	VkSurfaceKHR raw_surface = nullptr;
	glfwCreateWindowSurface(_impl->ctx.Instance(), glfwWin, nullptr, &raw_surface);
	_impl->surface = Vk::Surface(_impl->ctx.Instance(), raw_surface);

	if (!_impl->allocator.Init(_impl->ctx)) {
		ZHLN::Panic("FATAL: Vulkan Memory Allocator (VMA) failed to initialize");
	}

	// Initialize presentation with current window size
	int width = 0;
	int height = 0;
	glfwGetFramebufferSize(glfwWin, &width, &height);
	if (!_impl->presentation.Init(_impl->ctx, _impl->allocator, _impl->surface.Get(),
								  (uint32_t)width, (uint32_t)height, cfg.vsync)) {
		ZHLN::Panic("FATAL: Presentation Context initialization failed");
	}

	_impl->sync = Vk::FrameSync<2>::Create(_impl->ctx.Device());
	_impl->pools =
		Vk::CommandPools<2>::Create(_impl->ctx.Device(), _impl->ctx.PhysicalInfo().graphics_family);
	_impl->SetupUI(static_cast<GLFWwindow*>(window.GetNativeHandle()));

	uint32_t workerCount = TaskSystem::GetWorkerCount() + 1;
	if (workerCount == 0) {
		workerCount = 1;
	}
	_impl->workerCmds.resize(workerCount);

	for (auto& worker : _impl->workerCmds) {
		// Loop directly over the elements of the pools array
		for (auto& pool : worker.pools) {
			pool = Vk::CommandPool(_impl->ctx.Device(), _impl->ctx.PhysicalInfo().graphics_family);

			// Allocate secondary command buffers
			if (!pool.AllocateSecondary(256)) {
				ZHLN::Panic(
					"FATAL: Failed to pre-allocate secondary command buffers for worker threads. "
					"Check if your GPU supports {} command buffers per pool.",
					256);
			}
		}
	}
}

RenderContext::~RenderContext() {
	if (_impl && (_impl->ctx.Device() != nullptr)) {
		vkDeviceWaitIdle(_impl->ctx.Device());
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		if (_impl->uiPool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(_impl->ctx.Device(), _impl->uiPool, nullptr);
		}
		_impl->meshes.clear();
		_impl->materials.clear();
	}
}

auto RenderContext::GetRendererName() const -> const char* {
	return "ZHLN (Modernized TAA)";
}

void RenderContext::SetResolution([[maybe_unused]] const Extent2D& res) {
	_impl->resized = true;
}

auto RenderContext::CreateVertexBuffer(const void* data, size_t size) -> BufferHandle {
	auto gpu_buf =
		Vk::Buffer::Create(_impl->allocator.Get(), size,
						   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
						   VMA_MEMORY_USAGE_GPU_ONLY);
	VkCommandBuffer cmd = _impl->pools.Cmd(0);
	ZHLN_BeginCommandBuffer(cmd);
	auto staging = Vk::UploadToBuffer(_impl->allocator.Get(), cmd, gpu_buf, data, size);
	ZHLN_EndCommandBuffer(cmd);
	VkCommandBufferSubmitInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
										  .pNext = nullptr,
										  .commandBuffer = cmd,
										  .deviceMask = 0};
	VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
							.pNext = nullptr,
							.flags = 0,
							.waitSemaphoreInfoCount = 0,
							.pWaitSemaphoreInfos = nullptr,
							.commandBufferInfoCount = 1,
							.pCommandBufferInfos = &cmd_info,
							.signalSemaphoreInfoCount = 0,
							.pSignalSemaphoreInfos = nullptr};
	vkQueueSubmit2(_impl->ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(_impl->ctx.GraphicsQueue());

	auto mesh = std::make_unique<NativeMesh>(std::move(gpu_buf),
											 static_cast<uint32_t>(size / sizeof(Vertex)));
	auto handle = static_cast<BufferHandle>(reinterpret_cast<uintptr_t>(mesh.get()));
	_impl->meshes.push_back(std::move(mesh));
	return handle;
}

auto RenderContext::CreateMaterial(const PipelineDesc& desc) -> Material {
	ZHLN_ShaderDesc v_desc = {.code = Vk::AsSpirV(desc.vertexShaderData),
							  .size = desc.vertexShaderSize,
							  .entry_point = nullptr};
	ZHLN_ShaderDesc f_desc = {.code = Vk::AsSpirV(desc.fragShaderData),
							  .size = desc.fragShaderSize,
							  .entry_point = nullptr};
	auto shaders = Vk::ShaderStages::Create(_impl->ctx.Device(), v_desc, f_desc);
	auto* impl = _impl.get();

	VkPushConstantRange pc_range = {.stageFlags =
										VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
									.offset = 0,
									.size = sizeof(ZHLN::FrameConstants)};
	const std::array layouts = {impl->bindlessLayout.Get()};
	ZHLN_PipelineLayoutDesc layout_desc = {.set_layouts = layouts.data(),
										   .set_layout_count = 1,
										   .push_constants = &pc_range,
										   .push_constant_count = 1};
	auto layout = Vk::PipelineLayout(impl->ctx.Device(),
									 ZHLN_CreatePipelineLayout(impl->ctx.Device(), &layout_desc));

	auto pipeline = Vk::PipelineBuilder{}
						.Shaders(shaders)
						.Layout(layout.Get())
						.Vertex<Vertex>()
						// OUTPUT 2 ATTACHMENTS (Color + Velocity)
						.ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16_SFLOAT})
						.DepthFormat(VK_FORMAT_D32_SFLOAT)
						.Build(impl->ctx.Device());

	auto mat = std::make_unique<NativeMaterial>(std::move(pipeline), std::move(layout));
	auto handle = static_cast<PipelineHandle>(reinterpret_cast<uintptr_t>(mat.get()));
	impl->materials.push_back(std::move(mat));
	return Material{.pipeline = handle};
}

void RenderContext::BeginFrame() {
	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];

	// 1. Synchronize
	ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, &_impl->pools[_impl->frame_index]);

	// Reset worker command pools and counters
	for (auto& worker : _impl->workerCmds) {
		worker.cmdCount[_impl->frame_index].store(0, std::memory_order_relaxed);
		worker.pools[_impl->frame_index].Reset();
	}

	// 2. Handle Resize (Allocation Only)
	if (_impl->resized) {
		int width = 0;
		int height = 0;
		glfwGetFramebufferSize(static_cast<GLFWwindow*>(_impl->window.GetNativeHandle()), &width,
							   &height);
		if (width == 0 || height == 0) {
			return;
		}

		if (!_impl->presentation.Rebuild((uint32_t)width, (uint32_t)height)) {
			return;
		}

		VkExtent2D ext = _impl->presentation.swapchain.Get().extent;

		// Re-create textures
		_impl->sceneColor = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->velocityBuffer = Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->accumBuffers[0] = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->accumBuffers[1] = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

		_impl->needsInitialClear = true; // Mark that these need a transition/clear
		_impl->depth_ready = false;
		_impl->resized = false;
	}

	// 3. Acquire Swapchain Image
	ZHLN_AcquireDesc acq = {.swapchain = _impl->presentation.swapchain.Get().handle,
							.image_available = s.image_available,
							.timeout_ns = UINT64_MAX};
	if (ZHLN_AcquireImage(_impl->ctx.Device(), &acq, &_impl->current_image_index) ==
		ZHLN_FrameResult_OutOfDate) {
		_impl->resized = true;
		_impl->current_cmd = VK_NULL_HANDLE;
		return;
	}

	// 4. Start Command Buffer
	_impl->current_cmd = _impl->pools.Cmd(_impl->frame_index);
	ZHLN_BeginCommandBuffer(_impl->current_cmd);

	// 5. THE FIX: Initialize new textures if needed, right here in the main stream
	if (_impl->needsInitialClear) {
		// Barriers: Undefined -> Color Attachment
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
			_impl->current_cmd, _impl->sceneColor.image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
			_impl->current_cmd, _impl->velocityBuffer.image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
			_impl->current_cmd, _impl->accumBuffers[0].image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
			_impl->current_cmd, _impl->accumBuffers[1].image.Handle());

		// Clear them
		VkClearValue clearColor = {.color = {.float32 = {0, 0, 0, 1}}};
		std::array<VkRenderingAttachmentInfo, 4> attachments{};
		const std::array views = {_impl->sceneColor.view.Get(), _impl->velocityBuffer.view.Get(),
								  _impl->accumBuffers[0].view.Get(),
								  _impl->accumBuffers[1].view.Get()};

		// Zip both collections together into parallel pairs
		for (auto&& [attachment, view] : std::views::zip(attachments, views)) {
			attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
						  .pNext = nullptr,
						  .imageView = view, // Pure reference assignment, no brackets, no pointers!
						  .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
						  .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
						  .clearValue = clearColor};
		}

		VkRenderingInfo renderInfo = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.pNext = nullptr,
			.flags = 0,
			.renderArea = {.offset = {.x = 0, .y = 0},
						   .extent = _impl->presentation.swapchain.Get().extent},
			.layerCount = 1,
			.colorAttachmentCount = 4,
			.pColorAttachments = attachments.data()};
		vkCmdBeginRendering(_impl->current_cmd, &renderInfo);
		vkCmdEndRendering(_impl->current_cmd);

		// Transition: Color Attachment -> Shader Read (To match descriptors)
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			_impl->current_cmd, _impl->sceneColor.image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			_impl->current_cmd, _impl->velocityBuffer.image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			_impl->current_cmd, _impl->accumBuffers[0].image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			_impl->current_cmd, _impl->accumBuffers[1].image.Handle());

		// Update Descriptors immediately (Write to both sets in the double buffer)
		for (int i = 0; i < 2; ++i) {
			_impl->taaPass.WriteIndex(
				_impl->ctx.Device(), i,
				Vk::ImageWrite{.view = _impl->sceneColor.view.Get(),
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				Vk::ImageWrite{.view = _impl->accumBuffers[1 - i].view.Get(),
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				Vk::ImageWrite{.view = _impl->velocityBuffer.view.Get(),
							   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
				Vk::SamplerWrite{.sampler = _impl->defaultSampler.Get()});
		}

		_impl->needsInitialClear = false;
	}
}

void RenderContext::EndFrame() {
	ZHLN_PROFILE_SCOPE("Render (CPU Record)");
	if (_impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	if (!_impl->drawQueue.empty()) {
		uint32_t numChunks = (_impl->drawQueue.size() + 255) / 256;
		std::vector<VkCommandBuffer> secondaries(numChunks);

		// Tell secondary buffers they are writing to Color and Velocity
		std::array<VkFormat, 2> formats = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16_SFLOAT};
		const VkCommandBufferInheritanceRenderingInfo inherit = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
			.pNext = nullptr,
			.flags = 0,
			.viewMask = 0,
			.colorAttachmentCount = 2,
			.pColorAttachmentFormats = formats.data(),
			.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
			.stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
		const VkCommandBufferInheritanceInfo pInherit = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
			.pNext = &inherit,
			.renderPass = VK_NULL_HANDLE,
			.subpass = 0,
			.framebuffer = VK_NULL_HANDLE,
			.occlusionQueryEnable = VK_FALSE,
			.queryFlags = 0,
			.pipelineStatistics = 0};

		std::sort(_impl->drawQueue.begin(), _impl->drawQueue.end(),
				  [](const DrawCommand& a, const DrawCommand& b) {
					  if (a.material != b.material) {
						  return a.material < b.material;
					  }
					  return a.mesh < b.mesh;
				  });

		TaskSystem::ParallelFor(
			_impl->drawQueue.size(), 256,
			[&](uint32_t start, uint32_t end, uint32_t chunkIdx) -> void {
				uint32_t wIdx = TaskSystem::GetWorkerIndex();
				if (wIdx >= _impl->workerCmds.size()) {
					wIdx = (uint32_t)(_impl->workerCmds.size() - 1);
				}

				uint32_t localCmdIdx =
					_impl->workerCmds[wIdx].cmdCount[_impl->frame_index].fetch_add(
						1, std::memory_order_relaxed);
				VkCommandBuffer sec_cmd =
					_impl->workerCmds[wIdx].pools[_impl->frame_index][localCmdIdx];

				const VkCommandBufferBeginInfo beginInfo = {
					.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					.pNext = nullptr,
					.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
							 VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
					.pInheritanceInfo = &pInherit};
				vkBeginCommandBuffer(sec_cmd, &beginInfo);

				VkExtent2D extent = _impl->presentation.swapchain.Get().extent;
				const VkViewport viewport = {.x = 0.0f,
											 .y = (float)extent.height,
											 .width = (float)extent.width,
											 .height = -(float)extent.height,
											 .minDepth = 0.0f,
											 .maxDepth = 1.0f};
				const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = extent};
				vkCmdSetViewport(sec_cmd, 0, 1, &viewport);
				vkCmdSetScissor(sec_cmd, 0, 1, &scissor);

				for (uint32_t i = start; i < end; ++i) {
					const auto& cmd = _impl->drawQueue[i];

					if (!cmd.material->pipeline.Valid()) {
						continue;
					}

					vkCmdBindPipeline(sec_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
									  cmd.material->pipeline.Get());
					vkCmdBindDescriptorSets(sec_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
											cmd.material->layout.Get(), 0, 1, &_impl->bindlessSet,
											0, nullptr);

					JPH::Mat44 final_transform = _impl->current_view_proj * cmd.transform;
					JPH::Mat44 final_prev_transform = _impl->prev_view_proj * cmd.prevTransform;
					FrameConstants constants = {.transform = final_transform,
												.prevTransform = final_prev_transform,
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
				secondaries[chunkIdx] = sec_cmd;
			});

		Vk::ExecuteCommands(_impl->current_cmd, secondaries);
		_impl->drawQueue.clear();
	}

	// Close the main rendering pass (Color + Velocity + Depth)
	ZHLN_EndRendering(_impl->current_cmd);

	// --- 2. Synchronize Scene Results for Post-Processing ---
	VkExtent2D extent = _impl->presentation.swapchain.Get().extent;

	if (!_impl->sceneColor.image.Valid() || !_impl->accumBuffers.Current().image.Valid()) {
		ZHLN_EndCommandBuffer(_impl->current_cmd);
		_impl->current_cmd = VK_NULL_HANDLE;
		return;
	}

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		_impl->current_cmd, _impl->sceneColor.image.Handle());
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		_impl->current_cmd, _impl->velocityBuffer.image.Handle());

	// --- 3. TAA Pass ---
	bool useTAA = g_TAAState.enabled && _impl->taaPass.pipeline.Valid();
	if (useTAA) {
		// History (Current) transitions to READ. Output (Next) transitions to WRITE.
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			_impl->current_cmd, _impl->accumBuffers.Current().image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
							 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
			_impl->current_cmd, _impl->accumBuffers.Next().image.Handle());

		VkRenderingAttachmentInfo col = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
										 .pNext = nullptr,
										 .imageView = _impl->accumBuffers.Next().view.Get(),
										 .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
										 .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
										 .storeOp = VK_ATTACHMENT_STORE_OP_STORE};

		const VkRenderingInfo renderInfo = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.pNext = nullptr,
			.flags = 0,
			.renderArea = {.offset = {.x = 0, .y = 0}, .extent = extent},
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &col};

		vkCmdBeginRendering(_impl->current_cmd, &renderInfo);

		// Safely write the ping-ponging target descriptors
		_impl->taaPass.WriteNext(_impl->ctx.Device(),
								 Vk::ImageWrite{.view = _impl->sceneColor.view.Get(),
												.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
								 Vk::ImageWrite{.view = _impl->accumBuffers.Current().view.Get(),
												.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
								 Vk::ImageWrite{.view = _impl->velocityBuffer.view.Get(),
												.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
								 Vk::SamplerWrite{.sampler = _impl->defaultSampler.Get()});

		_impl->taaPass.Execute(_impl->current_cmd, g_TAAState.feedback);
		ZHLN_EndRendering(_impl->current_cmd);

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			_impl->current_cmd, _impl->accumBuffers.Next().image.Handle());
	}

	// --- 4. Final Blit (To Swapchain) ---
	VkImage swapImg = _impl->presentation.swapchain.Get().images[_impl->current_image_index];
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		_impl->current_cmd, swapImg);

	VkImageView blitSource =
		useTAA ? _impl->accumBuffers.Next().view.Get() : _impl->sceneColor.view.Get();

	if (!useTAA) {
		Renderer::Clear(*this, {1.0f, 0.0f, 1.0f, 1.0f});
	}

	_impl->blitPass.WriteNext(
		_impl->ctx.Device(),
		Vk::ImageWrite{.view = blitSource, .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		Vk::SamplerWrite{_impl->defaultSampler.Get()});

	if (_impl->blitPass.pipeline.Valid()) {
		VkRenderingAttachmentInfo col = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.pNext = nullptr,
			.imageView = _impl->presentation.swapchain.Get().views[_impl->current_image_index],
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE};

		const VkRenderingInfo renderInfo = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
			.pNext = nullptr,
			.flags = 0,
			.renderArea{.offset = {.x = 0, .y = 0}, .extent = extent},
			.layerCount = 1,
			.colorAttachmentCount = 1,
			.pColorAttachments = &col};

		vkCmdBeginRendering(_impl->current_cmd, &renderInfo);
		_impl->blitPass.Execute(_impl->current_cmd);

		ImGui::Render();
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), _impl->current_cmd);
		ZHLN_EndRendering(_impl->current_cmd);
	}

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(
		_impl->current_cmd, swapImg);
	ZHLN_EndCommandBuffer(_impl->current_cmd);

	// --- 5. Submit ---
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

	// FLIP THE BUFFERS
	_impl->accumBuffers.Flip();
	_impl->taaPass.sets.Flip();
	_impl->blitPass.sets.Flip();

	_impl->frame_index = (_impl->frame_index + 1) % 2;
	_impl->current_cmd = VK_NULL_HANDLE;
}

auto RenderContext::CreateTexture(const void* data, uint32_t width, uint32_t height) -> uint32_t {
	auto* impl = _impl.get();
	const VkDevice device = impl->ctx.Device();
	const size_t imageSize = static_cast<size_t>(width) * height * 4;

	// 1. Create the GPU Image
	const VkImageCreateInfo imgInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
									   .pNext = nullptr,
									   .imageType = VK_IMAGE_TYPE_2D,
									   .format = VK_FORMAT_R8G8B8A8_UNORM,
									   .extent{.width = width, .height = height, .depth = 1},
									   .mipLevels = 1,
									   .arrayLayers = 1,
									   .samples = VK_SAMPLE_COUNT_1_BIT,
									   .tiling = VK_IMAGE_TILING_OPTIMAL,
									   .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
												VK_IMAGE_USAGE_SAMPLED_BIT,
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
										 .pNext = nullptr,
										 .commandBuffer = cmd};
	VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
							.pNext = nullptr,
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
								  .pNext = nullptr,
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

auto RenderContext::GetGPUName() const -> const char* {
	return &_impl->ctx.PhysicalInfo().properties.properties.deviceName[0];
}

namespace Renderer {
void Clear(RenderContext& ctx, const JPH::Vec4& color, float depth) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		impl->current_cmd, impl->sceneColor.image.Handle());
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		impl->current_cmd, impl->velocityBuffer.image.Handle());

	if (!impl->depth_ready) {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			impl->current_cmd, impl->presentation.depthTarget.image.Handle(),
			VK_IMAGE_ASPECT_DEPTH_BIT);
		impl->depth_ready = true;
	} else {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			impl->current_cmd, impl->presentation.depthTarget.image.Handle(),
			VK_IMAGE_ASPECT_DEPTH_BIT);
	} 


	std::array<VkRenderingAttachmentInfo, 2> cols = {
		VkRenderingAttachmentInfo{
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.pNext = nullptr,
			.imageView = impl->sceneColor.view.Get(),
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.resolveMode = VK_RESOLVE_MODE_NONE,
			.resolveImageView = VK_NULL_HANDLE,
			.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue = {.color = {.float32 = {color.GetX(), color.GetY(), color.GetZ(),
												 color.GetW()}}}},
		{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
		 .pNext = nullptr,
		 .imageView = impl->velocityBuffer.view.Get(),
		 .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		 .resolveMode = VK_RESOLVE_MODE_NONE,
		 .resolveImageView = VK_NULL_HANDLE,
		 .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		 .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
		 .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		 .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 0.0f}}}}};
	VkRenderingAttachmentInfo depthAtt = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
										  .pNext = nullptr,
										  .imageView = impl->presentation.depthTarget.view.Get(),
										  .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
										  .resolveMode = VK_RESOLVE_MODE_NONE,
										  .resolveImageView = VK_NULL_HANDLE,
										  .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
										  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
										  .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
										  .clearValue = {.depthStencil = {.depth = depth}}};
	const VkRenderingInfo renderInfo = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.pNext = nullptr,
		.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT,
		.renderArea{.offset = {.x = 0, .y = 0},
					.extent = impl->presentation.swapchain.Get().extent},
		.layerCount = 1,
		.viewMask = 0,
		.colorAttachmentCount = 2,
		.pColorAttachments = cols.data(),
		.pDepthAttachment = &depthAtt,
		.pStencilAttachment = nullptr};

	vkCmdBeginRendering(impl->current_cmd, &renderInfo);

	const VkViewport viewport = {.x = 0.0f,
								 .y = (float)impl->presentation.swapchain.Get().extent.height,
								 .width = (float)impl->presentation.swapchain.Get().extent.width,
								 .height = -(float)impl->presentation.swapchain.Get().extent.height,
								 .minDepth = 0.0f,
								 .maxDepth = 1.0f};
	const VkRect2D scissor = {.offset = {.x = 0, .y = 0},
							  .extent = impl->presentation.swapchain.Get().extent};
	vkCmdSetViewport(impl->current_cmd, 0, 1, &viewport);
	vkCmdSetScissor(impl->current_cmd, 0, 1, &scissor);
}

void SetMatrices(RenderContext& ctx, const JPH::Mat44& viewProj, const JPH::Mat44& prevViewProj) {
	auto* impl = ctx.GetImpl();
	impl->current_view_proj = viewProj;
	impl->prev_view_proj = prevViewProj;
}

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const JPH::Mat44& transform, const JPH::Mat44& prevTransform) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE) {
		return;
	}
	impl->drawQueue.push_back({.material = std::bit_cast<NativeMaterial*>(material.pipeline),
							   .mesh = std::bit_cast<NativeMesh*>(mesh.vertexBuffer),
							   .transform = transform,
							   .prevTransform = prevTransform,
							   .textureIndex = material.textureIndex});
}
} // namespace Renderer
} // namespace ZHLN
