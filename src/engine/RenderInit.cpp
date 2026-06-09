// File: src/engine/Render_Init.cpp
#include "PBR.hpp"
#include "RenderCore.hpp"
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include "SamplerBuilder.hpp"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

#include <Features.hpp>
#include <cstddef>
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

	// 1. Declare the leaf node of the chain
	auto swap_maint = Vk::FeatureFactory::Create<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>(
		[](auto& f) { f.swapchainMaintenance1 = VK_TRUE; });

	// 2. Point 1.3 to swap_maint
	auto feat13 =
		Vk::FeatureFactory::Create<VkPhysicalDeviceVulkan13Features>([&swap_maint](auto& f) {
			f.pNext = &swap_maint;
			f.synchronization2 = VK_TRUE;
			f.dynamicRendering = VK_TRUE;
		});

	// 3. Point 1.2 to 1.3
	auto feat12 = Vk::FeatureFactory::Create<VkPhysicalDeviceVulkan12Features>([&feat13](auto& f) {
		f.pNext = &feat13;
		f.descriptorIndexing = VK_TRUE;
		f.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
		f.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
		f.descriptorBindingPartiallyBound = VK_TRUE;
		f.runtimeDescriptorArray = VK_TRUE;
		f.bufferDeviceAddress = VK_TRUE;
	});

	// 4. Point root Features2 to 1.2
	auto feat2 = Vk::FeatureFactory::Create<VkPhysicalDeviceFeatures2>([&feat12](auto& f) {
		f.pNext = &feat12;
		f.features.multiDrawIndirect = VK_TRUE;
		f.features.samplerAnisotropy = VK_TRUE;
		f.features.drawIndirectFirstInstance = VK_TRUE;
	});
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
	_impl->InitCullingResources();
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
	_impl->pools = Vk::CommandPools<2>::Create(
		_impl->ctx.Device(),
		{.queue_family = _impl->ctx.PhysicalInfo().graphics_family, .buffers_per_pool = 1});
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

	// Index 0: Solid Black (Used for Emissive, Metallic, and Roughness fallbacks) -> Linear
	uint8_t blackPixel[4] = {0, 0, 0, 0};
	CreateTexture(blackPixel, 1, 1, false);

	// Index 1: Solid White (Used for Albedo fallback) -> sRGB
	uint8_t whitePixel[4] = {255, 255, 255, 255};
	CreateTexture(whitePixel, 1, 1, true);

	// Index 2: Flat Tangent-Space Normal Map (R=128, G=128, B=255) -> Linear
	uint8_t normalPixel[4] = {128, 128, 255, 255};
	CreateTexture(normalPixel, 1, 1, false);
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

	for (int i = 0; i < 2; ++i) {
		frameUniformBuffers[i] =
			Vk::Buffer::Create(allocator.Get(), sizeof(FrameUniforms),
							   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

		lightStorageBuffers[i] =
			Vk::Buffer::Create(allocator.Get(), sizeof(GPULight) * 128,
							   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	}
}

void RenderContext::Impl::InitCullingResources() {
	cullingLayout = CullingLayout::CreateLayout(ctx.Device());
	cullingPool = CullingLayout::CreatePool(ctx.Device(), 2);
	cullingSets[0] = CullingLayout::Allocate(ctx.Device(), cullingPool.Get(), cullingLayout.Get());
	cullingSets[1] = CullingLayout::Allocate(ctx.Device(), cullingPool.Get(), cullingLayout.Get());

	for (int i = 0; i < 2; ++i) {
		instanceDataBuffers[i] =
			Vk::Buffer::Create(allocator.Get(), sizeof(InstanceData) * kGpuCullingMaxInstances,
							   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

		indirectCommandsBuffers[i] = Vk::Buffer::Create(
			allocator.Get(), sizeof(VkDrawIndexedIndirectCommand) * kGpuCullingMaxInstances,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
			VMA_MEMORY_USAGE_GPU_ONLY);

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

	VkDescriptorSetLayout cullingLayouts[] = {cullingLayout.Get()};
	auto desc = ZHLN_PipelineLayoutDesc{.set_layouts = cullingLayouts,
										.set_layout_count = 1,
										.push_constants = &cullingPush,
										.push_constant_count = 1};

	cullingPipelineLayout =
		Vk::PipelineLayout(ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &desc));

	cullingPipeline = Vk::ComputePipelineBuilder{}
						  .Shader(Vk::AsSpirV(&ZHLN_Resource_CullingCompSpv[0]),
								  ZHLN_Resource_CullingCompSpv_Len, "CSMain")
						  .Layout(cullingPipelineLayout.Get())
						  .Build(ctx.Device());
}

void RenderContext::Impl::InitBindless() {
	bindlessLayout = GlobalSceneLayout::CreateLayout(ctx.Device());
	bindlessPool = GlobalSceneLayout::CreatePool(ctx.Device(), 2);
	bindlessSets[0] =
		GlobalSceneLayout::Allocate(ctx.Device(), bindlessPool.Get(), bindlessLayout.Get());
	bindlessSets[1] =
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
	// Create the math/environment sampler (ClampToEdge prevents wrap-around artifacts)
	clampSampler = Vk::SamplerBuilder{}.Linear().ClampToEdge().Build(ctx.Device());

	// Allocate our global Joint storage buffer (Supports 8192 dynamic matrices)
	JPH::Array<JPH::Mat44> identities(8192, JPH::Mat44::sIdentity());
	for (int i = 0; i < 2; ++i) {
		jointBuffers[i] =
			Vk::Buffer::Create(allocator.Get(), sizeof(JPH::Mat44) * 8192,
							   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

		// Upload identity matrices initially
		auto mapped = jointBuffers[i].Map();
		std::memcpy(mapped.data, identities.data(), identities.size() * sizeof(JPH::Mat44));
	}

	morphDeltasBuffer =
		Vk::Buffer::Create(allocator.Get(), sizeof(float) * 4 * 1000000,
						   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	// -----------------------------------------------------------------
	// Pass 1: BRDF LUT Generation
	// -----------------------------------------------------------------
	ZHLN::Log("[IBL] Generating 2D BRDF Look-Up Table...");
	std::vector<uint32_t> lutData = ZHLN::PBR::GenerateBRDFLUT(512, 512);
	VkImageCreateInfo lutInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
								 .pNext = nullptr,
								 .flags = 0,
								 .imageType = VK_IMAGE_TYPE_2D,
								 .format = VK_FORMAT_R8G8B8A8_UNORM,
								 .extent = {.width = 512, .height = 512, .depth = 1},
								 .mipLevels = 1,
								 .arrayLayers = 1,
								 .samples = VK_SAMPLE_COUNT_1_BIT,
								 .tiling = VK_IMAGE_TILING_OPTIMAL,
								 .usage =
									 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
								 .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
								 .queueFamilyIndexCount = 0,
								 .pQueueFamilyIndices = nullptr,
								 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	brdfLutImage = Vk::Image::Create(allocator.Get(), lutInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	{
		Vk::CommandPool lutPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
		if (!lutPool.Allocate(1)) {
			ZHLN::Panic("Vulkan: Failed to allocate LUT command buffer");
		}
		VkCommandBuffer cmd = lutPool[0];

		ZHLN_BeginCommandBuffer(cmd);
		Vk::Buffer lutStaging =
			Vk::Buffer::Create(allocator.Get(), static_cast<size_t>(512 * 512 * 4),
							   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		std::memcpy(lutStaging.Map().data, lutData.data(), static_cast<size_t>(512 * 512 * 4));

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
			cmd, brdfLutImage.Handle());
		ZHLN_BufferImageCopyDesc lutCopy = {.buffer = lutStaging.Handle(),
											.image = brdfLutImage.Handle(),
											.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
											.width = 512,
											.height = 512,
											.buffer_offset = 0,
											.mip_level = 0,
											.base_array_layer = 0};
		ZHLN_CmdCopyBufferToImage(cmd, &lutCopy);
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, brdfLutImage.Handle());
		ZHLN_EndCommandBuffer(cmd);

		VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											 .pNext = nullptr,
											 .commandBuffer = cmd,
											 .deviceMask = 0};
		VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								.pNext = nullptr,
								.flags = 0,
								.waitSemaphoreInfoCount = 0,
								.pWaitSemaphoreInfos = nullptr,
								.commandBufferInfoCount = 1,
								.pCommandBufferInfos = &subInfo,
								.signalSemaphoreInfoCount = 0,
								.pSignalSemaphoreInfos = nullptr};
		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
		vkQueueWaitIdle(ctx.GraphicsQueue());
	}
	brdfLutView = Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(ctx.Device(), brdfLutImage.Handle());

	// -----------------------------------------------------------------
	// Pass 2: Diffuse Irradiance Cubemap Generation
	// -----------------------------------------------------------------
	ZHLN::Log("[IBL] Generating Diffuse Irradiance Cubemap...");
	std::vector<std::vector<uint32_t>> irrData = ZHLN::PBR::GenerateIrradianceCubemap();
	VkImageCreateInfo irrInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
								 .pNext = nullptr,
								 .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
								 .imageType = VK_IMAGE_TYPE_2D,
								 .format = VK_FORMAT_R8G8B8A8_UNORM,
								 .extent = {.width = 32, .height = 32, .depth = 1},
								 .mipLevels = 1,
								 .arrayLayers = 6,
								 .samples = VK_SAMPLE_COUNT_1_BIT,
								 .tiling = VK_IMAGE_TILING_OPTIMAL,
								 .usage =
									 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
								 .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
								 .queueFamilyIndexCount = 0,
								 .pQueueFamilyIndices = nullptr,
								 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	irradianceImage = Vk::Image::Create(allocator.Get(), irrInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	{
		Vk::CommandPool irrPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
		if (!irrPool.Allocate(1)) {
			ZHLN::Panic("Vulkan: Failed to allocate Irradiance command buffer");
		}
		VkCommandBuffer cmd = irrPool[0];

		ZHLN_BeginCommandBuffer(cmd);
		Vk::Buffer irrStaging =
			Vk::Buffer::Create(allocator.Get(), static_cast<size_t>(32 * 32 * 4 * 6),
							   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
		{
			auto irrMap = irrStaging.Map();
			for (int i = 0; i < 6; ++i) {
				std::memcpy((char*)irrMap.data + static_cast<ptrdiff_t>(i * 32 * 32 * 4),
							irrData[i].data(), static_cast<size_t>(32 * 32 * 4));
			}
		}

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
			cmd, irradianceImage.Handle());
		for (int i = 0; i < 6; ++i) {
			VkBufferImageCopy2 region = {
				.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
				.pNext = nullptr,
				.bufferOffset = static_cast<VkDeviceSize>(i * 32 * 32 * 4),
				.bufferRowLength = 0,
				.bufferImageHeight = 0,
				.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
									 .mipLevel = 0,
									 .baseArrayLayer = (uint32_t)i,
									 .layerCount = 1},
				.imageOffset = {.x = 0, .y = 0, .z = 0},
				.imageExtent = {.width = 32, .height = 32, .depth = 1}};
			VkCopyBufferToImageInfo2 copyInfo = {
				.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
				.pNext = nullptr,
				.srcBuffer = irrStaging.Handle(),
				.dstImage = irradianceImage.Handle(),
				.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				.regionCount = 1,
				.pRegions = &region};
			vkCmdCopyBufferToImage2(cmd, &copyInfo);
		}
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd,
																	   irradianceImage.Handle());
		ZHLN_EndCommandBuffer(cmd);

		VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											 .pNext = nullptr,
											 .commandBuffer = cmd,
											 .deviceMask = 0};
		VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								.pNext = nullptr,
								.flags = 0,
								.waitSemaphoreInfoCount = 0,
								.pWaitSemaphoreInfos = nullptr,
								.commandBufferInfoCount = 1,
								.pCommandBufferInfos = &subInfo,
								.signalSemaphoreInfoCount = 0,
								.pSignalSemaphoreInfos = nullptr};
		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
		vkQueueWaitIdle(ctx.GraphicsQueue());
	}
	irradianceView =
		Vk::CreateViewCube<VK_FORMAT_R8G8B8A8_UNORM>(ctx.Device(), irradianceImage.Handle(), 1);

	// -----------------------------------------------------------------
	// Pass 3: Specular Pre-filtered Cubemap Generation (6 Mip Levels)
	// -----------------------------------------------------------------
	ZHLN::Log("[IBL] Generating Specular Pre-filtered Cubemap Mips...");

	constexpr uint32_t kBaseSize = 256;
	constexpr uint32_t kMipLevels = 6;

	VkImageCreateInfo specInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
								  .pNext = nullptr,
								  .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
								  .imageType = VK_IMAGE_TYPE_2D,
								  .format = VK_FORMAT_R8G8B8A8_UNORM,
								  .extent = {.width = kBaseSize, .height = kBaseSize, .depth = 1},
								  .mipLevels = kMipLevels, // <-- Set to 6
								  .arrayLayers = 6,
								  .samples = VK_SAMPLE_COUNT_1_BIT,
								  .tiling = VK_IMAGE_TILING_OPTIMAL,
								  .usage =
									  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
								  .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
								  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	prefilteredImage = Vk::Image::Create(allocator.Get(), specInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	{
		Vk::CommandPool specPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
		specPool.Allocate(1);
		VkCommandBuffer cmd = specPool[0];
		ZHLN_BeginCommandBuffer(cmd);

		// Pre-calculate total bytes needed for all mips of 6 faces to allocate one flat buffer
		size_t totalBytes = 0;
		for (uint32_t m = 0; m < kMipLevels; ++m) {
			uint32_t s = kBaseSize >> m;
			totalBytes += (static_cast<size_t>(s * s * 4 * 6));
		}

		Vk::Buffer specStaging =
			Vk::Buffer::Create(allocator.Get(), totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
							   VMA_MEMORY_USAGE_CPU_ONLY);

		auto specMap = specStaging.Map();
		char* writePtr = static_cast<char*>(specMap.data);
		size_t currentOffset = 0;

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
			cmd, prefilteredImage.Handle());

		// Process and upload each mip level sequentially
		for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
			uint32_t mipSize = kBaseSize >> mip;
			float roughness = static_cast<float>(mip) / static_cast<float>(kMipLevels - 1);

			// Generate CPU-side
			auto mipData = ZHLN::PBR::GenerateSpecularMip(mipSize, roughness);

			// Write directly into mapped memory sequentially
			auto faceBytes = static_cast<size_t>(mipSize) * mipSize * 4;
			for (int face = 0; face < 6; ++face) {
				std::memcpy(writePtr + currentOffset + (face * faceBytes), mipData[face].data(),
							faceBytes);
			}

			// Record Vulkan copy commands for this mip level's 6 faces
			for (int face = 0; face < 6; ++face) {
				VkBufferImageCopy2 region = {
					.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
					.bufferOffset = currentOffset + (face * faceBytes),
					.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
										 .mipLevel = mip,
										 .baseArrayLayer = static_cast<uint32_t>(face),
										 .layerCount = 1},
					.imageExtent = {.width = mipSize, .height = mipSize, .depth = 1}};

				VkCopyBufferToImageInfo2 copyInfo = {
					.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
					.srcBuffer = specStaging.Handle(),
					.dstImage = prefilteredImage.Handle(),
					.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					.regionCount = 1,
					.pRegions = &region};
				vkCmdCopyBufferToImage2(cmd, &copyInfo);
			}
			currentOffset += (faceBytes * 6);
		}

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			cmd, prefilteredImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT);

		ZHLN_EndCommandBuffer(cmd);

		VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											 .commandBuffer = cmd};
		VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								.commandBufferInfoCount = 1,
								.pCommandBufferInfos = &subInfo};
		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
		vkQueueWaitIdle(ctx.GraphicsQueue());
	}
	prefilteredView = Vk::CreateViewCube<VK_FORMAT_R8G8B8A8_UNORM>(
		ctx.Device(), prefilteredImage.Handle(), kMipLevels);

	// -----------------------------------------------------------------
	// Pass 4: LTC Area Light LUT Uploads (Direct embedded memory blast)
	// -----------------------------------------------------------------
	ZHLN::Log("[IBL] Uploading Linearly Transformed Cosines (LTC) LUTs...");

	VkImageCreateInfo ltcInfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
								 .pNext = nullptr,
								 .flags = 0,
								 .imageType = VK_IMAGE_TYPE_2D,
								 .format = VK_FORMAT_R16G16B16A16_SFLOAT,
								 .extent = {.width = 64, .height = 64, .depth = 1},
								 .mipLevels = 1,
								 .arrayLayers = 1,
								 .samples = VK_SAMPLE_COUNT_1_BIT,
								 .tiling = VK_IMAGE_TILING_OPTIMAL,
								 .usage =
									 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
								 .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
								 .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};

	ltcMatImage = Vk::Image::Create(allocator.Get(), ltcInfo, VMA_MEMORY_USAGE_GPU_ONLY);
	ltcAmpImage = Vk::Image::Create(allocator.Get(), ltcInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	{
		Vk::CommandPool ltcPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
		ltcPool.Allocate(1);
		VkCommandBuffer cmd = ltcPool[0];

		ZHLN_BeginCommandBuffer(cmd);

		// 1. We allocate the staging buffer based on raw pixel data size (excluding the 128-byte
		// headers) [1]
		const size_t matRawSize = ZHLN_Resource_LtcMatBin_Len - 128;
		const size_t ampRawSize = ZHLN_Resource_LtcAmpBin_Len - 128;

		Vk::Buffer ltcStaging =
			Vk::Buffer::Create(allocator.Get(), matRawSize + ampRawSize,
							   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

		auto mapped = ltcStaging.Map();
		char* stagePtr = static_cast<char*>(mapped.data);

		// 2. Direct-copy but offset source by 128 bytes to skip DDS headers [1]
		std::memcpy(stagePtr, ZHLN_Resource_LtcMatBin + 128, matRawSize);
		std::memcpy(stagePtr + matRawSize, ZHLN_Resource_LtcAmpBin + 128, ampRawSize);

		// Transition images to DST
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
			cmd, ltcMatImage.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
			cmd, ltcAmpImage.Handle());

		// Copy Mat LUT (64x64)
		ZHLN_BufferImageCopyDesc copyMat = {.buffer = ltcStaging.Handle(),
											.image = ltcMatImage.Handle(),
											.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
											.width = 64,
											.height = 64,
											.buffer_offset = 0};
		ZHLN_CmdCopyBufferToImage(cmd, &copyMat);

		// Copy Amp LUT (64x64)
		ZHLN_BufferImageCopyDesc copyAmp = {
			.buffer = ltcStaging.Handle(),
			.image = ltcAmpImage.Handle(),
			.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.width = 64,
			.height = 64,
			.buffer_offset = matRawSize // Staged offset
		};
		ZHLN_CmdCopyBufferToImage(cmd, &copyAmp);

		// Transition to READ
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, ltcMatImage.Handle());
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, ltcAmpImage.Handle());

		ZHLN_EndCommandBuffer(cmd);

		VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
											 .commandBuffer = cmd};
		VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								.commandBufferInfoCount = 1,
								.pCommandBufferInfos = &subInfo};
		vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
		vkQueueWaitIdle(ctx.GraphicsQueue());
	}
	ltcMatView = Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcMatImage.Handle());
	ltcAmpView = Vk::CreateView<VK_FORMAT_R16G16B16A16_SFLOAT>(ctx.Device(), ltcAmpImage.Handle());

	// Update global descriptor bindings
	for (int i = 0; i < 2; ++i) {
		GlobalSceneLayout::Write(
			ctx.Device(), bindlessSets[i], Vk::SkipWrite{}, Vk::SamplerWrite{globalSampler.Get()},
			Vk::ImageWrite{.view = shadowMap.view.Get(),
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			Vk::SamplerWrite{shadowSampler.Get()},
			Vk::BufferWrite{.buffer = frameUniformBuffers[i].Handle()},
			Vk::BufferWrite{.buffer = lightStorageBuffers[i].Handle()},
			Vk::BufferWrite{.buffer = instanceDataBuffers[i].Handle()},
			Vk::BufferWrite{.buffer = jointBuffers[i].Handle()},

			// --- WRITE IBL DESCRIPTORS ---
			Vk::ImageWrite{.view = irradianceView.Get(),
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			Vk::ImageWrite{.view = prefilteredView.Get(),
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			Vk::ImageWrite{.view = brdfLutView.Get(),
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			Vk::BufferWrite{.buffer = morphDeltasBuffer.Handle()},
			Vk::SamplerWrite{clampSampler.Get()},
			Vk::ImageWrite{.view = ltcMatView.Get(),
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
			Vk::ImageWrite{.view = ltcAmpView.Get(),
						   .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
	}
}
void RenderContext::Impl::InitPostProcessing() {
	defaultSampler = Vk::SamplerBuilder{}.Linear().ClampToEdge().Build(ctx.Device());

	VkPushConstantRange taaPush = {
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 0, .size = sizeof(float)};
	auto taaShaders = Vk::ShaderStages::Create(ctx.Device(),
											   {.code = Vk::AsSpirV(&ZHLN_Resource_TaaVertSpv[0]),
												.size = ZHLN_Resource_TaaVertSpv_Len,
												.entry_point = "VSMain"},
											   {.code = Vk::AsSpirV(&ZHLN_Resource_TaaFragSpv[0]),
												.size = ZHLN_Resource_TaaFragSpv_Len,
												.entry_point = "PSMain"});

	if (!taaPass.Build(ctx.Device(), taaShaders, {VK_FORMAT_R16G16B16A16_SFLOAT}, &taaPush, 1)) {
		ZHLN::Log("TAA pass build failure, continuing...");
	}

	auto blitShaders = Vk::ShaderStages::Create(ctx.Device(),
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
		.poolSizeCount = (uint32_t)std::size(pool_sizes),
		.pPoolSizes = pool_sizes.data(),
	};

	VkDescriptorPool rawPool = VK_NULL_HANDLE;
	vkCreateDescriptorPool(ctx.Device(), &pool_info, nullptr, &rawPool);
	uiPool = Vk::DescriptorPool(ctx.Device(), rawPool);

	auto uiShaders = Vk::ShaderStages::Create(
		ctx.Device(),
		Vk::CreateShaderDesc(Vk::AsSpirV(&ZHLN_Resource_UiVertSpv[0]), ZHLN_Resource_UiVertSpv_Len),
		Vk::CreateShaderDesc(Vk::AsSpirV(&ZHLN_Resource_UiFragSpv[0]),
							 ZHLN_Resource_UiFragSpv_Len));

	// 144 bytes matches the exact size of the UIObjectConstants struct in HLSL
	VkPushConstantRange uiPush = {.stageFlags =
									  VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
								  .offset = 0,
								  .size = 144};
	VkDescriptorSetLayout rawLayout = bindlessLayout.Get();
	ZHLN_PipelineLayoutDesc uiLayoutDesc = {.set_layouts = &rawLayout,
											.set_layout_count = 1,
											.push_constants = &uiPush,
											.push_constant_count = 1};

	uiPipelineLayout =
		Vk::PipelineLayout(ctx.Device(), ZHLN_CreatePipelineLayout(ctx.Device(), &uiLayoutDesc));

	uiPipeline =
		Vk::PipelineBuilder{}
			.Shaders(uiShaders)
			.Layout(uiPipelineLayout.Get())
			.Vertex<Vertex>()
			.ColorFormats({presentation.swapchain.Get().format}) // Render straight to swapchain!
			.NoDepth()											 // Disable depth testing
			.AlphaBlend()										 // Enable transparency
			.CullNone()
			.Build(ctx.Device());

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

} // namespace ZHLN
