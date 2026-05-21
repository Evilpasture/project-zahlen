// File: src/engine/Render_Init.cpp
#include "RenderInternal.hpp"
#include "SamplerBuilder.hpp"
#include "Resources.hpp"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"
#include <threading/TaskSystem.hpp>

namespace ZHLN {

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
	_impl->appName = cfg.appName;
	ZHLN_InstanceDesc inst_desc = {.app_name = {},
								   .version = VK_MAKE_API_VERSION(0, 1, 0, 0),
								   .extension_count = static_cast<uint32_t>(inst_exts.size()),
								   .severity_flags =
									   VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
									   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
								   .extensions = inst_exts.data(),
								   .enable_validation = cfg.enableValidation};
	_impl->appName.copy_to(inst_desc.app_name);

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
		.bufferDeviceAddress = VK_TRUE,
	};
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

	if (!_impl->allocator.Init(_impl->ctx)) {
		ZHLN::Panic("FATAL: Vulkan Memory Allocator (VMA) failed to initialize");
	}

	_impl->InitShadowResources();
	_impl->InitBindless();
	_impl->CompileShadowPipeline(_impl->ctx.Device(), &ZHLN_Resource_BasicVertSpv[0],
								 ZHLN_Resource_BasicVertSpv_Len);

	_impl->InitPostProcessing();

	auto* glfwWin = static_cast<GLFWwindow*>(window.GetNativeHandle());
	VkSurfaceKHR raw_surface = nullptr;
	glfwCreateWindowSurface(_impl->ctx.Instance(), glfwWin, nullptr, &raw_surface);
	_impl->surface = Vk::Surface(_impl->ctx.Instance(), raw_surface);

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
		for (auto& pool : worker.pools) {
			pool = Vk::CommandPool(_impl->ctx.Device(), _impl->ctx.PhysicalInfo().graphics_family);

			if (!pool.AllocateSecondary(256)) {
				ZHLN::Panic(
					"FATAL: Failed to pre-allocate secondary command buffers for worker threads.");
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
		_impl->meshes.clear();
		_impl->materials.clear();
	}
}

void RenderContext::Impl::InitShadowResources() {
	shadowMap = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
		allocator, ctx, {.width = SHADOW_RES, .height = SHADOW_RES},
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);

	shadowSampler = Vk::SamplerBuilder{}
						.Linear()
						.ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE)
						.DepthCompare()
						.Build(ctx.Device());

	frameUniformBuffer =
		Vk::Buffer::Create(allocator.Get(), sizeof(FrameUniforms),
						   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	lightStorageBuffer = Vk::Buffer::Create(
		allocator.Get(), sizeof(GPULight) * 128,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
}

void RenderContext::Impl::InitBindless() {
	bindlessLayout = GlobalSceneLayout::CreateLayout(ctx.Device());
	bindlessPool = GlobalSceneLayout::CreatePool(ctx.Device(), 1);
	bindlessSet =
		GlobalSceneLayout::Allocate(ctx.Device(), bindlessPool.Get(), bindlessLayout.Get());

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
	VkSampler rawSampler = VK_NULL_HANDLE;
	vkCreateSampler(ctx.Device(), &samplerInfo, nullptr, &rawSampler);
	globalSampler = Vk::Sampler(ctx.Device(), rawSampler);

	GlobalSceneLayout::Write(
		ctx.Device(), bindlessSet, Vk::SkipWrite{}, Vk::SamplerWrite{globalSampler.Get()},
		Vk::ImageWrite{shadowMap.view.Get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
		Vk::SamplerWrite{shadowSampler.Get()}, Vk::BufferWrite{frameUniformBuffer.Handle()},
		Vk::BufferWrite{lightStorageBuffer.Handle()});
}

void RenderContext::Impl::InitPostProcessing() {
	defaultSampler = Vk::SamplerBuilder{}.Linear().ClampToEdge().Build(ctx.Device());

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

	if (!taaPass.Build(ctx.Device(), taaShaders, {VK_FORMAT_R16G16B16A16_SFLOAT}, &taaPush, 1)) {
		ZHLN::Log("TAA pass build failure, continuing...");
	}

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

void RenderContext::Impl::SetupUI(GLFWwindow* window) {
	const std::array pool_sizes = {
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1000}};
	
	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.pNext = nullptr,
		.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		.maxSets = 1000,
		.poolSizeCount = (uint32_t)std::size(pool_sizes),
		.pPoolSizes = pool_sizes.data(),
	};

	VkDescriptorPool rawPool = VK_NULL_HANDLE;
	vkCreateDescriptorPool(ctx.Device(), &pool_info, nullptr, &rawPool);
	uiPool = Vk::DescriptorPool(ctx.Device(), rawPool);

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
		.DescriptorPool = uiPool.Get(),
		.DescriptorPoolSize = 0,
		.MinImageCount = 2,
		.ImageCount = 2,
		.PipelineCache = VK_NULL_HANDLE,
		.PipelineInfoMain = {
			.RenderPass = VK_NULL_HANDLE,
			.Subpass = 0,
			.MSAASamples = VK_SAMPLE_COUNT_1_BIT,
			.ExtraDynamicStates{},
			.PipelineRenderingCreateInfo = {
				.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
				.pNext = nullptr,
				.viewMask = 0,
				.colorAttachmentCount = 1,
				.pColorAttachmentFormats = &swapchainFormat,
				.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT,
				.stencilAttachmentFormat = VK_FORMAT_UNDEFINED
			},
		},
		.UseDynamicRendering = true,
		.Allocator = nullptr,
		.CheckVkResultFn = nullptr,
		.MinAllocationSize = 0,
	};
	ImGui_ImplVulkan_Init(&init_info);
}

} // namespace ZHLN