// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/RenderResources.cpp
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include "Zahlen/Types.hpp"
#include "detail/ControlFlow.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace ZHLN {

void RenderContext::Impl::CompileShadowPipeline(VkDevice device,
												const Resource::ShaderPair& shaderData) {
	auto shaders = Vk::ShaderStages::Create(device, shaderData, "VSMain", "PSShadow");

	shadowPipelineLayout =
		Vk::PipelineLayoutBuilder(device)
			.AddDescriptorSetLayout(bindlessLayout.Get())
			.AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
							 sizeof(ObjectConstants))
			.Build();

	auto shadowPipelineRes = Vk::PipelineBuilder{}
								 .Shaders(shaders)
								 .Layout(shadowPipelineLayout.Get())
								 .DepthOnly()
								 .DepthFormat(VK_FORMAT_D32_SFLOAT)
								 .CullNone()
								 .Build(device);

	ZHLN::PanicIf(!shadowPipelineRes, "FATAL: Failed to build Shadow Pipeline!");
	shadowPipeline = std::move(*shadowPipelineRes);
}

void RenderContext::Impl::CompilePunctualShadowPipeline(VkDevice device,
														const Resource::ShaderPair& shaderData) {
	auto shaders = Vk::ShaderStages::Create(device, shaderData);

	punctualShadowPipelineLayout =
		Vk::PipelineLayoutBuilder(device)
			.AddDescriptorSetLayout(bindlessLayout.Get())
			.AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT, sizeof(uint32_t))
			.Build();

	auto punctualShadowPipelineRes = Vk::PipelineBuilder{}
										 .Shaders(shaders)
										 .Layout(punctualShadowPipelineLayout.Get())
										 .DepthOnly()
										 .DepthFormat(VK_FORMAT_D32_SFLOAT)
										 .CullNone()
										 .Build(device);

	ZHLN::PanicIf(!punctualShadowPipelineRes, "FATAL: Failed to build Punctual Shadow Pipeline!");
	punctualShadowPipeline = std::move(*punctualShadowPipelineRes);
}

auto RenderContext::GetRendererName() const -> const char* {
	return _impl->appName.data();
}

auto RenderContext::GetGPUName() const -> const char* {
	return &_impl->ctx.PhysicalInfo().properties.properties.deviceName[0];
}

uint32_t RenderContext::GetFrameIndex() const noexcept {
	return _impl->frame_index;
}

void RenderContext::SetResolution([[maybe_unused]] const Extent2D& res) {
	_impl->resized = true;
}

auto RenderContext::CreateVertexBuffer(const void* data, size_t size, uint32_t stride)
	-> BufferHandle {
	auto [gpu_buf, address] = _impl->CreateGPUBuffer(size, data, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
	return _impl->meshPool.Create(std::move(gpu_buf), static_cast<uint32_t>(size / stride),
								  address);
}

auto RenderContext::CreateIndexBuffer(const void* data, size_t size) -> BufferHandle {
	auto [gpu_buf, address] = _impl->CreateGPUBuffer(size, data, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
	return _impl->meshPool.Create(std::move(gpu_buf),
								  static_cast<uint32_t>(size / sizeof(uint32_t)), address);
}

void RenderContext::DestroyBuffer(BufferHandle handle) {
	if (handle != BufferHandle::Invalid) {
		_impl->meshPool.Destroy(handle);
	}
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

	auto layout = Vk::PipelineLayoutBuilder(impl->ctx.Device())
					  .AddDescriptorSetLayout(impl->bindlessLayout.Get())
					  .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
									   sizeof(ZHLN::ObjectConstants))
					  .Build();

	auto pipeline = Vk::PipelineBuilder{}
						.Shaders(shaders)
						.Layout(layout.Get())
						.DepthFormat(VK_FORMAT_D32_SFLOAT);

	if (desc.doubleSided) {
		pipeline.CullNone();
	} else {
		pipeline.CullBack();
	}

	if (desc.alphaBlend) {
		pipeline.ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT}); // Output straight to the Lit pass
		pipeline.DepthWrite(false); // DO NOT write to depth, to preserve opaque occlusion
		pipeline.AlphaBlend();
	} else {
		pipeline.ColorFormats(ActiveGBuffer::array);
	}

	if (desc.isLineList) {
		pipeline.Topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	}

	auto finalPipelineRes = pipeline.Build(impl->ctx.Device());
	if (!finalPipelineRes) {
		ZHLN::Panic("FATAL: Failed to compile Material Pipeline!");
	}

	return {.pipeline = impl->materialPool.Create(std::move(*finalPipelineRes), std::move(layout)),
			.alphaMode = desc.alphaBlend ? 2u : 0u};
}

auto RenderContext::Impl::CreateTextureInternal(const void* data, uint32_t width, uint32_t height,
												bool isSRGB) -> uint32_t {
	const VkDevice device = ctx.Device();
	const size_t imageSize = static_cast<size_t>(width) * height * 4;

	uint32_t mipLevels = std::bit_width(std::max(width, height));

	const VkImageCreateInfo imgInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = isSRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
		.extent = {.width = width, .height = height, .depth = 1},
		.mipLevels = mipLevels,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				 VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};

	auto gpuImage = Vk::Image::Create(allocator.Get(), imgInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	// Textures are bound back to the graphics queue staging buffer (Safe blitting & stages)
	auto stagingAlloc = stagingRingBuffer.Allocate(imageSize);
	std::memcpy(stagingAlloc.mappedData, data, imageSize);

	Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
			cmd, gpuImage.Handle());

		ZHLN_BufferImageCopyDesc copyRegion = {.buffer = stagingAlloc.buffer,
											   .image = gpuImage.Handle(),
											   .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
											   .width = width,
											   .height = height,
											   .buffer_offset = stagingAlloc.offset,
											   .mip_level = 0,
											   .base_array_layer = 0};
		Vk::CopyBufferToImage(cmd, copyRegion);

		Vk::GenerateMipmaps(cmd, gpuImage.Handle(), width, height);
	});

	auto gpuView = isSRGB ? Vk::CreateView<VK_FORMAT_R8G8B8A8_SRGB>(
								device, gpuImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, mipLevels)
						  : Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(
								device, gpuImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

	uint32_t index = nextTextureIndex++;
	Vk::UpdateBindlessTextureSlot(device, index, gpuView.Get(), bindlessSets, 0);

	textureImages.push_back(std::move(gpuImage));
	textureViews.push_back(std::move(gpuView));

	return index;
}

uint32_t RenderContext::Impl::CreateTextureCubeInternal(const void* const* faceData, uint32_t width,
														uint32_t height) {
	const VkDevice device = ctx.Device();
	const size_t faceSize = static_cast<size_t>(width) * height * 4;

	const VkImageCreateInfo imgInfo = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = nullptr,
		.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8G8B8A8_UNORM,
		.extent = {.width = width, .height = height, .depth = 1},
		.mipLevels = 1,
		.arrayLayers = 6,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices = nullptr,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	auto gpuImage = Vk::Image::Create(allocator.Get(), imgInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	// Textures are bound back to the graphics queue staging buffer (Safe blitting & stages)
	auto stagingAlloc = stagingRingBuffer.Allocate(faceSize * 6);
	for (uint32_t i = 0; i < 6; ++i) {
		std::memcpy(static_cast<char*>(stagingAlloc.mappedData) + (i * faceSize), faceData[i],
					faceSize);
	}

	Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
			cmd, gpuImage.Handle());

		// 1. Generate the 6 cubemap regions on the stack (zero-overhead compile-time loop)
		auto regions = Vk::CreateCopyRegions<6>(stagingAlloc.offset, faceSize,
												{.width = width, .height = height, .depth = {}});

		// 2. Dispatch the copy using the new type-safe overload (layout defaults to
		// TRANSFER_DST)
		Vk::CopyBufferToImage(cmd, stagingAlloc.buffer, gpuImage.Handle(), regions);

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, gpuImage.Handle());
	});

	auto gpuView = Vk::CreateViewCube<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(), 1);

	uint32_t index = nextTextureIndex++;
	Vk::UpdateBindlessTextureSlot(device, index, gpuView.Get(), bindlessSets, 0);

	textureImages.push_back(std::move(gpuImage));
	textureViews.push_back(std::move(gpuView));

	return index;
}

auto RenderContext::Impl::CreateGPUBuffer(size_t size, const void* data,
										  VkBufferUsageFlags functionalUsage) const
	-> std::pair<Vk::Buffer, VkDeviceAddress> {

	VkBufferUsageFlags usage = functionalUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
							   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

	if (rtCtx.Valid()) {
		usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	}

	bool diffQueue = ctx.PhysicalInfo().graphics_family != ctx.PhysicalInfo().transfer_family;

	auto gpu_buf = Vk::Buffer::Create(allocator.Get(), size, usage, VMA_MEMORY_USAGE_GPU_ONLY);

	auto stagingAlloc = transferRingBuffer.Allocate(size);
	std::memcpy(stagingAlloc.mappedData, data, size);

	Vk::ExecuteImmediate<Vk::QueueType::Transfer>(
		ctx, transferCmdRing, transferRingBuffer, [&](VkCommandBuffer cmd) {
			Vk::CopyRingBuffer(cmd, stagingAlloc, gpu_buf, size);
			if (diffQueue) {
				auto [release, acquire] = Vk::BufferQueueBarrier::Create(
					{.buffer = gpu_buf.Handle(),
					 .size = size,
					 .src_queue_family = ctx.PhysicalInfo().transfer_family,
					 .dst_queue_family = ctx.PhysicalInfo().graphics_family,
					 .src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
					 .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
					 .dst_stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					 .dst_access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT});

				VkDependencyInfo depInfo = {
					.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
					.pNext = {},
					.dependencyFlags = {},
					.memoryBarrierCount = {},
					.pMemoryBarriers = {},
					.bufferMemoryBarrierCount = 1,
					.pBufferMemoryBarriers = &release,
					.imageMemoryBarrierCount = {},
					.pImageMemoryBarriers = {},
				};
				vkCmdPipelineBarrier2(cmd, &depInfo);

				ZHLN_LOCK(pendingAcquires.mutex) {
					pendingAcquires.buffers.push_back(acquire);
				}
			}
		});

	return {std::move(gpu_buf), Vk::GetBufferDeviceAddress(ctx.Device(), gpu_buf.Handle())};
}

auto RenderContext::CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB)
	-> uint32_t {
	return _impl->CreateTextureInternal(data, width, height, isSRGB);
}

auto RenderContext::CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height)
	-> uint32_t {
	return _impl->CreateTextureCubeInternal(faceData, width, height);
}

auto RenderContext::CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle {
	size_t size = (vertexCount * sizeof(VertexPosition)) + (vertexCount * sizeof(VertexAttributes));

	VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
							   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
							   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	if (_impl->rtCtx.Valid()) {
		usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
	}
	auto gpu_buf =
		Vk::Buffer::Create(_impl->allocator.Get(), size, usage, VMA_MEMORY_USAGE_GPU_ONLY);

	VkDeviceAddress address = Vk::GetBufferDeviceAddress(_impl->ctx.Device(), gpu_buf.Handle());
	return _impl->meshPool.Create(std::move(gpu_buf), vertexCount, address);
}

void RenderContext::UploadDebugVertices(const void* posData, size_t posSize, const void* attrData,
										size_t attrSize, uint32_t vertexCount) noexcept {
	uint32_t frameIdx = _impl->frame_index;
	auto* nativeMesh = _impl->meshPool.Resolve(_impl->debugMeshHandles[frameIdx]);
	if (nativeMesh == nullptr) {
		return;
	}

	size_t maxPosSize = 500000 * sizeof(VertexPosition);
	size_t maxAttrSize = 500000 * sizeof(VertexAttributes);

	auto mapped = nativeMesh->buffer.Map();
	char* basePtr = static_cast<char*>(mapped.data);

	std::memcpy(basePtr, posData, std::min(posSize, maxPosSize));
	std::memcpy(basePtr + maxPosSize, attrData, std::min(attrSize, maxAttrSize));

	nativeMesh->vertexCount = std::min(vertexCount, 500000u);
}

BufferHandle RenderContext::GetDebugMeshBuffer() const noexcept {
	return _impl->debugMeshHandles[_impl->frame_index];
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

	auto mappedRegion = _impl->morphDeltasBuffer.Map();
	float* gpuDeltas = std::bit_cast<float*>(mappedRegion.data) + (static_cast<size_t>(offset * 4));

	std::memcpy(gpuDeltas, deltas, count * sizeof(float) * 4);

	_impl->nextMorphDeltaIndex += count;
	return offset;
}

void RenderContext::SetShadowResolution(uint32_t resolution) {
	auto* impl = _impl.get();
	auto* device = impl->ctx.Device();
	Vk::WaitIdle(device);

	impl->shadowMap = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
		impl->allocator, impl->ctx, {.width = resolution, .height = resolution},
		{.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		 .arrayLayers = RenderContext::Impl::NUM_CASCADES});

	impl->shadowCascadeViews.clear();
	impl->shadowCascadeViews.resize(RenderContext::Impl::NUM_CASCADES);
	for (uint32_t i = 0; i < RenderContext::Impl::NUM_CASCADES; ++i) {
		impl->shadowCascadeViews[i] = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(
			impl->ctx.Device(), impl->shadowMap.image.Handle(), i, 1);
	}

	Vk::ExecuteImmediate(impl->ctx, impl->graphicsCmdRing, [&](VkCommandBuffer cmd) {
		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			cmd, impl->shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			cmd, impl->shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
	});

	Vk::UpdateBindlessTextureSlot(device, 0, impl->shadowMap.view.Get(), impl->bindlessSets, 2);
	ZHLN::Log("Shadow map dynamically resized on the GPU to {}x{}", resolution, resolution);
}

void RenderContext::SetAAState(const AAState& state) {
	_impl->aaState = state;
}

void RenderContext::BuildMeshBLAS(Mesh& mesh) {
	auto* impl = _impl.get();
	if (!impl->rtCtx.Valid()) {
		return;
	}

	auto* nativePosMesh = impl->meshPool.Resolve(mesh.posBuffer);
	auto* nativeIndexMesh = mesh.indexBuffer != BufferHandle::Invalid
								? impl->meshPool.Resolve(mesh.indexBuffer)
								: nullptr;

	if (nativePosMesh == nullptr) {
		return;
	}

	ZHLN_BlasGeometryDesc geom = {
		.vertex_data = nativePosMesh->vboAddress,
		.vertex_stride = sizeof(VertexPosition),
		.max_vertex = mesh.vertexCount,
		.vertex_format = VK_FORMAT_R32G32B32_SFLOAT,
		.index_data = (nativeIndexMesh != nullptr) ? nativeIndexMesh->vboAddress : 0,
		.index_type = (nativeIndexMesh != nullptr) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_NONE_KHR};

	uint32_t primitiveCount =
		(nativeIndexMesh != nullptr) ? mesh.indexCount / 3 : mesh.vertexCount / 3;

	ZHLN_AccelerationStructureSizes sizes;
	impl->rtCtx.GetBlasSizes(geom, primitiveCount, sizes);

	nativePosMesh->blasBuffer =
		Vk::Buffer::Create(impl->allocator.Get(), sizes.acceleration_structure_size,
						   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
							   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
						   VMA_MEMORY_USAGE_GPU_ONLY);
	nativePosMesh->blas =
		impl->rtCtx.CreateAS(nativePosMesh->blasBuffer.Handle(), sizes.acceleration_structure_size,
							 ZHLN_AS_TYPE_BOTTOM_LEVEL);
	nativePosMesh->blasAddress = impl->rtCtx.GetASAddress(nativePosMesh->blas);

	nativePosMesh->device = impl->ctx.Device();
	nativePosMesh->rtCtx = &impl->rtCtx;

	Vk::Buffer scratch = Vk::Buffer::Create(impl->allocator.Get(), sizes.build_scratch_size,
											VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
												VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
											VMA_MEMORY_USAGE_GPU_ONLY);

	{
		Vk::CommandPool tempPool(impl->ctx.Device(), impl->ctx.PhysicalInfo().graphics_family);
		if (tempPool.Allocate(1)) {
			VkCommandBuffer tempCmd = tempPool[0];
			{
				Vk::CommandBufferGuard guard(tempCmd);
				// 1. Cleanly drain pending family ownership acquires (locks and C-structs are
				// hidden)
				impl->pendingAcquires.Drain(tempCmd);

				// Synchronize the vertex/index buffer copy writes with the BLAS build reads
				Vk::MemoryBarrier(
					tempCmd, {.src_stage = VK_PIPELINE_STAGE_2_COPY_BIT,
							  .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
							  .dst_stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
							  .dst_access = VK_ACCESS_2_SHADER_READ_BIT});
				impl->rtCtx.CmdBuildBlas(
					tempCmd, geom, nativePosMesh->blas,
					Vk::GetBufferDeviceAddress(impl->ctx.Device(), scratch.Handle()),
					primitiveCount);
			}
			// 2. Synchronously submit and wait on the transfer staging timeline semaphore
			Vk::SubmitAndWait(impl->ctx.GraphicsQueue(), tempCmd,
							  impl->transferRingBuffer.GetSemaphore(),
							  impl->transferRingBuffer.GetCurrentValue(),
							  VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
		}
	}
}

void RenderContext::Impl::InitializeSystemTextures() {
	ZHLN::Log("[Resource Factory] Registering fallback system texture slots...");

	std::array<uint8_t, 4> blackPixel = {0, 0, 0, 0};
	uint32_t blackIdx = CreateTextureInternal(blackPixel.data(), 1, 1, false);

	std::array<uint8_t, 4> whitePixel = {255, 255, 255, 255};
	uint32_t whiteIdx = CreateTextureInternal(whitePixel.data(), 1, 1, true);

	std::array<uint8_t, 4> normalPixel = {128, 128, 255, 255};
	uint32_t normalIdx = CreateTextureInternal(normalPixel.data(), 1, 1, false);

	ZHLN::Assert(blackIdx == 0 && whiteIdx == 1 && normalIdx == 2,
				 "System textures allocated out of order! Expected [0, 1, 2], got [{}, {}, {}]",
				 blackIdx, whiteIdx, normalIdx);
}

void RenderContext::Impl::RegisterShaderWatcher(const char* path, std::function<void()> callback) {
	if constexpr (isDev) {
		shaderWatchers.push_back(
			{.path = path, .watcher = FileWatcher(path), .reloadCallback = std::move(callback)});
	}
}

auto RenderContext::BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx,
										  float scale, float randomness) -> uint32_t {
	return _impl->BakeProceduralTexture(width, height, variantIdx, scale, randomness, 0.0f);
}

void RenderContext::ProvokeDeviceLost() {
	_impl->ProvokeDeviceLostInternal();
}

void RenderContext::Impl::RegisterPipeline(const PipelineRegistration& reg) noexcept {
	reg.build();
	if constexpr (isDev) {
		for (const auto* path : reg.watchPaths) {
			RegisterShaderWatcher(path, reg.build);
		}
	}
}

void RenderContext::Impl::UploadClusterBounds(const JPH::Mat44& proj) {
	ZHLN::Array<ClusterBounds> cpuBounds(static_cast<size_t>(16 * 9 * 24));
	JPH::Mat44 invProj = proj.Inversed();

	float tsX = 2.0f / 16.0f;
	float tsY = 2.0f / 9.0f;

	auto Unproject = [&](const JPH::Vec4& coord) {
		JPH::Vec4 res = invProj * coord;
		return JPH::Vec3(res.GetX() / res.GetW(), res.GetY() / res.GetW(), res.GetZ() / res.GetW());
	};

	for (uint32_t z = 0; z < 24; ++z) {
		float n = 0.1f;
		float f = 1000.0f;
		float sNear = n * std::pow(f / n, (float)z / 24.0f);
		float sFar = n * std::pow(f / n, (float)(z + 1) / 24.0f);

		float tNear = (sNear - n) / (f - n);
		float tFar = (sFar - n) / (f - n);

		for (uint32_t y = 0; y < 9; ++y) {
			for (uint32_t x = 0; x < 16; ++x) {
				uint32_t cIdx = x + (y * 16) + (z * 144);

				std::array<JPH::Vec4, 4> ndc{
					{JPH::Vec4(-1.0f + x * tsX, -1.0f + y * tsY, 0.0f, 1.0f),
					 JPH::Vec4(-1.0f + (x + 1) * tsX, -1.0f + y * tsY, 0.0f, 1.0f),
					 JPH::Vec4(-1.0f + (x + 1) * tsX, -1.0f + (y + 1) * tsY, 0.0f, 1.0f),
					 JPH::Vec4(-1.0f + x * tsX, -1.0f + (y + 1) * tsY, 0.0f, 1.0f)}};

				std::array<JPH::Vec3, 4> pNear{};
				std::array<JPH::Vec3, 4> pFar{};
				for (int i = 0; i < 4; ++i) {
					pNear[i] = Unproject(JPH::Vec4(ndc[i].GetX(), ndc[i].GetY(), 0.0f, 1.0f));
					pFar[i] = Unproject(JPH::Vec4(ndc[i].GetX(), ndc[i].GetY(), 1.0f, 1.0f));
				}

				JPH::Vec3 pMin(1e30f, 1e30f, 1e30f);
				JPH::Vec3 pMax(-1e30f, -1e30f, -1e30f);

				for (int j = 0; j < 4; ++j) {
					JPH::Vec3 ptNear = pNear[j] + (pFar[j] - pNear[j]) * tNear;
					JPH::Vec3 ptFar = pNear[j] + (pFar[j] - pNear[j]) * tFar;
					pMin = JPH::Vec3::sMin(pMin, JPH::Vec3::sMin(ptNear, ptFar));
					pMax = JPH::Vec3::sMax(pMax, JPH::Vec3::sMax(ptNear, ptFar));
				}

				cpuBounds[cIdx].minPoint = JPH::Vec4(pMin.GetX(), pMin.GetY(), pMin.GetZ(), 1.0f);
				cpuBounds[cIdx].maxPoint = JPH::Vec4(pMax.GetX(), pMax.GetY(), pMax.GetZ(), 1.0f);
			}
		}
	}

	// Direct staging copy to GPU (Runs on main thread outside active render pass)
	auto stagingAlloc = stagingRingBuffer.Allocate(cpuBounds.size() * sizeof(ClusterBounds));
	std::memcpy(stagingAlloc.mappedData, cpuBounds.data(),
				cpuBounds.size() * sizeof(ClusterBounds));

	Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) {
		Vk::CopyRingBuffer(cmd, stagingAlloc, clusterBoundsBuffer,
						   cpuBounds.size() * sizeof(ClusterBounds));

		Vk::MemoryBarrier(cmd, {.src_stage = VK_PIPELINE_STAGE_2_COPY_BIT,
								.src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
								.dst_stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
								.dst_access = VK_ACCESS_2_SHADER_READ_BIT});
	});
}

} // namespace ZHLN
