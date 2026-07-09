// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/RenderInit.cpp
#include "IBLProcessor.hpp"
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include "SMAALUTGenerator.hpp"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "engine/TTYBackend.hpp"
#include "imgui.h"

#include <Features.hpp>
#include <StagingContext.hpp>
#include <cstddef>
#include <functional>
#include <stb_image.h>
#include <threading/TaskSystem.hpp>
#include <vector>

namespace {

struct HardwareCaps {
	bool supportsDrawIndirectCount = false;
	bool supportsInt64 = false;
};

class HardwareCapsProber {
  public:
	explicit HardwareCapsProber(VkPhysicalDevice physicalDevice, uint32_t apiVersion) noexcept
		: _physicalDevice(physicalDevice), _apiVersion(apiVersion) {}

	auto ProbeInt64(bool& target) && noexcept -> HardwareCapsProber&& {
		VkPhysicalDeviceFeatures2 features2{};
		features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		vkGetPhysicalDeviceFeatures2(_physicalDevice, &features2);
		target = (features2.features.shaderInt64 == VK_TRUE);
		return std::move(*this);
	}

	auto ProbeDrawIndirectCount(bool& target) && noexcept -> HardwareCapsProber&& {
		bool hasExt =
			ZHLN::Vk::IsDeviceExtensionSupported(_physicalDevice, "VK_KHR_draw_indirect_count");
		if (hasExt || _apiVersion >= VK_API_VERSION_1_2) {
			VkPhysicalDeviceFeatures2 features2{};

			features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			VkPhysicalDeviceVulkan12Features features12{};
			features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			features2.pNext = &features12;
			vkGetPhysicalDeviceFeatures2(_physicalDevice, &features2);
			target = (features12.drawIndirectCount == VK_TRUE);
		} else {
			target = false;
		}
		return std::move(*this);
	}

  private:
	VkPhysicalDevice _physicalDevice;
	uint32_t _apiVersion;
};

HardwareCaps ProbeHardware(VkPhysicalDevice physicalDevice, uint32_t apiVersion) noexcept {
	HardwareCaps caps{};
	HardwareCapsProber(physicalDevice, apiVersion)
		.ProbeInt64(caps.supportsInt64)
		.ProbeDrawIndirectCount(caps.supportsDrawIndirectCount);
	return caps;
}

bool CheckRayTracingSupport(VkPhysicalDevice physicalDevice) noexcept {
	return ZHLN::Vk::IsDeviceExtensionSupported(physicalDevice,
												VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
		   ZHLN::Vk::IsDeviceExtensionSupported(physicalDevice, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
		   ZHLN::Vk::IsDeviceExtensionSupported(physicalDevice,
												VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
}

} // namespace

namespace ZHLN {

std::expected<void, std::string> RenderContext::Impl::InitSubsystems(const RenderConfig& cfg,
																	 int width, int height) {
	using enum Resource::ShaderID;
	if (!allocator.Init(ctx)) {
		return std::unexpected("Vulkan Memory Allocator (VMA) failed to initialize.");
	}

	if (!stagingRingBuffer.Init(allocator.Get(), ctx.Device(), ctx.GraphicsQueue(),
								ctx.PhysicalInfo().graphics_family,
								static_cast<VkDeviceSize>(64 * 1024 * 1024))) {
		return std::unexpected("Failed to initialize Staging Ring Buffer.");
	}

	if (!transferRingBuffer.Init(allocator.Get(), ctx.Device(), ctx.TransferQueue(),
								 ctx.PhysicalInfo().transfer_family,
								 static_cast<VkDeviceSize>(64 * 1024 * 1024))) {
		return std::unexpected("Failed to initialize Transfer Ring Buffer.");
	}

	bool supportsRayTracing = CheckRayTracingSupport(ctx.Physical());
	if (!supportsRayTracing || !rtCtx.Init(ctx.Device())) {
		ZHLN::Log("WARNING: Raytracing context failed to initialize. RTR will be disabled.");
	} else {
		ZHLN::Log("Raytracing context initialized successfully.");
	}

	gpuProfiler.Init(ctx.Device(), ctx.Physical(), ctx.PhysicalInfo().graphics_family);
	graphicsCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	transferCmdRing.Init(ctx.Device(), ctx.PhysicalInfo().transfer_family);

	InitShadowResources();
	InitCullingResources();
	InitBindless();

	BuildHangGpuPipeline();

	CompileShadowPipeline(ctx.Device(),
						  Resource::ShaderPair{.vertex = Resource::GetShaderProgram(Basic).vertex,
											   .fragment = Resource::shadow_frag});
	CompilePunctualShadowPipeline(ctx.Device(), Resource::GetShaderProgram(PunctualShadows));

	if (!presentation.Init(ctx, allocator, surface.Get(), width, height, cfg.vsync)) {
		return std::unexpected("Presentation Context initialization failed.");
	}

	sync = Vk::FrameSync<2>::Create(ctx.Device());
	pools = Vk::CommandPools<2>::Create(
		ctx.Device(), {.queue_family = ctx.PhysicalInfo().graphics_family, .buffers_per_pool = 1});

	InitializeSystemTextures();

	InitPostProcessing();
	SetupUI(window.IsTTY() ? nullptr : static_cast<GLFWwindow*>(window.GetNativeHandle()));

	uint32_t workerCount = TaskSystem::GetWorkerCount() + 1;
	if (workerCount == 0) {
		workerCount = 1;
	}
	workerCmds.resize(workerCount);

	for (auto& worker : workerCmds) {
		for (auto& pool : worker.pools) {
			pool = Vk::CommandPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);

			if (!pool.AllocateSecondary(256)) {
				return std::unexpected(
					"Failed to pre-allocate secondary command buffers for worker threads.");
			}
		}
	}

	// Initialize the parallel recorder with 2 slots on the Graphics Queue family and propagate
	// result codes
	auto res0 = parallelRecorder[0].Init(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	if (!res0) {
		return std::unexpected(
			std::format("Failed to allocate hardware command pools for parallel recorder [0]: {}",
						ZHLN_VkResultString(res0.error())));
	}
	auto res1 = parallelRecorder[1].Init(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	if (!res1) {
		return std::unexpected(
			std::format("Failed to allocate hardware command pools for parallel recorder [1]: {}",
						ZHLN_VkResultString(res1.error())));
	}

	deletionQueue.Init(2);
	return {};
}

namespace {

std::vector<std::string_view> GetPlatformInstanceExtensions(Window& window,
															bool enableValidation) noexcept {
	std::vector<std::string_view> inst_exts;

	if (window.IsTTY()) {
		inst_exts = TTYBackend::GetRequiredInstanceExtensions();
	} else {
		uint32_t glfwExtensionCount = 0;
		const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

		if (glfwExtensionCount > 0 && glfwExtensions != nullptr) {
			inst_exts.assign(glfwExtensions, glfwExtensions + glfwExtensionCount);
		} else {
			inst_exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
			if constexpr (isWindows) {
				inst_exts.push_back("VK_KHR_win32_surface");
			} else if constexpr (isMac) {
				inst_exts.push_back("VK_EXT_metal_surface");
			} else {
				inst_exts.push_back("VK_KHR_xcb_surface");
				inst_exts.push_back("VK_KHR_xlib_surface");
				inst_exts.push_back("VK_KHR_wayland_surface");
			}
		}
		inst_exts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
		inst_exts.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
	}

	if (enableValidation) {
		inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}

	if constexpr (isMac) {
		inst_exts.push_back("VK_KHR_portability_enumeration");
	}

	return inst_exts;
}

auto BuildFeatureChain(VkPhysicalDevice physicalDevice, const HardwareCaps& caps) noexcept {
	return Vk::FeatureChainBuilder()
		.Require<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>(
			[](auto& f) { f.swapchainMaintenance1 = VK_TRUE; })
		.Require<VkPhysicalDeviceVulkan11Features>([](auto& f) {
			f.multiview = VK_TRUE;
			f.storageBuffer16BitAccess = VK_TRUE;
			f.uniformAndStorageBuffer16BitAccess = VK_TRUE;
		})
		.Require<VkPhysicalDeviceVulkan13Features>([](auto& f) {
			f.synchronization2 = VK_TRUE;
			f.dynamicRendering = VK_TRUE;
			f.shaderDemoteToHelperInvocation = VK_TRUE;
		})
		.Require<VkPhysicalDeviceVulkan12Features>([&](auto& f) {
			f.descriptorIndexing = VK_TRUE;
			f.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
			f.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
			f.descriptorBindingPartiallyBound = VK_TRUE;
			f.runtimeDescriptorArray = VK_TRUE;
			f.bufferDeviceAddress = VK_TRUE;
			f.hostQueryReset = VK_TRUE;
			f.timelineSemaphore = VK_TRUE;
			f.drawIndirectCount = caps.supportsDrawIndirectCount ? VK_TRUE : VK_FALSE;
			f.bufferDeviceAddress = VK_TRUE;
			f.uniformAndStorageBuffer8BitAccess = VK_TRUE;
		})
		.Optional<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(
			physicalDevice, [](auto& f) { f.accelerationStructure = VK_TRUE; })
		.Optional<VkPhysicalDeviceRayQueryFeaturesKHR>(physicalDevice,
													   [](auto& f) { f.rayQuery = VK_TRUE; })
		.Require<VkPhysicalDeviceFeatures2>([&](auto& f) {
			f.features.multiDrawIndirect = VK_TRUE;
			f.features.samplerAnisotropy = VK_TRUE;
			f.features.drawIndirectFirstInstance = VK_TRUE;
			f.features.shaderInt64 = caps.supportsInt64 ? VK_TRUE : VK_FALSE;
			f.features.imageCubeArray = VK_TRUE;
		})
		.Build();
}

std::vector<const char*> GetDeviceExtensions(VkPhysicalDevice physicalDevice) noexcept {
	std::vector<const char*> dev_exts = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME, "VK_EXT_robustness2"};

	if (CheckRayTracingSupport(physicalDevice)) {
		dev_exts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
		dev_exts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
		dev_exts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
	}

	if constexpr (isMac) {
		dev_exts.push_back("VK_KHR_portability_subset");
	}
	return dev_exts;
}

Vk::Context CreateLogicalDevice(VkInstance instance, VkSurfaceKHR surface,
								const ZHLN_PhysicalDeviceInfo& physical,
								const std::vector<const char*>& extensions,
								const VkPhysicalDeviceFeatures2* features,
								bool enableValidation) noexcept {
	return Vk::Context::Create(instance, surface, physical,
							   {.physical = &physical,
								.extensions = extensions.data(),
								.extension_count = static_cast<uint32_t>(extensions.size()),
								.features = features,
								.enable_validation = enableValidation});
}

// Generic helper for standard PostProcessPass compilation
template <typename LayoutT>
void BuildPassHelper(RenderContext::Impl* self, Vk::PostProcessPass<LayoutT>& pass,
					 const char* passName, ShaderStageSource vs, ShaderStageSource ps,
					 std::initializer_list<VkFormat> colorFormats,
					 const VkPushConstantRange* pushConstants = nullptr, uint32_t pushCount = 0,
					 bool additive = false) noexcept {

	auto shaders = self->LoadAndCreateShaders(vs, ps);

	if (pass.Build(self->ctx.Device(), shaders, colorFormats, pushConstants, pushCount, additive)) {
		ZHLN::Log("[Shader Reload] {} Pipeline built successfully.", passName);
	} else {
		ZHLN::Log("[Shader Reload] {} Pipeline failed to build.", passName);
	}
}

// Generic helper for specialized pipeline variants (e.g. Reflections)
template <typename LayoutT>
void BuildPassVariants(RenderContext::Impl* self, Vk::PostProcessPass<LayoutT>& pass,
					   const char* passName, ShaderStageSource vs, ShaderStageSource ps,
					   std::initializer_list<VkFormat> colorFormats,
					   std::span<const VkSpecializationInfo> specInfos,
					   const VkPushConstantRange* pushConstants = nullptr, uint32_t pushCount = 0,
					   bool additive = false) noexcept {

	auto shaders = self->LoadAndCreateShaders(vs, ps);

	if (pass.BuildVariants(self->ctx.Device(), shaders, colorFormats, pushConstants, pushCount,
						   specInfos, additive)) {
		ZHLN::Log("[Shader Reload] {} Pass variants built successfully.", passName);
	} else {
		ZHLN::Log("[Shader Reload] {} Pass variants failed to build.", passName);
	}
}

} // namespace

Vk::ShaderStages RenderContext::Impl::LoadAndCreateShaders(ShaderStageSource vs,
														   ShaderStageSource ps) const noexcept {
	const void* vs_code = nullptr;
	size_t vs_size = 0;
	const void* ps_code = nullptr;
	size_t ps_size = 0;
	std::vector<uint32_t> disk_vs;
	std::vector<uint32_t> disk_ps;

	LoadShaderData(vs, vs_code, vs_size, disk_vs);
	LoadShaderData(ps, ps_code, ps_size, disk_ps);

	return Vk::ShaderStages::Create(
		ctx.Device(), {.code = Vk::AsSpirV(vs_code), .size = vs_size, .entry_point = vs.entryPoint},
		{.code = Vk::AsSpirV(ps_code), .size = ps_size, .entry_point = ps.entryPoint});
}

Vk::Pipeline
RenderContext::Impl::LoadAndCreateComputeShader(ShaderStageSource cs,
												VkPipelineLayout layout) const noexcept {
	const void* cs_code = nullptr;
	size_t cs_size = 0;
	std::vector<uint32_t> disk_cs;

	LoadShaderData(cs, cs_code, cs_size, disk_cs);

	auto p_res = Vk::ComputePipelineBuilder()
					 .Shader(Vk::AsSpirV(cs_code), cs_size, cs.entryPoint)
					 .Layout(layout)
					 .Build(ctx.Device());

	if (!p_res) {
		return {};
	}
	return std::move(*p_res);
}

void RenderContext::Impl::WatchPipeline(const char* vsPath, const char* psPath,
										std::function<void()> rebuild_fn) noexcept {
	if constexpr (isDev) {
		RegisterShaderWatcher(vsPath, rebuild_fn);
		RegisterShaderWatcher(psPath, std::move(rebuild_fn));
	}
}

RenderContext::RenderContext(PrivateToken, std::unique_ptr<Impl> impl) noexcept
	: _impl(std::move(impl)) {}

std::expected<std::unique_ptr<RenderContext>, std::string>
RenderContext::Create(Window& window, const RenderConfig& cfg) noexcept {
	auto impl = std::make_unique<Impl>(window);
	impl->appName = cfg.appName;

	// Phase 1: Instance extensions selection
	auto inst_exts = GetPlatformInstanceExtensions(window, cfg.enableValidation);

	// Phase 2: Instance creation
	VkInstance instance = Vk::CreateInstance(impl->appName.c_str(), VK_MAKE_API_VERSION(0, 1, 0, 0),
											 inst_exts, cfg.enableValidation);
	if (instance == VK_NULL_HANDLE) {
		return std::unexpected("Failed to create Vulkan Instance.");
	}

	// Phase 3: Surface creation (Before device selection for windowed/GLFW)
	int width = 0;
	int height = 0;
	auto* raw_surface =
		static_cast<VkSurfaceKHR>(window.CreateVulkanSurface(instance, nullptr, width, height));
	if (!window.IsTTY() && raw_surface == VK_NULL_HANDLE) {
		return std::unexpected("Failed to create GLFW Vulkan surface.");
	}

	// Phase 4: Device selection
	auto physicalInfo = Vk::SelectDevice(instance, raw_surface);
	if (physicalInfo.handle == VK_NULL_HANDLE) {
		return std::unexpected(
			"Failed to select a suitable physical device supporting the required Vulkan features.");
	}

	// Phase 5: Surface creation (After device selection for TTY)
	if (window.IsTTY()) {
		raw_surface = static_cast<VkSurfaceKHR>(
			window.CreateVulkanSurface(instance, physicalInfo.handle, width, height));
		if (raw_surface == VK_NULL_HANDLE) {
			return std::unexpected("Failed to create TTY Vulkan Surface.");
		}
	}

	impl->surface = Vk::Surface(instance, raw_surface);

	// Phase 6: Capability detection
	HardwareCaps caps =
		ProbeHardware(physicalInfo.handle, physicalInfo.properties.properties.apiVersion);

	// Phase 7: Feature negotiation
	auto features = BuildFeatureChain(physicalInfo.handle, caps);

	// Phase 8: Device extension building
	auto dev_exts = GetDeviceExtensions(physicalInfo.handle);

	// Phase 9: Device creation
	impl->ctx = CreateLogicalDevice(instance, raw_surface, physicalInfo, dev_exts,
									features.GetRoot(), cfg.enableValidation);
	if (!impl->ctx.Valid()) {
		return std::unexpected("Failed to create Vulkan Logical Device.");
	}

	// Phase 10: Subsystem initialization
	auto status = impl->InitSubsystems(cfg, width, height);
	if (!status) {
		return std::unexpected(status.error());
	}

	return std::make_unique<RenderContext>(PrivateToken{}, std::move(impl));
}

RenderContext::~RenderContext() {
	if (_impl && (_impl->ctx.Device() != nullptr)) {
		vkDeviceWaitIdle(_impl->ctx.Device());
		_impl->stagingContext.reset();

		// --- SAFETY: Only shut down ImGui if it was actually initialized ---
		if (!_impl->window.IsTTY()) {
			ImGui_ImplVulkan_Shutdown();
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext();
		}
	}
}

void RenderContext::CheckShaderReload() noexcept {
	if constexpr (isDev) {
		_impl->CheckShaderWatchers();
	}
}

void RenderContext::Impl::BuildSkinningPipeline() {
	VkPushConstantRange pushRange = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(SkinningConstants)};

	ZHLN_PipelineLayoutDesc layout_desc = {.set_layouts = nullptr,
										   .set_layout_count = 0,
										   .push_constants = &pushRange,
										   .push_constant_count = 1};

	skinningPass.pipelineLayout =
		Vk::PipelineLayout(ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &layout_desc));

	skinningPass.pipeline = LoadAndCreateComputeShader({.path = SHADER_SKINNING_HLSL_CS_PATH,
														.fallback = Resource::skinning_comp,
														.entryPoint = "CSMain"},
													   skinningPass.pipelineLayout.Get());

	if (skinningPass.pipeline.Valid()) {
		ZHLN::Log("[Shader Reload] GPU Skinning Compute pipeline built successfully.");
	} else {
		ZHLN::Log("[Shader Reload] GPU Skinning Compute pipeline failed to build.");
	}
}

void RenderContext::Impl::RecreatePunctualShadowViews() noexcept {
	punctualShadowViews.clear();
	punctualShadowViews.resize(MAX_PUNCTUAL_LIGHTS);
	for (uint32_t i = 0; i < MAX_PUNCTUAL_LIGHTS; ++i) {
		VkImageViewCreateInfo viewInfo = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.pNext = {},
			.flags = {},
			.image = graphResources.shadowAtlas.image.Handle(),
			.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
			.format = VK_FORMAT_D32_SFLOAT,
			.components = {},
			.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
								 .baseMipLevel = 0,
								 .levelCount = 1,
								 .baseArrayLayer = i * 6,
								 .layerCount = 6},
		};

		VkImageView rawView = VK_NULL_HANDLE;
		vkCreateImageView(ctx.Device(), &viewInfo, nullptr, &rawView);
		punctualShadowViews[i] = Vk::ImageView(ctx.Device(), rawView);
	}
}

void RenderContext::Impl::InitShadowResources() {
	shadowSampler = Vk::SamplerBuilder{}
						.Linear()
						.ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE)
						.DepthCompare()
						.Build(ctx.Device());

	graphResources.shadowMap = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
		allocator, ctx, {.width = SHADOW_RES, .height = SHADOW_RES},
		{.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		 .arrayLayers = NUM_CASCADES});

	shadowCascadeViews.resize(NUM_CASCADES);
	for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
		shadowCascadeViews[i] = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(
			ctx.Device(), graphResources.shadowMap.image.Handle(), i, 1);
	}

	// 1. Allocate Shadow Atlas
	graphResources.shadowAtlas = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
		allocator, ctx, {.width = 1024, .height = 1024},
		{.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		 .arrayLayers = 24});

	// 2. Pre-allocate Views using clean C++ helper templates!
	shadowAtlasCubeView = Vk::CreateViewCubeArray<VK_FORMAT_D32_SFLOAT>(
		ctx.Device(), graphResources.shadowAtlas.image.Handle(), 24);
	shadowAtlas2DView = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(
		ctx.Device(), graphResources.shadowAtlas.image.Handle(), 0, 24);

	Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			cmd, graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			cmd, graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			cmd, graphResources.shadowAtlas.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			cmd, graphResources.shadowAtlas.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
	});

	RecreatePunctualShadowViews();

	frameUniformBuffers =
		CreateDoubleBuffered(allocator, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
							 VMA_MEMORY_USAGE_CPU_TO_GPU);

	lightStorageBuffers =
		CreateDoubleBuffered(allocator, sizeof(GPULight) * 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
							 VMA_MEMORY_USAGE_CPU_TO_GPU);

	shadowIndirectBuffers =
		CreateDoubleBuffered(allocator, sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances * 8,
							 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
}

void RenderContext::Impl::InitCullingResources() {
	using enum Resource::ShaderID;
	Vk::AllocateDoubleBufferedSet<CullingLayout>(ctx.Device(), cullingLayout, cullingPool,
												 cullingSets);

	for (int i = 0; i < 2; ++i) {
		instanceDataBuffers[i] =
			Vk::Buffer::Create(allocator.Get(), sizeof(InstanceData) * kGpuCullingMaxInstances,
							   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

		indirectCommandsBuffers[i] = Vk::Buffer::Create(
			allocator.Get(), sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
			VMA_MEMORY_USAGE_GPU_ONLY);

		// Bind all three targets to the culling descriptor set
		CullingLayout::Write(ctx.Device(), cullingSets[i],
							 Vk::BufferWrite{.buffer = instanceDataBuffers[i].Handle()},
							 Vk::BufferWrite{.buffer = indirectCommandsBuffers[i].Handle()});
	}

	constexpr uint32_t kCullingPushSize = sizeof(float) * 4 * 6 + sizeof(uint32_t) * 4;
	VkPushConstantRange cullingPush = {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset = 0,
		.size = kCullingPushSize,
	};

	auto cullingShader = Vk::CreateShaderDesc(Resource::culling_comp);

	if (!cullingPass.Build(ctx.Device(), cullingLayout.Get(), cullingShader, &cullingPush, 1)) {
		ZHLN::Panic("FATAL: Failed to compile or build the Compute Culling Pipeline.");
	}

	constexpr auto numClusters = static_cast<size_t>(16 * 9 * 24); // 3456

	clusterBoundsBuffer =
		Vk::Buffer::Create(allocator.Get(),
						   sizeof(
							   /* Don't remove the struct tag here or else the compiler will think
								* it's Resource::ShaderID::ClusterBounds. */
							   struct ClusterBounds) *
							   numClusters,
						   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
						   VMA_MEMORY_USAGE_GPU_ONLY);

	Vk::AllocateDoubleBufferedSet<ClusterCullingLayout>(ctx.Device(), clusterCullingDescLayout,
														clusterCullingPool, clusterCullingSets);

	for (int i = 0; i < 2; ++i) {
		clusterGridBuffers[i] =
			Vk::Buffer::Create(allocator.Get(), sizeof(ClusterVolume) * numClusters,
							   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		lightIndexListBuffers[i] =
			Vk::Buffer::Create(allocator.Get(), sizeof(uint32_t) * numClusters * 64,
							   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
		globalCounterBuffers[i] = Vk::Buffer::Create(allocator.Get(), sizeof(uint32_t),
													 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
														 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
													 VMA_MEMORY_USAGE_GPU_ONLY);

		ClusterCullingLayout::Write(
			ctx.Device(), clusterCullingSets[i],
			Vk::BufferWrite{.buffer = clusterBoundsBuffer.Handle()},
			Vk::BufferWrite{.buffer = clusterGridBuffers[i].Handle()},
			Vk::BufferWrite{.buffer = lightIndexListBuffers[i].Handle()},
			Vk::BufferWrite{.buffer = globalCounterBuffers[i].Handle()},
			Vk::BufferWrite{.buffer = frameUniformBuffers[i].Handle()}, // Binding 4: Frame UBO
			Vk::BufferWrite{.buffer = lightStorageBuffers[i].Handle()}	// Binding 5: Lights SSBO
		);
	}

	auto bDesc = Vk::CreateShaderDesc(Resource::GetShaderProgram(ClusterBounds).vertex);
	auto cDesc = Vk::CreateShaderDesc(Resource::GetShaderProgram(ClusterCulling).vertex);
	if (!clusterBoundsPass.Build(ctx.Device(), clusterCullingDescLayout.Get(), bDesc)) {
		ZHLN::Panic("FATAL: Failed to build Cluster Bounds Pass!");
	}
	if (!clusterCullingPass.Build(ctx.Device(), clusterCullingDescLayout.Get(), cDesc)) {
		ZHLN::Panic("FATAL: Failed to build Cluster Culling Pass!");
	}

	// --- PRE-ALLOCATE STATIC TLAS RESOURCES ---
	if (rtCtx.Valid()) {
		ZHLN_AccelerationStructureSizes tlasSizes;
		rtCtx.GetTlasSizes(kGpuCullingMaxInstances, tlasSizes);

		for (int i = 0; i < 2; ++i) {
			// Allocate TLAS Backing Storage
			tlasBuffer[i] =
				Vk::Buffer::Create(allocator.Get(), tlasSizes.acceleration_structure_size,
								   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
									   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
								   VMA_MEMORY_USAGE_GPU_ONLY);

			// Allocate TLAS Build Scratch Memory
			tlasScratchBuffer[i] = Vk::Buffer::Create(allocator.Get(), tlasSizes.build_scratch_size,
													  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
														  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
													  VMA_MEMORY_USAGE_GPU_ONLY);

			// Create Top-Level Acceleration Structure Handle
			tlas[i] = rtCtx.CreateAS(tlasBuffer[i].Handle(), tlasSizes.acceleration_structure_size,
									 ZHLN_AS_TYPE_TOP_LEVEL);

			// Allocate GPU Instance Buffer (Reads instances into the AS builder)
			tlasInstanceBuffers[i] = Vk::Buffer::Create(
				allocator.Get(),
				sizeof(VkAccelerationStructureInstanceKHR) * kGpuCullingMaxInstances,
				VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
					VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
					VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VMA_MEMORY_USAGE_GPU_ONLY);

			// Allocate Host-Visible Staging Buffer
			tlasStagingBuffers[i] = Vk::Buffer::Create(
				allocator.Get(),
				sizeof(VkAccelerationStructureInstanceKHR) * kGpuCullingMaxInstances,
				VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		}
	}

	BuildSkinningPipeline();
	if constexpr (isDev) {
		// Skinning
		RegisterShaderWatcher(SHADER_SKINNING_HLSL_CS_PATH, [this]() { BuildSkinningPipeline(); });
	}
}

void RenderContext::Impl::InitSkeletalAnimationResources() {
	// Allocate our global Joint storage buffer (Supports 8192 dynamic matrices)
	JPH::Array<JPH::Mat44> identities(8192, JPH::Mat44::sIdentity());
	for (int i = 0; i < 2; ++i) {
		jointBuffers[i] = Vk::Buffer::Create(allocator.Get(), sizeof(JPH::Mat44) * 8192,
											 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
												 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
											 VMA_MEMORY_USAGE_CPU_TO_GPU);

		// Upload identity matrices initially
		auto mapped = jointBuffers[i].Map();
		std::memcpy(mapped.data, identities.data(), identities.size() * sizeof(JPH::Mat44));
	}

	morphDeltasBuffer = Vk::Buffer::Create(allocator.Get(), sizeof(float) * 4 * 1000000,
										   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
											   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
										   VMA_MEMORY_USAGE_CPU_TO_GPU);
}

void RenderContext::Impl::InitLightingLUTs() {
	stagingContext = std::make_unique<Vk::StagingContext>(allocator, ctx);
	stagingContext->Begin();

	iblPayload = Vk::IBLProcessor::Bake(*this, *stagingContext);

	ZHLN::Log("[IBL] Uploading Linearly Transformed Cosines (LTC) LUTs...");

	VkImageCreateInfo ltcInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = {},
		.flags = {},
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R16G16B16A16_SFLOAT,
		.extent = {.width = 64, .height = 64, .depth = 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = {},
		.pQueueFamilyIndices = {},
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	using namespace Resource;
	const size_t matRawSize = ltc_mat.size() - 128;
	const size_t ampRawSize = ltc_amp.size() - 128;

	auto ltcStaging =
		Vk::Buffer::Create(allocator.Get(), matRawSize + ampRawSize,
						   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

	auto mapped = ltcStaging.Map();
	char* const stageBasePtr = static_cast<char*>(mapped.data);

	struct LUTUploadItem {
		Vk::Image* targetImage;
		std::span<const uint8_t> rawData;
		size_t rawSize;
		size_t bufferOffset;
	};

	std::array<LUTUploadItem, 2> uploads = {
		{{.targetImage = &ltcMatImage, .rawData = ltc_mat, .rawSize = matRawSize, .bufferOffset = 0},
		 {.targetImage = &ltcAmpImage,
		  .rawData = ltc_amp,
		  .rawSize = ampRawSize,
		  .bufferOffset = matRawSize}}};

	for (const auto& item : uploads) {
		*item.targetImage = Vk::Image::Create(allocator.Get(), ltcInfo, VMA_MEMORY_USAGE_GPU_ONLY);

		std::memcpy(stageBasePtr + item.bufferOffset, item.rawData.data() + 128, item.rawSize);

		stagingContext->UploadImage2DBuffer(item.targetImage->Handle(), 64, 64, 1,
											ltcStaging.Handle(), item.bufferOffset);
	}

	stagingContext->AddBuffer(std::move(ltcStaging));
	stagingContext->ExecuteAsync();

	// 4. Create image views safely outside the async execution line
	ltcMatView = Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcMatImage.Handle());
	ltcAmpView = Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcAmpImage.Handle());
}

void RenderContext::Impl::InitBindless() {
	Vk::AllocateDoubleBufferedSet<GlobalSceneLayout>(ctx.Device(), bindlessLayout, bindlessPool,
													 bindlessSets);

	VkSamplerCreateInfo samplerInfo = {
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
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
	// Create the math/environment sampler (ClampToEdge prevents wrap-around artifacts)
	clampSampler = Vk::SamplerBuilder{}.Linear().ClampToEdge().Build(ctx.Device());

	InitSkeletalAnimationResources();
	InitLightingLUTs();

	ZHLN::Log("[RenderInit] Pre-allocating persistently mapped Double-Buffered Debug VBOs...");
	size_t maxDebugVerts = 500000; // Large enough for dense wireframes (~32MB)
	size_t bufferSize = maxDebugVerts * (sizeof(VertexPosition) + sizeof(VertexAttributes));
	for (int i = 0; i < 2; ++i) {
		auto gpu_buf = Vk::Buffer::Create(allocator.Get(), bufferSize,
										  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
											  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
										  VMA_MEMORY_USAGE_CPU_TO_GPU);

		VkDeviceAddress address = Vk::GetBufferDeviceAddress(ctx.Device(), gpu_buf.Handle());
		debugMeshHandles[i] = meshPool.Create(std::move(gpu_buf), maxDebugVerts, address);
	}

	// Update global descriptor bindings
	Vk::DescriptorUpdater bindlessRegistry;
	for (int i = 0; i < 2; ++i) {
		bindlessRegistry.BindSampler(1, globalSampler.Get());
		bindlessRegistry.BindUniformBuffer(2, frameUniformBuffers[i].Handle());
		bindlessRegistry.BindStorageBuffer(3, lightStorageBuffers[i].Handle());
		bindlessRegistry.BindStorageBuffer(4, instanceDataBuffers[i].Handle());
		bindlessRegistry.BindStorageBuffer(5, jointBuffers[i].Handle());
		bindlessRegistry.BindStorageBuffer(6, jointBuffers[1 - i].Handle());
		bindlessRegistry.BindStorageBuffer(7, morphDeltasBuffer.Handle());

		bindlessRegistry.BindSampledImage(8, iblPayload.prefilteredView.Get(), VK_NULL_HANDLE,
										  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		bindlessRegistry.BindSampledImage(9, iblPayload.brdfLutView.Get(), VK_NULL_HANDLE,
										  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		bindlessRegistry.BindSampler(10, clampSampler.Get());

		bindlessRegistry.UpdateSet(ctx.Device(), bindlessSets[i]);
	}
}
using enum Resource::ShaderID;
void RenderContext::Impl::BuildTAAPipeline() {

	VkPushConstantRange taaPush = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float)};

	BuildPassHelper(this, taaPass, "TAA",
					{.path = SHADER_TAA_HLSL_VS_PATH,
					 .fallback = Resource::GetShaderProgram(Taa).vertex,
					 .entryPoint = "VSMain"},
					{.path = SHADER_TAA_HLSL_PS_PATH,
					 .fallback = Resource::GetShaderProgram(Taa).fragment,
					 .entryPoint = "PSMain"},
					{VK_FORMAT_R16G16B16A16_SFLOAT}, &taaPush, 1);
}

void RenderContext::Impl::BuildFXAAPipeline() {
	VkPushConstantRange fxaaPush = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float) * 6};

	BuildPassHelper(this, fxaaPass, "FXAA",
					{.path = SHADER_FXAA_HLSL_VS_PATH,
					 .fallback = Resource::GetShaderProgram(Fxaa).vertex,
					 .entryPoint = "VSMain"},
					{.path = SHADER_FXAA_HLSL_PS_PATH,
					 .fallback = Resource::GetShaderProgram(Fxaa).fragment,
					 .entryPoint = "PSMain"},
					{VK_FORMAT_R16G16B16A16_SFLOAT}, &fxaaPush, 1);
}

void RenderContext::Impl::BuildSMAAPipeline() {
	VkPushConstantRange smaaPush = {.stageFlags =
										VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
									.offset = 0,
									.size = sizeof(float) * 4};

	BuildPassHelper(this, smaaEdgePass, "SMAA Edge Detection",
					{.path = SHADER_SMAA_EDGE_VS_PATH,
					 .fallback = Resource::GetShaderProgram(SmaaEdge).vertex,
					 .entryPoint = "SmaaEdgeVS"},
					{.path = SHADER_SMAA_EDGE_PS_PATH,
					 .fallback = Resource::GetShaderProgram(SmaaEdge).fragment,
					 .entryPoint = "SmaaEdgePS"},
					{VK_FORMAT_R8G8_UNORM}, &smaaPush, 1);

	BuildPassHelper(this, smaaWeightPass, "SMAA Blending Weight",
					{.path = SHADER_SMAA_WEIGHT_VS_PATH,
					 .fallback = Resource::GetShaderProgram(SmaaWeight).vertex,
					 .entryPoint = "SmaaWeightVS"},
					{.path = SHADER_SMAA_WEIGHT_PS_PATH,
					 .fallback = Resource::GetShaderProgram(SmaaWeight).fragment,
					 .entryPoint = "SmaaWeightPS"},
					{VK_FORMAT_R8G8B8A8_UNORM}, &smaaPush, 1);

	BuildPassHelper(this, smaaBlendPass, "SMAA Neighborhood Blend",
					{.path = SHADER_SMAA_BLEND_VS_PATH,
					 .fallback = Resource::GetShaderProgram(SmaaBlend).vertex,
					 .entryPoint = "SmaaBlendVS"},
					{.path = SHADER_SMAA_BLEND_PS_PATH,
					 .fallback = Resource::GetShaderProgram(SmaaBlend).fragment,
					 .entryPoint = "SmaaBlendPS"},
					{VK_FORMAT_R16G16B16A16_SFLOAT}, &smaaPush, 1);
}

void RenderContext::Impl::BuildAmbientPipeline() {
	VkPushConstantRange ppPush = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 192};

	BuildPassHelper(this, ambientPass, "Ambient",
					{.path = SHADER_AMBIENT_HLSL_VS_PATH,
					 .fallback = Resource::GetShaderProgram(Ambient).vertex,
					 .entryPoint = "VSMain"},
					{.path = SHADER_AMBIENT_HLSL_PS_PATH,
					 .fallback = Resource::GetShaderProgram(Ambient).fragment,
					 .entryPoint = "PSMain"},
					{VK_FORMAT_R16G16B16A16_SFLOAT}, &ppPush, 1);
}

void RenderContext::Impl::BuildLightingPipeline() {
	VkPushConstantRange ppPush = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 192};

	struct SpecData {
		int enableRTR;
	};
	std::array<VkSpecializationMapEntry, 1> specEntries = {
		{{.constantID = 0, .offset = offsetof(SpecData, enableRTR), .size = sizeof(int)}}};

	std::array<SpecData, 2> variants = {{{.enableRTR = 0}, {.enableRTR = 1}}};
	std::array<VkSpecializationInfo, 2> specInfos{};
	for (int i = 0; i < 2; ++i) {
		specInfos[i] = {.mapEntryCount = 1,
						.pMapEntries = specEntries.data(),
						.dataSize = sizeof(SpecData),
						.pData = &variants[i]};
	}

	bool hasRt = rtCtx.Valid();
	const char* vsPath = hasRt ? SHADER_LIGHTING_HLSL_VS_PATH : SHADER_LIGHTING_NORT_HLSL_VS_PATH;
	const char* psPath = hasRt ? SHADER_LIGHTING_HLSL_PS_PATH : SHADER_LIGHTING_NORT_HLSL_PS_PATH;

	// Declared as const unsigned char* to match ShaderStageSource
	auto vsSpan = hasRt ? Resource::GetShaderProgram(Lighting).vertex
						: Resource::GetShaderProgram(LightingNort).vertex;
	auto psSpan = hasRt ? Resource::GetShaderProgram(Lighting).fragment
						: Resource::GetShaderProgram(LightingNort).fragment;

	BuildPassVariants(this, lightingPass, "Lighting",
					  {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
					  {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"},
					  {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos, &ppPush, 1);
}

void RenderContext::Impl::BuildReflectionPipelines() {
	VkPushConstantRange ppPush = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = 192};

	struct SpecData {
		int enableSSR;
		int enableRTR;
	};
	std::array<VkSpecializationMapEntry, 2> specEntries = {
		{{.constantID = 0, .offset = offsetof(SpecData, enableSSR), .size = sizeof(int)},
		 {.constantID = 1, .offset = offsetof(SpecData, enableRTR), .size = sizeof(int)}}};

	std::array<SpecData, 4> variants = {{{.enableSSR = 0, .enableRTR = 0},
										 {.enableSSR = 1, .enableRTR = 0},
										 {.enableSSR = 0, .enableRTR = 1},
										 {.enableSSR = 1, .enableRTR = 1}}};
	std::array<VkSpecializationInfo, 4> specInfos{};
	for (int i = 0; i < 4; ++i) {
		specInfos[i] = {.mapEntryCount = 2,
						.pMapEntries = specEntries.data(),
						.dataSize = sizeof(SpecData),
						.pData = &variants[i]};
	}

	bool hasRt = rtCtx.Valid();
	const char* vsPath =
		hasRt ? SHADER_REFLECTION_HLSL_VS_PATH : SHADER_REFLECTION_NORT_HLSL_VS_PATH;
	const char* psPath =
		hasRt ? SHADER_REFLECTION_HLSL_PS_PATH : SHADER_REFLECTION_NORT_HLSL_PS_PATH;

	// Declared as const unsigned char* to match ShaderStageSource
	auto vsSpan = hasRt ? Resource::GetShaderProgram(Reflection).vertex
						: Resource::GetShaderProgram(Resource::ShaderID::ReflectionNort).vertex;
	auto psSpan = hasRt ? Resource::GetShaderProgram(Reflection).fragment
						: Resource::GetShaderProgram(Resource::ShaderID::ReflectionNort).fragment;

	BuildPassVariants(this, reflectionPass, "Reflection",
					  {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
					  {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"},
					  {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos, &ppPush, 1);
}

void RenderContext::Impl::BuildBloomPipelines() {
	using enum Resource::ShaderID;
	BuildPassHelper(this, bloomThresholdPass, "Bloom Threshold",
					{.path = SHADER_BLOOM_THRESHOLD_HLSL_VS_PATH,
					 .fallback = Resource::GetShaderProgram(BloomThreshold).vertex,
					 .entryPoint = "VSMain"},
					{.path = SHADER_BLOOM_THRESHOLD_HLSL_PS_PATH,
					 .fallback = Resource::GetShaderProgram(BloomThreshold).fragment,
					 .entryPoint = "PSMain"},
					{VK_FORMAT_R16G16B16A16_SFLOAT});

	VkPushConstantRange kawasePush = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = 16 // Holds Mode (int), Width (float), Height (float), Padding (float)
	};

	for (int i = 0; i < 3; ++i) {
		std::string downName = std::format("Bloom Downsample {}", i);
		BuildPassHelper(this, bloomDownPass[i], downName.c_str(),
						{.path = SHADER_BLOOM_BLUR_HLSL_VS_PATH,
						 .fallback = Resource::GetShaderProgram(BloomBlur).vertex,
						 .entryPoint = "VSMain"},
						{.path = SHADER_BLOOM_BLUR_HLSL_PS_PATH,
						 .fallback = Resource::GetShaderProgram(BloomBlur).fragment,
						 .entryPoint = "PSMain"},
						{VK_FORMAT_R16G16B16A16_SFLOAT}, &kawasePush, 1);

		std::string upName = std::format("Bloom Upsample {}", i);
		BuildPassHelper(this, bloomUpPass[i], upName.c_str(),
						{.path = SHADER_BLOOM_BLUR_HLSL_VS_PATH,
						 .fallback = Resource::GetShaderProgram(BloomBlur).vertex,
						 .entryPoint = "VSMain"},
						{.path = SHADER_BLOOM_BLUR_HLSL_PS_PATH,
						 .fallback = Resource::GetShaderProgram(BloomBlur).fragment,
						 .entryPoint = "PSMain"},
						{VK_FORMAT_R16G16B16A16_SFLOAT}, &kawasePush, 1);
	}
}

void RenderContext::Impl::BuildBlitPipeline() {
	VkPushConstantRange blitPush = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(BlitPushConstants),
	};

	BuildPassHelper(this, blitPass, "Blit",
					{.path = SHADER_BLIT_HLSL_VS_PATH,
					 .fallback = Resource::GetShaderProgram(Blit).vertex,
					 .entryPoint = "VSMain"},
					{.path = SHADER_BLIT_HLSL_PS_PATH,
					 .fallback = Resource::GetShaderProgram(Blit).fragment,
					 .entryPoint = "PSMain"},
					{presentation.swapchain.Get().format}, &blitPush, 1);
}

void RenderContext::Impl::InitPostProcessing() {
	defaultSampler = Vk::SamplerBuilder{}.Linear().ClampToEdge().Build(ctx.Device());
	pointSampler = Vk::SamplerBuilder{}.Nearest().ClampToEdge().Build(ctx.Device());

	// Unified build and watch registration
	RegisterPipeline({.name = "TAA",
					  .build = [this]() { BuildTAAPipeline(); },
					  .watchPaths = {SHADER_TAA_HLSL_VS_PATH, SHADER_TAA_HLSL_PS_PATH}});
	RegisterPipeline({.name = "FXAA",
					  .build = [this]() { BuildFXAAPipeline(); },
					  .watchPaths = {SHADER_FXAA_HLSL_VS_PATH, SHADER_FXAA_HLSL_PS_PATH}});
	RegisterPipeline({.name = "SMAA",
					  .build = [this]() { BuildSMAAPipeline(); },
					  .watchPaths = {SHADER_SMAA_EDGE_VS_PATH, SHADER_SMAA_EDGE_PS_PATH,
									 SHADER_SMAA_WEIGHT_VS_PATH, SHADER_SMAA_WEIGHT_PS_PATH,
									 SHADER_SMAA_BLEND_VS_PATH, SHADER_SMAA_BLEND_PS_PATH}});
	RegisterPipeline({.name = "Ambient",
					  .build = [this]() { BuildAmbientPipeline(); },
					  .watchPaths = {SHADER_AMBIENT_HLSL_VS_PATH, SHADER_AMBIENT_HLSL_PS_PATH}});
	RegisterPipeline(
		{.name = "Lighting",
		 .build = [this]() { BuildLightingPipeline(); },
		 .watchPaths = {SHADER_LIGHTING_HLSL_VS_PATH, SHADER_LIGHTING_HLSL_PS_PATH,
						SHADER_LIGHTING_NORT_HLSL_VS_PATH, SHADER_LIGHTING_NORT_HLSL_PS_PATH}});
	RegisterPipeline(
		{.name = "Reflection",
		 .build = [this]() { BuildReflectionPipelines(); },
		 .watchPaths = {SHADER_REFLECTION_HLSL_VS_PATH, SHADER_REFLECTION_HLSL_PS_PATH,
						SHADER_REFLECTION_NORT_HLSL_VS_PATH, SHADER_REFLECTION_NORT_HLSL_PS_PATH}});
	RegisterPipeline(
		{.name = "Bloom",
		 .build = [this]() { BuildBloomPipelines(); },
		 .watchPaths = {SHADER_BLOOM_THRESHOLD_HLSL_VS_PATH, SHADER_BLOOM_THRESHOLD_HLSL_PS_PATH,
						SHADER_BLOOM_BLUR_HLSL_VS_PATH, SHADER_BLOOM_BLUR_HLSL_PS_PATH}});
	RegisterPipeline({.name = "Blit",
					  .build = [this]() { BuildBlitPipeline(); },
					  .watchPaths = {SHADER_BLIT_HLSL_VS_PATH, SHADER_BLIT_HLSL_PS_PATH}});

	ZHLN::Array<uint32_t> smaaAreaPixels(static_cast<size_t>(160 * 560));
	ZHLN::PBR::FillSmaaAreaTex(smaaAreaPixels);
	ZHLN::Array<uint32_t> smaaSearchPixels(static_cast<size_t>(64 * 16));
	ZHLN::PBR::FillSmaaSearchTex(smaaSearchPixels);
	smaaAreaTexIdx = CreateTextureInternal(smaaAreaPixels.data(), 160, 560, false);
	smaaSearchTexIdx = CreateTextureInternal(smaaSearchPixels.data(), 64, 16, false);
}

void RenderContext::Impl::SetupUI(GLFWwindow* window) {
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
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
							 .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
							 .descriptorCount = 1000},
		VkDescriptorPoolSize{.type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, .descriptorCount = 1000}};

	const VkDescriptorPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.pNext = nullptr,
		.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
		.maxSets = 1000,
		.poolSizeCount = std::size(pool_sizes),
		.pPoolSizes = pool_sizes.data(),
	};

	VkDescriptorPool rawPool = VK_NULL_HANDLE;
	vkCreateDescriptorPool(ctx.Device(), &pool_info, nullptr, &rawPool);
	uiPool = Vk::DescriptorPool(ctx.Device(), rawPool);

	auto uiShaders = Vk::ShaderStages::Create(ctx.Device(), Resource::GetShaderProgram(Ui));

	VkPushConstantRange uiPush = {.stageFlags =
									  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
								  .offset = 0,
								  .size = sizeof(UIObjectConstants)};
	VkDescriptorSetLayout rawLayout = bindlessLayout.Get();
	ZHLN_PipelineLayoutDesc uiLayoutDesc = {.set_layouts = &rawLayout,
											.set_layout_count = 1,
											.push_constants = &uiPush,
											.push_constant_count = 1};

	uiPipelineLayout =
		Vk::PipelineLayout(ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &uiLayoutDesc));

	VkFormat swapchainFormat = presentation.swapchain.Get().format;

	auto uiPipelineRes = Vk::PipelineBuilder{}
							 .Shaders(uiShaders)
							 .Layout(uiPipelineLayout.Get())
							 .ColorFormats(std::array{swapchainFormat})
							 .NoDepth()
							 .AlphaBlend()
							 .CullNone()
							 .Build(ctx.Device());

	if (uiPipelineRes) {
		uiPipeline = std::move(*uiPipelineRes);
	} else {
		ZHLN::Panic("FATAL: Failed to build UI Pipeline!");
	}

	// --- ONLY RUN THE IMGUI/GLFW PORTION IF WINDOW IS VALID ---
	if (window != nullptr) {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui_ImplGlfw_InitForVulkan(window, true);

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
			.CustomShaderVertCreateInfo = {},
			.CustomShaderFragCreateInfo = {},
		};
		ImGui_ImplVulkan_Init(&init_info);
	}
}

bool RenderContext::Impl::RecreateTargets(VkExtent2D ext) {
	if (!presentation.Rebuild(ext.width, ext.height)) {
		return false;
	}

	graphResources.sceneColor = CreateDefaultTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>(ext);
	graphResources.velocityBuffer = CreateDefaultTarget<VK_FORMAT_R16G16_SFLOAT>(ext);
	accumBuffers[0] = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext);
	accumBuffers[1] = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext);
	graphResources.normalRoughnessBuffer = CreateDefaultTarget<VK_FORMAT_R8G8B8A8_UNORM>(ext);
	graphResources.postProcessTarget = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext);
	graphResources.ambientTarget = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext);
	graphResources.lightingTarget = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext);
	graphResources.smaaEdgeTarget = CreateDefaultTarget<VK_FORMAT_R8G8_UNORM>(ext);
	graphResources.smaaWeightTarget = CreateDefaultTarget<VK_FORMAT_R8G8B8A8_UNORM>(ext);

	VkExtent2D ext2 = {.width = std::max(1u, ext.width / 2), .height = std::max(1u, ext.height / 2)};
	VkExtent2D ext4 = {.width = std::max(1u, ext.width / 4), .height = std::max(1u, ext.height / 4)};
	VkExtent2D ext8 = {.width = std::max(1u, ext.width / 8), .height = std::max(1u, ext.height / 8)};
	VkExtent2D ext16 = {.width = std::max(1u, ext.width / 16),
						.height = std::max(1u, ext.height / 16)};

	graphResources.bloomThresholdTarget = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext2);
	graphResources.bloomDown1 = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext4);
	graphResources.bloomDown2 = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext8);
	graphResources.bloomDown3 = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext16);
	graphResources.bloomUp2 = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext8);
	graphResources.bloomUp1 = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext4);
	graphResources.bloomFinalTarget = CreateDefaultTarget<VK_FORMAT_R16G16B16A16_SFLOAT>(ext2);

	RecreatePunctualShadowViews();

	// Transition all newly allocated render targets to their correct default layouts
	Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
		std::array colorTargets = {graphResources.sceneColor.image.Handle(),
								   graphResources.velocityBuffer.image.Handle(),
								   accumBuffers[0].image.Handle(),
								   accumBuffers[1].image.Handle(),
								   graphResources.normalRoughnessBuffer.image.Handle(),
								   graphResources.postProcessTarget.image.Handle(),
								   graphResources.ambientTarget.image.Handle(),
								   graphResources.lightingTarget.image.Handle(),
								   graphResources.smaaEdgeTarget.image.Handle(),
								   graphResources.smaaWeightTarget.image.Handle(),
								   graphResources.bloomThresholdTarget.image.Handle(),
								   graphResources.bloomDown1.image.Handle(),
								   graphResources.bloomDown2.image.Handle(),
								   graphResources.bloomDown3.image.Handle(),
								   graphResources.bloomUp2.image.Handle(),
								   graphResources.bloomUp1.image.Handle(),
								   graphResources.bloomFinalTarget.image.Handle()};

		for (auto* const img : colorTargets) {
			Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
				cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
			Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
				cmd, img, VK_IMAGE_ASPECT_COLOR_BIT);
		}

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			cmd, presentation.depthTarget.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
	});

	return true;
}

void RenderContext::Impl::BuildHangGpuPipeline() {
	ZHLN_PipelineLayoutDesc pLayoutDesc = {.set_layouts = nullptr,
										   .set_layout_count = 0,
										   .push_constants = nullptr,
										   .push_constant_count = 0};

	hangGpuPass.pipelineLayout =
		Vk::PipelineLayout(ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &pLayoutDesc));

	hangGpuPass.pipeline = LoadAndCreateComputeShader({.path = SHADER_HANG_GPU_HLSL_CS_PATH,
													   .fallback = Resource::hang_gpu_comp,
													   .entryPoint = "CSMain"},
													  hangGpuPass.pipelineLayout.Get());
	if (hangGpuPass.pipeline.Valid()) {
		ZHLN::Log("[Shader Reload] Hang GPU Pipeline built successfully with empty layout.");
	} else {
		ZHLN::Log("ERROR: Failed to build Hang GPU compute pipeline. This isn't a fatal error, but "
				  "please check your build.");
	}
}

} // namespace ZHLN
