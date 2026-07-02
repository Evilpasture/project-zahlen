// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/RenderResources.cpp
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include "Texture.hpp"
#include "Zahlen/Types.hpp"

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

	shadowPipeline = Vk::PipelineBuilder{}
						 .Shaders(shaders)
						 .Layout(shadowPipelineLayout.Get())
						 .DepthOnly()
						 .DepthFormat(VK_FORMAT_D32_SFLOAT)
						 .CullNone()
						 .Build(device);
}

void RenderContext::Impl::CompilePunctualShadowPipeline(VkDevice device,
														const Resource::ShaderPair& shaderData) {
	auto shaders = Vk::ShaderStages::Create(device, shaderData);

	punctualShadowPipelineLayout =
		Vk::PipelineLayoutBuilder(device)
			.AddDescriptorSetLayout(bindlessLayout.Get())
			.AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT, sizeof(uint32_t))
			.Build();

	punctualShadowPipeline = Vk::PipelineBuilder{}
								 .Shaders(shaders)
								 .Layout(punctualShadowPipelineLayout.Get())
								 .DepthOnly()
								 .DepthFormat(VK_FORMAT_D32_SFLOAT)
								 .CullNone()
								 .Build(device);
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

	auto finalPipeline = pipeline.Build(impl->ctx.Device());

	return {.pipeline = impl->materialPool.Create(std::move(finalPipeline), std::move(layout)),
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
		.extent{.width = width, .height = height, .depth = 1},
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

	auto staging = Vk::Buffer::Create(allocator.Get(), imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
									  VMA_MEMORY_USAGE_CPU_ONLY);
	std::memcpy(staging.Map().data, data, imageSize);

	{
		Vk::ImmediateCommand cmd(device, ctx.GraphicsQueue(), ctx.PhysicalInfo().graphics_family);

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

		ZHLN_GenerateMipmaps(cmd, gpuImage.Handle(), width, height, mipLevels);

		cmd.KeepAlive(std::move(staging));
	}

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

	auto gpuImage = Vk::Image::Create(allocator.Get(), imgInfo, VMA_MEMORY_USAGE_GPU_ONLY);

	Vk::Buffer staging = Vk::Buffer::Create(
		allocator.Get(), faceSize * 6, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

	{
		auto mapped = staging.Map();
		for (uint32_t i = 0; i < 6; ++i) {
			std::memcpy(static_cast<char*>(mapped.data) + (i * faceSize), faceData[i], faceSize);
		}
	}

	{
		Vk::ImmediateCommand cmd(device, ctx.GraphicsQueue(), ctx.PhysicalInfo().graphics_family);

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
			cmd, gpuImage.Handle());

		// Batch all six cubemap faces into a single copy pipeline dispatch
		std::array<VkBufferImageCopy2, 6> regions{};
		for (uint32_t i = 0; i < 6; ++i) {
			regions[i] = {.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
						  .pNext = nullptr,
						  .bufferOffset = i * faceSize,
						  .bufferRowLength = 0,
						  .bufferImageHeight = 0,
						  .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
											   .mipLevel = 0,
											   .baseArrayLayer = i,
											   .layerCount = 1},
						  .imageOffset = {.x = 0, .y = 0, .z = 0},
						  .imageExtent = {.width = width, .height = height, .depth = 1}};
		}

		VkCopyBufferToImageInfo2 copyInfo = {.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
											 .pNext = nullptr,
											 .srcBuffer = staging.Handle(),
											 .dstImage = gpuImage.Handle(),
											 .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
											 .regionCount = static_cast<uint32_t>(regions.size()),
											 .pRegions = regions.data()};
		vkCmdCopyBufferToImage2(cmd, &copyInfo);

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, gpuImage.Handle());

		cmd.KeepAlive(std::move(staging));
	}

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

	auto gpu_buf = Vk::Buffer::Create(allocator.Get(), size, usage, VMA_MEMORY_USAGE_GPU_ONLY);

	{
		Vk::ImmediateCommand cmd(ctx.Device(), ctx.GraphicsQueue(),
								 ctx.PhysicalInfo().graphics_family);
		auto staging = Vk::UploadToBuffer(allocator.Get(), cmd, gpu_buf, data, size);
		cmd.KeepAlive(std::move(staging));
	}

	VkBufferDeviceAddressInfo bdaInfo = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		.pNext = {},
		.buffer = gpu_buf.Handle(),
	};
	VkDeviceAddress address = vkGetBufferDeviceAddress(ctx.Device(), &bdaInfo);

	return {std::move(gpu_buf), address};
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
	vkDeviceWaitIdle(device);

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

	{
		Vk::ImmediateCommand cmd(impl->ctx.Device(), impl->ctx.GraphicsQueue(),
								 impl->ctx.PhysicalInfo().graphics_family);

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
			cmd, impl->shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);

		Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
							 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
			cmd, impl->shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT);
	}

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
		Vk::ImmediateCommand cmd(impl->ctx.Device(), impl->ctx.GraphicsQueue(),
								 impl->ctx.PhysicalInfo().graphics_family);
		impl->rtCtx.CmdBuildBlas(cmd, geom, nativePosMesh->blas,
								 Vk::GetBufferDeviceAddress(impl->ctx.Device(), scratch.Handle()),
								 primitiveCount);
		cmd.KeepAlive(std::move(scratch));
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

} // namespace ZHLN
