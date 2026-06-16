// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/Render_Resources.cpp
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include "Zahlen/Types.hpp"

#include <cstddef>

namespace ZHLN {

// Define CompileShadowPipeline here so it compiles with vertex reflection visible
void RenderContext::Impl::CompileShadowPipeline(VkDevice device, const void* shaderData,
												size_t shaderSize) {
	auto v_desc = Vk::CreateShaderDesc(Vk::AsSpirV(shaderData), shaderSize);
	// NEW: Include fragment shader in shadow pass so alpha-discard functions correctly!
	ZHLN_ShaderDesc f_desc = {.code = Vk::AsSpirV(&ZHLN_Resource_ShadowFragSpv[0]),
							  .size = ZHLN_Resource_ShadowFragSpv_Len,
							  .entry_point = "PSShadow"};

	auto shaders = Vk::ShaderStages::Create(device, v_desc, f_desc);

	VkPushConstantRange pc_range = {.stageFlags =
										VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
									.offset = 0,
									.size = sizeof(ObjectConstants)};

	const std::array layouts = {bindlessLayout.Get()};
	ZHLN_PipelineLayoutDesc layout_desc = {.set_layouts = layouts.data(),
										   .set_layout_count = 1,
										   .push_constants = &pc_range,
										   .push_constant_count = 1};

	shadowPipelineLayout =
		Vk::PipelineLayout(device, ZHLN_CreatePipelineLayout(device, &layout_desc));

	shadowPipeline = Vk::PipelineBuilder{}
						 .Shaders(shaders)
						 .Layout(shadowPipelineLayout.Get())
						 .Vertex<Vertex>()
						 .DepthOnly()
						 .DepthFormat(VK_FORMAT_D32_SFLOAT)
						 .CullFront()
						 .Build(device);
}

auto RenderContext::GetRendererName() const -> const char* {
	return _impl->appName.data();
}

auto RenderContext::GetGPUName() const -> const char* {
	return &_impl->ctx.PhysicalInfo().properties.properties.deviceName[0];
}

void RenderContext::SetResolution([[maybe_unused]] const Extent2D& res) {
	_impl->resized = true;
}

auto RenderContext::CreateVertexBuffer(const void* data, size_t size) -> BufferHandle {
	VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (_impl->rtCtx.Valid()) {
		usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
				 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	}
	auto gpu_buf =
		Vk::Buffer::Create(_impl->allocator.Get(), size, usage, VMA_MEMORY_USAGE_GPU_ONLY);
	Vk::CommandPool tempPool(_impl->ctx.Device(), _impl->ctx.PhysicalInfo().graphics_family);
	if (!tempPool.Allocate(1)) {
		ZHLN::Panic("Vulkan: Failed to allocate transient command buffer for vertex upload.");
	}
	VkCommandBuffer cmd = tempPool[0];
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

	// Return a packed generational handle
	uint64_t handle =
		_impl->meshPool.Create(std::move(gpu_buf), static_cast<uint32_t>(size / sizeof(Vertex)),
							   VK_NULL_HANDLE, 0ull, Vk::Buffer{});
	return static_cast<BufferHandle>(handle);
}

auto RenderContext::CreateIndexBuffer(const void* data, size_t size) -> BufferHandle {
	VkBufferUsageFlags usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	if (_impl->rtCtx.Valid()) {
		usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
				 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	}

	auto gpu_buf =
		Vk::Buffer::Create(_impl->allocator.Get(), size, usage, VMA_MEMORY_USAGE_GPU_ONLY);

	Vk::CommandPool tempPool(_impl->ctx.Device(), _impl->ctx.PhysicalInfo().graphics_family);
	if (!tempPool.Allocate(1)) {
		ZHLN::Panic("Vulkan: Failed to allocate transient command buffer for index upload.");
	}
	VkCommandBuffer cmd = tempPool[0];
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

	// Return a packed generational handle
	uint64_t handle =
		_impl->meshPool.Create(std::move(gpu_buf), static_cast<uint32_t>(size / sizeof(uint32_t)),
							   VK_NULL_HANDLE, 0ull, Vk::Buffer{});
	return static_cast<BufferHandle>(handle);
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
									.size = sizeof(ZHLN::ObjectConstants)};
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
						.ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16_SFLOAT,
									   VK_FORMAT_R16G16B16A16_SFLOAT})
						.DepthFormat(VK_FORMAT_D32_SFLOAT)
						.CullNone();

	if (desc.doubleSided) {
		pipeline.CullNone();
	} else {
		pipeline.CullBack();
	}

	if (desc.alphaBlend) {
		pipeline.AlphaBlend();
	}

	auto finalPipeline = pipeline.Build(impl->ctx.Device());

	// Return a packed generational handle
	uint64_t handle = impl->materialPool.Create(std::move(finalPipeline), std::move(layout));
	return Material{.pipeline = static_cast<PipelineHandle>(handle)};
}

auto RenderContext::CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB)
	-> uint32_t {
	auto* impl = _impl.get();
	const VkDevice device = impl->ctx.Device();
	const size_t imageSize = static_cast<size_t>(width) * height * 4;

	const VkImageCreateInfo imgInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = isSRGB ? VK_FORMAT_R8G8B8A8_SRGB
						 : VK_FORMAT_R8G8B8A8_UNORM, // <--- Dynamically chose format
		.extent{.width = width, .height = height, .depth = 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};

	auto gpuImage = Vk::Image::Create(impl->allocator.Get(), imgInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	Vk::CommandPool tempPool(device, impl->ctx.PhysicalInfo().graphics_family);
	if (!tempPool.Allocate(1)) {
		ZHLN::Panic("Allocation failed for texture");
	}
	VkCommandBuffer cmd = tempPool[0];

	ZHLN_BeginCommandBuffer(cmd);

	auto staging = Vk::Buffer::Create(impl->allocator.Get(), imageSize,
									  VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	std::memcpy(staging.Map().data, data, imageSize);

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
		cmd, gpuImage.Handle());

	ZHLN_BufferImageCopyDesc copyRegion = {.buffer = staging.Handle(),
										   .image = gpuImage.Handle(),
										   .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
										   .width = width,
										   .height = height,
										   .buffer_offset = 0,
										   .mip_level = 0,
										   .base_array_layer = 0};
	ZHLN_CmdCopyBufferToImage(cmd, &copyRegion);

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, gpuImage.Handle());

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
	vkQueueSubmit2(impl->ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(impl->ctx.GraphicsQueue());

	auto gpuView = isSRGB ? Vk::CreateView<VK_FORMAT_R8G8B8A8_SRGB>(device, gpuImage.Handle())
						  : Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(
								device, gpuImage.Handle()); // <--- Choose view

	uint32_t index = impl->nextTextureIndex++;

	VkDescriptorImageInfo bindlessUpdate = {.sampler = VK_NULL_HANDLE,
											.imageView = gpuView.Get(),
											.imageLayout =
												VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

	std::array<VkWriteDescriptorSet, 2> writes = {};
	for (int i = 0; i < 2; ++i) {
		writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
					 .pNext = nullptr,
					 .dstSet = impl->bindlessSets[i],
					 .dstBinding = 0,
					 .dstArrayElement = index,
					 .descriptorCount = 1,
					 .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
					 .pImageInfo = &bindlessUpdate,
					 .pBufferInfo = nullptr,
					 .pTexelBufferView = nullptr};
	}

	vkUpdateDescriptorSets(device, 2, writes.data(), 0, nullptr);
	impl->textureImages.push_back(std::move(gpuImage));
	impl->textureViews.push_back(std::move(gpuView));

	return index;
}

uint32_t RenderContext::CreateTextureCube(const void* const* faceData, uint32_t width,
										  uint32_t height) {
	auto* impl = _impl.get();
	const VkDevice device = impl->ctx.Device();
	const size_t faceSize = static_cast<size_t>(width) * height * 4;

	const VkImageCreateInfo imgInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = nullptr,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent{.width = width, .height = height, .depth = 1},
		.mipLevels = 1,
		.arrayLayers = 6,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = {},
		.pQueueFamilyIndices = {},
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	auto gpuImage = Vk::Image::Create(impl->allocator.Get(), imgInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	Vk::CommandPool tempPool(device, impl->ctx.PhysicalInfo().graphics_family);
	if (!tempPool.Allocate(1)) {
		ZHLN::Panic("Vulkan: Failed to allocate command buffer for Cubemap copy");
	}
	VkCommandBuffer cmd = tempPool[0];

	ZHLN_BeginCommandBuffer(cmd);

	Vk::Buffer staging =
		Vk::Buffer::Create(impl->allocator.Get(), faceSize * 6, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
						   VMA_MEMORY_USAGE_CPU_ONLY);

	{
		auto mapped = staging.Map();
		for (uint32_t i = 0; i < 6; ++i) {
			std::memcpy(static_cast<char*>(mapped.data) + (i * faceSize), faceData[i], faceSize);
		}
	}

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
		cmd, gpuImage.Handle());

	for (uint32_t i = 0; i < 6; ++i) {
		VkBufferImageCopy2 region = {
			.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
			.pNext = nullptr,
			.bufferOffset = i * faceSize,
			.bufferRowLength = {},
			.bufferImageHeight = {},
			.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
								 .mipLevel = 0,
								 .baseArrayLayer = i,
								 .layerCount = 1},
			.imageOffset = {},
			.imageExtent = {.width = width, .height = height, .depth = 1},
		};

		VkCopyBufferToImageInfo2 copyInfo = {
			.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
			.pNext = {},
			.srcBuffer = staging.Handle(),
			.dstImage = gpuImage.Handle(),
			.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.regionCount = 1,
			.pRegions = &region,
		};
		vkCmdCopyBufferToImage2(cmd, &copyInfo);
	}

	Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, gpuImage.Handle());

	ZHLN_EndCommandBuffer(cmd);

	VkCommandBufferSubmitInfo subInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.pNext = {},
		.commandBuffer = cmd,
		.deviceMask = {},
	};
	VkSubmitInfo2 submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.pNext = {},
		.flags = {},
		.waitSemaphoreInfoCount = {},
		.pWaitSemaphoreInfos = {},
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &subInfo,
		.signalSemaphoreInfoCount = {},
		.pSignalSemaphoreInfos = {},
	};
	vkQueueSubmit2(impl->ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(impl->ctx.GraphicsQueue());

	auto gpuView = Vk::CreateViewCube<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(), 1);

	uint32_t index = impl->nextTextureIndex++;

	VkDescriptorImageInfo bindlessUpdate = {.sampler = VK_NULL_HANDLE,
											.imageView = gpuView.Get(),
											.imageLayout =
												VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

	std::array<VkWriteDescriptorSet, 2> writes = {};
	for (int i = 0; i < 2; ++i) {
		writes[i] = {
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.pNext = {},
			.dstSet = impl->bindlessSets[i],
			.dstBinding = 0,
			.dstArrayElement = index,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
			.pImageInfo = &bindlessUpdate,
			.pBufferInfo = {},
			.pTexelBufferView = {},
		};
	}
	vkUpdateDescriptorSets(device, 2, writes.data(), 0, nullptr);
	impl->textureImages.push_back(std::move(gpuImage));
	impl->textureViews.push_back(std::move(gpuView));

	return index;
}

void RenderContext::UpdateJointMatrices(uint32_t offset, const JPH::Mat44* matrices,
										uint32_t count) {
	if (count == 0) {
		return;
	}
	auto mappedRegion = _impl->jointBuffers[_impl->frame_index].Map();
	auto* gpuJoints = std::bit_cast<JPH::Mat44*>(mappedRegion.data);

	std::memcpy(gpuJoints + offset, matrices, count * sizeof(JPH::Mat44));
}

uint32_t RenderContext::AllocateMorphDeltas(uint32_t count, const float* deltas) {
	uint32_t offset = _impl->nextMorphDeltaIndex;

	// 1. Correctly map the morphDeltasBuffer and keep it in scope!
	auto mappedRegion = _impl->morphDeltasBuffer.Map();

	// 2. Safely offset the pointer within the mapped memory block
	float* gpuDeltas = std::bit_cast<float*>(mappedRegion.data) + (static_cast<size_t>(offset * 4));

	// 3. This memcpy is now 100% safe since mappedRegion is still alive
	std::memcpy(gpuDeltas, deltas, count * sizeof(float) * 4);

	_impl->nextMorphDeltaIndex += count;
	return offset;
}

void RenderContext::SetTAAState(const TAAState& state) {
	_impl->taaState = state;
}

void RenderContext::BuildMeshBLAS(Mesh& mesh) {
	auto* impl = _impl.get();
	if (!impl->rtCtx.Valid()) {
		return; // Gracefully skip BLAS build on non-RT systems
	}
	auto* nativeMesh = impl->meshPool.Resolve(static_cast<uint64_t>(mesh.vertexBuffer));
	auto* nativeIndexMesh = mesh.indexBuffer != BufferHandle::Invalid
								? impl->meshPool.Resolve(static_cast<uint64_t>(mesh.indexBuffer))
								: nullptr;
	if (nativeMesh == nullptr) {
		return;
	}

	ZHLN_BlasGeometryDesc geom = {
		.vertex_data = Vk::GetBufferDeviceAddress(impl->ctx.Device(), nativeMesh->buffer.Handle()),
		.vertex_stride = sizeof(Vertex),
		.max_vertex = mesh.vertexCount,
		.vertex_format = VK_FORMAT_R32G32B32_SFLOAT,
		.index_data =
			(nativeIndexMesh != nullptr)
				? Vk::GetBufferDeviceAddress(impl->ctx.Device(), nativeIndexMesh->buffer.Handle())
				: 0,
		.index_type = (nativeIndexMesh != nullptr) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_NONE_KHR};

	uint32_t primitiveCount =
		(nativeIndexMesh != nullptr) ? mesh.indexCount / 3 : mesh.vertexCount / 3;

	ZHLN_AccelerationStructureSizes sizes;
	impl->rtCtx.GetBlasSizes(geom, primitiveCount, sizes);

	nativeMesh->blasBuffer =
		Vk::Buffer::Create(impl->allocator.Get(), sizes.acceleration_structure_size,
						   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
							   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
						   VMA_MEMORY_USAGE_GPU_ONLY);
	nativeMesh->blas =
		impl->rtCtx.CreateAS(nativeMesh->blasBuffer.Handle(), sizes.acceleration_structure_size,
							 ZHLN_AS_TYPE_BOTTOM_LEVEL);
	nativeMesh->blasAddress = impl->rtCtx.GetASAddress(nativeMesh->blas);

	nativeMesh->device = impl->ctx.Device();
	nativeMesh->rtCtx = &impl->rtCtx;

	Vk::Buffer scratch = Vk::Buffer::Create(impl->allocator.Get(), sizes.build_scratch_size,
											VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
												VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
											VMA_MEMORY_USAGE_GPU_ONLY);

	// Because we submit isolated queues, we just block the main thread and sync manually here (safe
	// for initial static uploads)
	Vk::CommandPool tempPool(impl->ctx.Device(), impl->ctx.PhysicalInfo().graphics_family);
	if (!tempPool.Allocate(1)) {
		ZHLN::Panic("Failed to initialize command pool for BLAS mesh building.");
	}
	VkCommandBuffer cmd = tempPool[0];
	ZHLN_BeginCommandBuffer(cmd);

	impl->rtCtx.CmdBuildBlas(cmd, geom, nativeMesh->blas,
							 Vk::GetBufferDeviceAddress(impl->ctx.Device(), scratch.Handle()),
							 primitiveCount);

	ZHLN_EndCommandBuffer(cmd);

	VkCommandBufferSubmitInfo subInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.pNext = {},
		.commandBuffer = cmd,
		.deviceMask = {},
	};
	VkSubmitInfo2 submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.pNext = {},
		.flags = {},
		.waitSemaphoreInfoCount = {},
		.pWaitSemaphoreInfos = {},
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &subInfo,
		.signalSemaphoreInfoCount = {},
		.pSignalSemaphoreInfos = {},
	};
	vkQueueSubmit2(impl->ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(impl->ctx.GraphicsQueue());
}
} // namespace ZHLN
