#include "Allocator.hpp"
#include "DescriptorLayout.hpp"
#include "PipelineBuilder.hpp"
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
#include <atomic>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
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

// FIX: Inject the missing FormatTraits so RenderTarget.hpp can deduce the Aspect Mask
template <> struct FormatTraits<VK_FORMAT_R16G16_SFLOAT> {
	static constexpr VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};
} // namespace ZHLN::Vk

ZHLN_REFLECT_VERTEX(::ZHLN::Vertex, position, normal, tangent, uv, color);

namespace ZHLN {

using GlobalBindlessLayout =
	Vk::DescriptorLayout<Vk::BindlessSampledImageSlot<0, 4096>, Vk::SamplerSlot<1>>;

// FIX: DescriptorLayout requires matching SamplerWrite and ImageWrite arguments.
// Separate Image and Sampler slots prevent the template constraint errors.
using TAALayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // Current Color
									   Vk::SampledImageSlot<1>, // History
									   Vk::SampledImageSlot<2>, // Velocity
									   Vk::SamplerSlot<3>		// Sampler
									   >;
using BlitLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // Input
										Vk::SamplerSlot<1>		 // Sampler
										>;

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
	Vk::CommandPool pools[2];
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

	// --- TAA RENDER TARGETS ---
	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> sceneColor;
	Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT> velocityBuffer;
	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> accumulationBuffers[2];
	uint32_t historyIndex = 0;

	Vk::DescriptorSetLayout taaLayout;
	Vk::DescriptorPool taaPool;
	VkDescriptorSet taaSets[2];
	Vk::PipelineLayout taaPipelineLayout;
	Vk::Pipeline taaPipeline;

	Vk::DescriptorSetLayout blitLayout;
	Vk::DescriptorPool blitPool;
	VkDescriptorSet blitSets[2];
	Vk::PipelineLayout blitPipelineLayout;
	Vk::Pipeline blitPipeline;

	Vk::Sampler defaultSampler;

	uint32_t frame_index = 0;
	bool resized = true;
	VkCommandBuffer current_cmd = VK_NULL_HANDLE;
	uint32_t current_image_index = 0;

	JPH::Mat44 current_view_proj;
	JPH::Mat44 prev_view_proj;

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

		VkSamplerCreateInfo samplerInfo = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.anisotropyEnable = VK_TRUE;
		samplerInfo.maxAnisotropy =
			ctx.PhysicalInfo().properties.properties.limits.maxSamplerAnisotropy;

		VkSampler rawSampler;
		vkCreateSampler(ctx.Device(), &samplerInfo, nullptr, &rawSampler);
		globalSampler = Vk::Sampler(ctx.Device(), rawSampler);

		GlobalBindlessLayout::Write(ctx.Device(), bindlessSet, Vk::SkipWrite{},
									Vk::SamplerWrite{globalSampler.Get()});
	}

	void InitPostProcessing() {
		defaultSampler = Vk::SamplerBuilder{}.Linear().ClampToEdge().Build(ctx.Device());

		// 1. TAA Layouts
		taaLayout = TAALayout::CreateLayout(ctx.Device());
		taaPool = TAALayout::CreatePool(ctx.Device(), 2);
		taaSets[0] = TAALayout::Allocate(ctx.Device(), taaPool.Get(), taaLayout.Get());
		taaSets[1] = TAALayout::Allocate(ctx.Device(), taaPool.Get(), taaLayout.Get());

		VkPushConstantRange taaPush = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float)};
		VkDescriptorSetLayout rawTaaLayout = taaLayout.Get();
		ZHLN_PipelineLayoutDesc taaLDesc = {.set_layouts = &rawTaaLayout,
											.set_layout_count = 1,
											.push_constants = &taaPush,
											.push_constant_count = 1};
		taaPipelineLayout =
			Vk::PipelineLayout(ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &taaLDesc));

		// 2. Blit (Tonemapping) Layouts
		blitLayout = BlitLayout::CreateLayout(ctx.Device());
		blitPool = BlitLayout::CreatePool(ctx.Device(), 2);
		blitSets[0] = BlitLayout::Allocate(ctx.Device(), blitPool.Get(), blitLayout.Get());
		blitSets[1] = BlitLayout::Allocate(ctx.Device(), blitPool.Get(), blitLayout.Get());

		VkDescriptorSetLayout rawBlitLayout = blitLayout.Get();
		ZHLN_PipelineLayoutDesc blitLDesc = {.set_layouts = &rawBlitLayout,
											 .set_layout_count = 1,
											 .push_constants = nullptr,
											 .push_constant_count = 0};
		blitPipelineLayout =
			Vk::PipelineLayout(ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &blitLDesc));

		// LOAD EMBEDDED TAA SHADERS
		ZHLN_ShaderDesc taa_v = {(const uint32_t*)ZHLN_Resource_TaaVertSpv,
								 ZHLN_Resource_TaaVertSpv_Len, "VSMain"};
		ZHLN_ShaderDesc taa_f = {(const uint32_t*)ZHLN_Resource_TaaFragSpv,
								 ZHLN_Resource_TaaFragSpv_Len, "PSMain"};
		auto taaShaders = Vk::ShaderStages::Create(ctx.Device(), taa_v, taa_f);

		if (taaShaders.Valid()) {
			taaPipeline = Vk::PipelineBuilder{}
							  .Shaders(taaShaders)
							  .Layout(taaPipelineLayout.Get())
							  .ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
							  .NoDepth()
							  .CullNone()
							  .Build(ctx.Device());
		}

		// LOAD EMBEDDED BLIT SHADERS
		ZHLN_ShaderDesc blit_v = {(const uint32_t*)ZHLN_Resource_BlitVertSpv,
								  ZHLN_Resource_BlitVertSpv_Len, "VSMain"};
		ZHLN_ShaderDesc blit_f = {(const uint32_t*)ZHLN_Resource_BlitFragSpv,
								  ZHLN_Resource_BlitFragSpv_Len, "PSMain"};
		auto blitShaders = Vk::ShaderStages::Create(ctx.Device(), blit_v, blit_f);

		if (blitShaders.Valid()) {
			blitPipeline = Vk::PipelineBuilder{}
							   .Shaders(blitShaders)
							   .Layout(blitPipelineLayout.Get())
							   .ColorFormats({VK_FORMAT_B8G8R8A8_SRGB})
							   .NoDepth()
							   .CullNone()
							   .Build(ctx.Device());
		}
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
			.MinImageCount = 2,
			.ImageCount = 2,
			.PipelineInfoMain =
				{
					.MSAASamples = VK_SAMPLE_COUNT_1_BIT,
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
		};
		ImGui_ImplVulkan_Init(&init_info);
	}
};

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
								.extensions = dev_exts,
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
								  (uint32_t)h, cfg.vsync)) {
		ZHLN::Panic("FATAL: Presentation Context initialization failed");
	}

	_impl->sync = Vk::FrameSync<2>::Create(_impl->ctx.Device());
	_impl->pools =
		Vk::CommandPools<2>::Create(_impl->ctx.Device(), _impl->ctx.PhysicalInfo().graphics_family);
	_impl->SetupUI(static_cast<GLFWwindow*>(window.GetNativeHandle()));

	uint32_t workerCount = TaskSystem::GetWorkerCount() + 1;
	if (workerCount == 0)
		workerCount = 1;
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
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		if (_impl->uiPool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(_impl->ctx.Device(), _impl->uiPool, nullptr);
		_impl->meshes.clear();
		_impl->materials.clear();
	}
}

const char* RenderContext::GetRendererName() const {
	return "ZHLN (Modernized TAA)";
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

Material RenderContext::CreateMaterial(const PipelineDesc& desc) {
	ZHLN_ShaderDesc v_desc = {(const uint32_t*)desc.vertexShaderData, desc.vertexShaderSize,
							  nullptr};
	ZHLN_ShaderDesc f_desc = {(const uint32_t*)desc.fragShaderData, desc.fragShaderSize, nullptr};
	auto shaders = Vk::ShaderStages::Create(_impl->ctx.Device(), v_desc, f_desc);
	auto* impl = _impl.get();

	VkPushConstantRange pc_range = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
									sizeof(ZHLN::FrameConstants)};
	VkDescriptorSetLayout layouts[] = {impl->bindlessLayout.Get()};
	ZHLN_PipelineLayoutDesc layout_desc = {.set_layouts = layouts,
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
	// 1. Get sync primitives for the current frame slot
	const ZHLN_FrameSync& s = _impl->sync[_impl->frame_index];

	// 2. Wait for the GPU to finish the previous work for this frame slot
	// and reset the command pool so we can record new commands.
	ZHLN_WaitAndResetFrame(_impl->ctx.Device(), s.in_flight, &_impl->pools[_impl->frame_index]);

	// Reset worker counters
	for (auto& w : _impl->workerCmds)
		w.cmdCount[_impl->frame_index].store(0, std::memory_order_relaxed);

	// 3. Acquire the next available swapchain image
	ZHLN_AcquireDesc acq = {_impl->presentation.swapchain.Get().handle, s.image_available,
							UINT64_MAX};
	if (ZHLN_AcquireImage(_impl->ctx.Device(), &acq, &_impl->current_image_index) ==
		ZHLN_FrameResult_OutOfDate) {
		_impl->resized = true;
	}

	// 4. Start the command buffer NOW
	_impl->current_cmd = _impl->pools.Cmd(_impl->frame_index);
	ZHLN_BeginCommandBuffer(_impl->current_cmd);

	// 5. Now handle the resize/initialization logic
	if (_impl->resized) {
		int w, h;
		glfwGetFramebufferSize(static_cast<GLFWwindow*>(_impl->window.GetNativeHandle()), &w, &h);

		// If Rebuild fails (e.g. window minimized), we stop here
		if (!_impl->presentation.Rebuild((uint32_t)w, (uint32_t)h))
			return;

		_impl->depth_ready = false;
		_impl->resized = false;

		// Recreate TAA Render Targets
		VkExtent2D ext = _impl->presentation.swapchain.Get().extent;
		_impl->sceneColor = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->velocityBuffer = Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->accumulationBuffers[0] = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
		_impl->accumulationBuffers[1] = Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>::Create(
			_impl->allocator, _impl->ctx, ext,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

		for (int i = 0; i < 2; ++i) {
			TAALayout::Write(_impl->ctx.Device(), _impl->taaSets[i],
							 Vk::ImageWrite{_impl->sceneColor.view.Get(),
											VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
							 Vk::ImageWrite{_impl->accumulationBuffers[1 - i].view.Get(),
											VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
							 Vk::ImageWrite{_impl->velocityBuffer.view.Get(),
											VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
							 Vk::SamplerWrite{_impl->defaultSampler.Get()});
		}

		// 6. BOOTSTRAP: Now current_cmd is valid and open!
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
			_impl->current_cmd, _impl->accumulationBuffers[0].image.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
			_impl->current_cmd, _impl->accumulationBuffers[1].image.Handle());
	}
}

void RenderContext::EndFrame() {
	ZHLN_PROFILE_SCOPE("Render (CPU Record)");
	if (_impl->current_cmd == VK_NULL_HANDLE)
		return;

	if (!_impl->drawQueue.empty()) {
		uint32_t numChunks = (_impl->drawQueue.size() + 255) / 256;
		std::vector<VkCommandBuffer> secondaries(numChunks);

		// Tell secondary buffers they are writing to Color and Velocity
		VkFormat formats[2] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16_SFLOAT};
		const VkCommandBufferInheritanceRenderingInfo inherit = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
			.pNext = nullptr,
			.flags = 0,
			.viewMask = 0,
			.colorAttachmentCount = 2,
			.pColorAttachmentFormats = formats,
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
					  if (a.material != b.material)
						  return a.material < b.material;
					  return a.mesh < b.mesh;
				  });

		TaskSystem::ParallelFor(
			_impl->drawQueue.size(), 256, [&](uint32_t start, uint32_t end, uint32_t chunkIdx) {
				uint32_t wIdx = TaskSystem::GetWorkerIndex();
				if (wIdx >= _impl->workerCmds.size())
					wIdx = (uint32_t)(_impl->workerCmds.size() - 1);

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
				const VkRect2D scissor = {{0, 0}, extent};
				vkCmdSetViewport(sec_cmd, 0, 1, &viewport);
				vkCmdSetScissor(sec_cmd, 0, 1, &scissor);

				for (uint32_t i = start; i < end; ++i) {
					const auto& cmd = _impl->drawQueue[i];

					if (!cmd.material->pipeline.Valid())
						continue;

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

	ZHLN_EndRendering(_impl->current_cmd);

	// ========================================================================
	// TAA & BLIT PASSES
	// ========================================================================
	uint32_t currHistory = _impl->historyIndex;
	uint32_t nextHistory = 1 - _impl->historyIndex;
	VkExtent2D extent = _impl->presentation.swapchain.Get().extent;

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		_impl->current_cmd, _impl->sceneColor.image.Handle());
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		_impl->current_cmd, _impl->velocityBuffer.image.Handle());
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		_impl->current_cmd, _impl->accumulationBuffers[currHistory].image.Handle());
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		_impl->current_cmd, _impl->accumulationBuffers[nextHistory].image.Handle());

	if (_impl->taaPipeline.Valid() && g_TAAState.enabled) {
		VkRenderingAttachmentInfo col = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
										 .pNext = nullptr,
										 .imageView =
											 _impl->accumulationBuffers[nextHistory].view.Get(),
										 .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
										 .resolveMode = VK_RESOLVE_MODE_NONE,
										 .resolveImageView = VK_NULL_HANDLE,
										 .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
										 .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
										 .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
										 .clearValue = {}};
		const VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
									.pNext = nullptr,
									.flags = 0,
									.renderArea = {{0, 0}, extent},
									.layerCount = 1,
									.viewMask = 0,
									.colorAttachmentCount = 1,
									.pColorAttachments = &col,
									.pDepthAttachment = nullptr,
									.pStencilAttachment = nullptr};
		vkCmdBeginRendering(_impl->current_cmd, &ri);

		vkCmdBindPipeline(_impl->current_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
						  _impl->taaPipeline.Get());
		vkCmdBindDescriptorSets(_impl->current_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
								_impl->taaPipelineLayout.Get(), 0, 1, &_impl->taaSets[nextHistory],
								0, nullptr);
		vkCmdPushConstants(_impl->current_cmd, _impl->taaPipelineLayout.Get(),
						   VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &g_TAAState.feedback);
		vkCmdDraw(_impl->current_cmd, 3, 1, 0, 0);
		ZHLN_EndRendering(_impl->current_cmd);
	}

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
		_impl->current_cmd, _impl->accumulationBuffers[nextHistory].image.Handle());

	// Write dynamic descriptor for the blit (TAA output OR raw scene)
	BlitLayout::Write(_impl->ctx.Device(), _impl->blitSets[nextHistory],
					  Vk::ImageWrite{g_TAAState.enabled
										 ? _impl->accumulationBuffers[nextHistory].view.Get()
										 : _impl->sceneColor.view.Get(),
									 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
					  Vk::SamplerWrite{_impl->defaultSampler.Get()});

	VkImage swapImg = _impl->presentation.swapchain.Get().images[_impl->current_image_index];
	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
		_impl->current_cmd, swapImg);

	if (_impl->blitPipeline.Valid()) {
		VkRenderingAttachmentInfo col = {
			.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			.pNext = nullptr,
			.imageView = _impl->presentation.swapchain.Get().views[_impl->current_image_index],
			.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			.resolveMode = VK_RESOLVE_MODE_NONE,
			.resolveImageView = VK_NULL_HANDLE,
			.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.clearValue = {}};
		const VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
									.pNext = nullptr,
									.flags = 0,
									.renderArea = {{0, 0}, extent},
									.layerCount = 1,
									.viewMask = 0,
									.colorAttachmentCount = 1,
									.pColorAttachments = &col,
									.pDepthAttachment = nullptr,
									.pStencilAttachment = nullptr};
		vkCmdBeginRendering(_impl->current_cmd, &ri);

		vkCmdBindPipeline(_impl->current_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
						  _impl->blitPipeline.Get());
		vkCmdBindDescriptorSets(_impl->current_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
								_impl->blitPipelineLayout.Get(), 0, 1,
								&_impl->blitSets[nextHistory], 0, nullptr);
		vkCmdDraw(_impl->current_cmd, 3, 1, 0, 0);

		ImGui::Render();
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), _impl->current_cmd);
		ZHLN_EndRendering(_impl->current_cmd);
	}

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR>(
		_impl->current_cmd, swapImg);
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
	if (Vk::SubmitAndPresent(submitDesc) != ZHLN_FrameResult_Ok)
		_impl->resized = true;

	_impl->frame_index = (_impl->frame_index + 1) % 2;
	_impl->historyIndex = nextHistory;
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

const char* RenderContext::GetGPUName() const {
	return _impl->ctx.PhysicalInfo().properties.properties.deviceName;
}

namespace Renderer {
void Clear(RenderContext& ctx, const JPH::Vec4& color, float depth) {
	auto* impl = ctx.GetImpl();
	if (impl->current_cmd == VK_NULL_HANDLE)
		return;

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

	VkRenderingAttachmentInfo cols[2] = {
		{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
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
	const VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
								.pNext = nullptr,
								.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT,
								.renderArea = {{0, 0}, impl->presentation.swapchain.Get().extent},
								.layerCount = 1,
								.viewMask = 0,
								.colorAttachmentCount = 2,
								.pColorAttachments = cols,
								.pDepthAttachment = &depthAtt,
								.pStencilAttachment = nullptr};

	vkCmdBeginRendering(impl->current_cmd, &ri);

	const VkViewport viewport = {.x = 0.0f,
								 .y = (float)impl->presentation.swapchain.Get().extent.height,
								 .width = (float)impl->presentation.swapchain.Get().extent.width,
								 .height = -(float)impl->presentation.swapchain.Get().extent.height,
								 .minDepth = 0.0f,
								 .maxDepth = 1.0f};
	const VkRect2D scissor = {{0, 0}, impl->presentation.swapchain.Get().extent};
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
	if (impl->current_cmd == VK_NULL_HANDLE)
		return;
	impl->drawQueue.push_back({reinterpret_cast<NativeMaterial*>(material.pipeline),
							   reinterpret_cast<NativeMesh*>(mesh.vertexBuffer), transform,
							   prevTransform, material.textureIndex});
}
} // namespace Renderer
} // namespace ZHLN
