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

std::expected<void, Error> RenderContext::Impl::CompileShadowPipeline(VkDevice device, const Resource::ShaderPair& shaderData) {
    return Vk::ShaderStages::Create(device, shaderData, "VSMain", "PSShadow")
        .transform_error([](auto err) -> Error { return err; })
        .and_then([&, device](auto&& shaders) -> std::expected<void, Error> {
            return Vk::PipelineLayoutBuilder(device)
                .AddDescriptorSetLayout(bindlessLayout.Get())
                .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ObjectConstants))
                .Build()
                .transform_error([](auto err) -> Error {
                    ZHLN::Log("Shadow pipeline layout creation error: {} (Category: {})", err.Message(), err.Category());
                    return RenderInitError::PipelineLayoutCreationFailed;
                })
                .and_then([&, device, shaders = std::forward<decltype(shaders)>(shaders)](auto&& layout) -> std::expected<void, Error> {
                    shadowPipelineLayout = std::forward<decltype(layout)>(layout);

                    return Vk::PipelineBuilder {}
                        .Shaders(shaders)
                        .Layout(shadowPipelineLayout.Get())
                        .DepthOnly()
                        .DepthFormat(VK_FORMAT_D32_SFLOAT)
                        .CullNone()
                        .Build(device)
                        .transform_error([](auto err) -> Error {
                            ZHLN::Log("Shadow pipeline compilation error: {} (Category: {})", err.Message(), err.Category());
                            return RenderInitError::PipelineCreationFailed;
                        })
                        .transform([&](auto&& pipeline) { shadowPipeline = std::forward<decltype(pipeline)>(pipeline); });
                });
        });
}

std::expected<void, Error> RenderContext::Impl::CompilePunctualShadowPipeline(VkDevice device, const Resource::ShaderPair& shaderData) {
    return Vk::ShaderStages::Create(device, shaderData)
        .transform_error([](auto err) -> Error { return err; })
        .and_then([&, device](auto&& shaders) -> std::expected<void, Error> {
            return Vk::PipelineLayoutBuilder(device)
                .AddDescriptorSetLayout(bindlessLayout.Get())
                .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT, sizeof(uint32_t))
                .Build()
                .transform_error([](auto err) -> Error {
                    ZHLN::Log("Punctual shadow pipeline layout creation error: {} (Category: {})", err.Message(), err.Category());
                    return RenderInitError::PipelineLayoutCreationFailed;
                })
                .and_then([&, device, shaders = std::forward<decltype(shaders)>(shaders)](auto&& layout) -> std::expected<void, Error> {
                    punctualShadowPipelineLayout = std::forward<decltype(layout)>(layout);

                    return Vk::PipelineBuilder {}
                        .Shaders(shaders)
                        .Layout(punctualShadowPipelineLayout.Get())
                        .DepthOnly()
                        .DepthFormat(VK_FORMAT_D32_SFLOAT)
                        .CullNone()
                        .Build(device)
                        .transform_error([](auto err) -> Error {
                            ZHLN::Log("Punctual shadow pipeline compilation error: {} (Category: {})", err.Message(), err.Category());
                            return RenderInitError::PipelineCreationFailed;
                        })
                        .transform([&](auto&& pipeline) { punctualShadowPipeline = std::forward<decltype(pipeline)>(pipeline); });
                });
        });
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

void RenderContext::CheckShaderReload() noexcept {
    if constexpr (isDev) {
        _impl->CheckShaderWatchers();
    }
}

void RenderContext::SetResolution([[maybe_unused]] const Extent2D& res) {
    _impl->resized = true;
}

auto RenderContext::CreateVertexBuffer(const void* data, size_t size, uint32_t stride) -> BufferHandle {
    return _impl->CreateGPUBuffer(size, data, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
        .transform([this, size, stride](auto&& pair) {
            return _impl->meshPool.Create(std::move(pair.first), static_cast<uint32_t>(size / stride), pair.second);
        })
        .value_or(BufferHandle::Invalid);
}

auto RenderContext::CreateIndexBuffer(const void* data, size_t size) -> BufferHandle {
    return _impl->CreateGPUBuffer(size, data, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
        .transform([this, size](auto&& pair) {
            return _impl->meshPool.Create(std::move(pair.first), static_cast<uint32_t>(size / sizeof(uint32_t)), pair.second);
        })
        .value_or(BufferHandle::Invalid);
}

void RenderContext::DestroyBuffer(BufferHandle handle) {
    if (handle != BufferHandle::Invalid) {
        _impl->meshPool.Destroy(handle);
    }
}

std::expected<Material, Error> RenderContext::CreateMaterial(const PipelineDesc& desc) {
    ZHLN_ShaderDesc v_desc = {.code = Vk::AsSpirV(desc.vertexShaderData), .size = desc.vertexShaderSize, .entry_point = nullptr};
    ZHLN_ShaderDesc f_desc = {.code = Vk::AsSpirV(desc.fragShaderData), .size = desc.fragShaderSize, .entry_point = nullptr};

    auto* impl = _impl.get();

    return Vk::ShaderStages::Create(impl->ctx.Device(), v_desc, f_desc)
        .transform_error([](auto err) -> Error {
            ZHLN::Log("Material shader compilation error: {} (Category: {})", err.Message(), err.Category());
            return MaterialCreationError::ShaderCompilationFailed;
        })
        .and_then([impl, &desc](auto&& shaders) -> std::expected<Material, Error> {
            // 1. Build the pipeline layout
            return Vk::PipelineLayoutBuilder(impl->ctx.Device())
                .AddDescriptorSetLayout(impl->bindlessLayout.Get())
                .AddPushConstant(VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(ObjectConstants))
                .Build()
                .transform_error([](auto err) -> Error {
                    ZHLN::Log("Material pipeline layout creation error: {} (Category: {})", err.Message(), err.Category());
                    return MaterialCreationError::PipelineLayoutCreationFailed;
                })
                .and_then([impl, &desc, &shaders](auto&& layout) -> std::expected<Material, Error> {
                    // 2. Configure the graphics pipeline builder options
                    auto pipeline = Vk::PipelineBuilder {}.Shaders(shaders).Layout(layout.Get()).DepthFormat(VK_FORMAT_D32_SFLOAT);

                    if (desc.doubleSided) {
                        pipeline.CullNone();
                    } else {
                        pipeline.CullBack();
                    }

                    if (desc.alphaBlend) {
                        pipeline.ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT}); // Output straight to the Lit pass
                        pipeline.DepthWrite(false);                             // DO NOT write to depth, to preserve opaque occlusion
                        pipeline.AlphaBlend();
                    } else {
                        pipeline.ColorFormats(ActiveGBuffer::array);
                    }

                    if (desc.isLineList) {
                        pipeline.Topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
                    }

                    // 3. Compile the graphics pipeline and return the initialized Material on success
                    return pipeline.Build(impl->ctx.Device())
                        .transform_error([](auto err) -> Error {
                            ZHLN::Log("Material pipeline creation error: {} (Category: {})", err.Message(), err.Category());
                            return MaterialCreationError::PipelineCreationFailed;
                        })
                        .transform([impl, &desc, &layout](auto&& compiledPipeline) {
                            return Material {
                                .pipeline = impl->materialPool.Create(
                                    std::forward<decltype(compiledPipeline)>(compiledPipeline), std::forward<decltype(layout)>(layout)
                                ),
                                .alphaMode = desc.alphaBlend ? 2u : 0u
                            };
                        });
                });
        });
}

void RenderContext::Impl::CheckShaderWatchers() noexcept {
    if constexpr (isDev) {
        bool anyReloaded = false;
        for (auto& watcher: shaderWatchers) {
            if (watcher.watcher.CheckModified()) {
                if (!anyReloaded) {
                    // Prevent write-after-read race conditions by forcing GPU idle
                    vkDeviceWaitIdle(ctx.Device());
                    anyReloaded = true;
                }
                watcher.reloadCallback();
            }
        }
    }
}

auto RenderContext::CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB) -> std::expected<uint32_t, Error> {
    return _impl->CreateTextureInternal(data, width, height, isSRGB);
}

auto RenderContext::CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height) -> std::expected<uint32_t, Error> {
    return _impl->CreateTextureCubeInternal(faceData, width, height);
}

std::expected<uint32_t, Error> RenderContext::Impl::CreateTextureInternal(const void* data, uint32_t width, uint32_t height, bool isSRGB) {
    auto* const  device    = ctx.Device();
    const size_t imageSize = static_cast<size_t>(width) * height * 4;
    uint32_t     mipLevels = std::bit_width(std::max(width, height));

    VkFormat          format = isSRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage  = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    return Vk::ImageBuilder {}
        .Texture2D(width, height, format, usage, mipLevels)
        .Build(allocator.Get())
        .transform_error([](VkResult res) -> Error { return res; })
        .and_then([&, device, width, height, isSRGB, mipLevels, data, imageSize](auto&& gpuImage) -> std::expected<uint32_t, Error> {
            auto stagingAlloc = stagingRingBuffer.Allocate(imageSize);
            std::memcpy(stagingAlloc.mappedData, data, imageSize);

            Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) {
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, gpuImage.Handle());

                Vk::CopyBufferToImage(
                    cmd, {.buffer           = stagingAlloc.buffer,
                          .image            = gpuImage.Handle(),
                          .layout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          .width            = width,
                          .height           = height,
                          .buffer_offset    = stagingAlloc.offset,
                          .mip_level        = 0,
                          .base_array_layer = 0}
                );

                Vk::GenerateMipmaps(cmd, gpuImage.Handle(), width, height);
            });

            auto gpuView = isSRGB ? Vk::CreateView<VK_FORMAT_R8G8B8A8_SRGB>(device, gpuImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, mipLevels) :
                                    Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);

            uint32_t index = nextTextureIndex++;
            Vk::UpdateBindlessTextureSlot(device, index, gpuView.Get(), bindlessSets, 0);

            textureImages.push_back(std::forward<decltype(gpuImage)>(gpuImage));
            textureViews.push_back(std::move(gpuView));

            return index;
        });
}

std::expected<uint32_t, Error> RenderContext::Impl::CreateTextureCubeInternal(const void* const* faceData, uint32_t width, uint32_t height) {
    auto* const       device   = ctx.Device();
    const size_t      faceSize = static_cast<size_t>(width) * height * 4;
    VkImageUsageFlags usage    = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    return Vk::ImageBuilder {}
        .TextureCube(width, VK_FORMAT_R8G8B8A8_UNORM, usage, 1)
        .Build(allocator.Get())
        .transform_error([](VkResult res) -> Error { return res; })
        .and_then([&, device, width, height, faceData, faceSize](auto&& gpuImage) -> std::expected<uint32_t, Error> {
            auto stagingAlloc = stagingRingBuffer.Allocate(faceSize * 6);
            for (uint32_t i = 0; i < 6; ++i) {
                std::memcpy(static_cast<char*>(stagingAlloc.mappedData) + (i * faceSize), faceData[i], faceSize);
            }

            Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) {
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, gpuImage.Handle());

                auto regions = Vk::CreateCopyRegions<6>(stagingAlloc.offset, faceSize, {.width = width, .height = height, .depth = {}});
                Vk::CopyBufferToImage(cmd, stagingAlloc.buffer, gpuImage.Handle(), regions);

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, gpuImage.Handle());
            });

            auto gpuView = Vk::CreateViewCube<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(), 1);

            uint32_t index = nextTextureIndex++;
            Vk::UpdateBindlessTextureSlot(device, index, gpuView.Get(), bindlessSets, 0);

            textureImages.push_back(std::forward<decltype(gpuImage)>(gpuImage));
            textureViews.push_back(std::move(gpuView));

            return index;
        });
}

std::expected<std::pair<Vk::Buffer, VkDeviceAddress>, VkResult>
    RenderContext::Impl::CreateGPUBuffer(size_t size, const void* data, VkBufferUsageFlags functionalUsage) const {
    VkBufferUsageFlags usage = functionalUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if (rtCtx.Valid()) {
        usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    bool diffQueue = ctx.PhysicalInfo().graphics_family != ctx.PhysicalInfo().transfer_family;

    return Vk::Buffer::Create(allocator.Get(), size, usage, VMA_MEMORY_USAGE_GPU_ONLY).transform([&, size, data, diffQueue](auto&& gpu_buf) {
        auto stagingAlloc = transferRingBuffer.Allocate(size);
        std::memcpy(stagingAlloc.mappedData, data, size);

        Vk::ExecuteImmediate<Vk::QueueType::Transfer>(ctx, transferCmdRing, transferRingBuffer, [&](VkCommandBuffer cmd) {
            Vk::CopyRingBuffer(cmd, stagingAlloc, gpu_buf, size);
            if (diffQueue) {
                auto [release, acquire] = Vk::BufferQueueBarrier::Create(
                    {.buffer           = gpu_buf.Handle(),
                     .size             = size,
                     .src_queue_family = ctx.PhysicalInfo().transfer_family,
                     .dst_queue_family = ctx.PhysicalInfo().graphics_family,
                     .src_stage        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     .src_access       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     .dst_stage        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     .dst_access       = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT}
                );

                Vk::BufferBarrier(cmd, release);

                ZHLN_LOCK(pendingAcquires.mutex) {
                    pendingAcquires.buffers.push_back(acquire);
                }
            }
        });

        VkDeviceAddress address = Vk::GetBufferAddress(ctx.Device(), gpu_buf.Handle());
        return std::make_pair(std::forward<decltype(gpu_buf)>(gpu_buf), address);
    });
}

auto RenderContext::CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle {
    size_t size = (vertexCount * sizeof(VertexPosition)) + (vertexCount * sizeof(VertexAttributes));

    VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (_impl->rtCtx.Valid()) {
        usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    return Vk::Buffer::Create(_impl->allocator.Get(), size, usage, VMA_MEMORY_USAGE_GPU_ONLY)
        .transform([this, vertexCount](auto&& gpu_buf) {
            VkDeviceAddress address = Vk::GetBufferAddress(_impl->ctx.Device(), gpu_buf.Handle());
            return _impl->meshPool.Create(std::forward<decltype(gpu_buf)>(gpu_buf), vertexCount, address);
        })
        .value_or(BufferHandle::Invalid);
}

void RenderContext::UploadDebugVertices(const void* posData, size_t posSize, const void* attrData, size_t attrSize, uint32_t vertexCount) noexcept {
    auto* nativeMesh = _impl->meshPool.Resolve(_impl->debugMeshHandles[]).value_or(nullptr);
    if (nativeMesh == nullptr) {
        return;
    }

    size_t maxPosSize  = 500000 * sizeof(VertexPosition);
    size_t maxAttrSize = 500000 * sizeof(VertexAttributes);

    auto  mapped  = nativeMesh->buffer.Map();
    char* basePtr = static_cast<char*>(mapped.data);

    std::memcpy(basePtr, posData, std::min(posSize, maxPosSize));
    std::memcpy(basePtr + maxPosSize, attrData, std::min(attrSize, maxAttrSize));

    nativeMesh->vertexCount = std::min(vertexCount, 500000u);
}

BufferHandle RenderContext::GetDebugMeshBuffer() const noexcept {
    return _impl->debugMeshHandles[];
}

void RenderContext::UpdateJointMatrices(uint32_t offset, const JPH::Mat44* matrices, uint32_t count) {
    if (count == 0) {
        return;
    }
    auto  mappedRegion = _impl->jointBuffers->Map();
    auto* gpuJoints    = std::bit_cast<JPH::Mat44*>(mappedRegion.data);

    std::memcpy(gpuJoints + offset, matrices, count * sizeof(JPH::Mat44));
}

uint32_t RenderContext::AllocateMorphDeltas(uint32_t count, const float* deltas) {
    uint32_t offset = _impl->nextMorphDeltaIndex;

    auto   mappedRegion = _impl->morphDeltasBuffer.Map();
    float* gpuDeltas    = std::bit_cast<float*>(mappedRegion.data) + (static_cast<size_t>(offset * 4));

    std::memcpy(gpuDeltas, deltas, count * sizeof(float) * 4);

    _impl->nextMorphDeltaIndex += count;
    return offset;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
std::expected<void, Error> RenderContext::SetShadowResolution(uint32_t resolution) {
    auto* impl   = _impl.get();
    auto* device = impl->ctx.Device();

    // 1. Wait for GPU idle monadically to prevent pipeline hazards during reallocation
    return Vk::WaitIdle(device)
        .transform_error([](auto err) -> Error {
            ZHLN::Log("SetShadowResolution WaitIdle failed: {}", ToString(err));
            return ShadowResolutionError::RecreationFailed;
        })
        // 2. Perform the target reallocation and layout transitions on success
        .transform([&](VkResult /*success*/) {
            impl->graphResources.shadowMap = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                impl->allocator, impl->ctx, {.width = resolution, .height = resolution},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = RenderContext::Impl::NUM_CASCADES}
            );

            impl->shadowCascadeViews.clear();
            impl->shadowCascadeViews.resize(RenderContext::Impl::NUM_CASCADES);
            for (uint32_t i = 0; i < RenderContext::Impl::NUM_CASCADES; ++i) {
                impl->shadowCascadeViews[i] =
                    Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(impl->ctx.Device(), impl->graphResources.shadowMap.image.Handle(), i, 1);
            }

            Vk::ExecuteImmediate(impl->ctx, impl->graphicsCmdRing, [&](VkCommandBuffer cmd) {
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                    cmd, impl->graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, impl->graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
            });

            ZHLN::Log("Shadow map dynamically resized on the GPU to {}x{}", resolution, resolution);
        });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

void RenderContext::SetAAState(const AAState& state) {
    _impl->aaState = state;
}

RenderResult RenderContext::BuildMeshBLAS(Mesh& mesh) noexcept {
    auto* impl = _impl.get();

    // Struct to propagate intermediate allocations down the monadic railway
    struct BuildContext {
        NativeMesh*                     posMesh;
        NativeMesh*                     indexMesh;
        ZHLN_BlasGeometryDesc           geom;
        uint32_t                        primitiveCount;
        ZHLN_AccelerationStructureSizes sizes;
        Vk::Buffer                      blasBuffer;
        VkAccelerationStructureKHR      blas;
        Vk::Buffer                      scratch;
    };

    // Invoking the default constructor directly resolves the ambiguity
    return std::expected<void, Error>()
        .and_then([&]() -> std::expected<BuildContext, Error> {
            if (!impl->rtCtx.Valid()) {
                return std::unexpected(VulkanCallError::FeatureNotPresent);
            }
            // Resolve the vertex buffer. Propagates ResourcePoolError if invalid/stale
            return impl->meshPool.Resolve(mesh.posBuffer)
                .transform_error([](auto err) -> Error { return err; })
                .and_then([&](auto* pos) -> std::expected<BuildContext, Error> {
                    // Optional index buffer: use value_or(nullptr) to handle missing indices cleanly
                    auto* index = (mesh.indexBuffer != BufferHandle::Invalid) ? impl->meshPool.Resolve(mesh.indexBuffer).value_or(nullptr) : nullptr;
                    return BuildContext {
                        .posMesh = pos, .indexMesh = index, .geom = {}, .primitiveCount = {}, .sizes = {}, .blasBuffer = {}, .blas = nullptr, .scratch = {}
                    };
                });
        })
        .and_then([&](BuildContext b) -> std::expected<BuildContext, Error> {
            b.geom = {
                .vertex_data   = b.posMesh->vboAddress,
                .vertex_stride = sizeof(VertexPosition),
                .max_vertex    = mesh.vertexCount,
                .vertex_format = VK_FORMAT_R32G32B32_SFLOAT,
                .index_data    = (b.indexMesh != nullptr) ? b.indexMesh->vboAddress : 0,
                .index_type    = (b.indexMesh != nullptr) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_NONE_KHR
            };
            b.primitiveCount = (b.indexMesh != nullptr) ? mesh.indexCount / 3 : mesh.vertexCount / 3;

            impl->rtCtx.GetBlasSizes(b.geom, b.primitiveCount, b.sizes);

            return Vk::Buffer::Create(
                       impl->allocator.Get(), b.sizes.acceleration_structure_size,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY
            )
                .transform_error([](VkResult res) -> Error { return {res}; })
                .transform([b = std::move(b)](auto&& buffer) mutable {
                    b.blasBuffer = std::forward<decltype(buffer)>(buffer);
                    return std::move(b); // <-- Explicitly move to trigger the move constructor
                });
        })
        .and_then([&](BuildContext b) -> std::expected<BuildContext, Error> {
            b.blas = impl->rtCtx.CreateAS(b.blasBuffer.Handle(), b.sizes.acceleration_structure_size, ZHLN_AS_TYPE_BOTTOM_LEVEL);
            if (b.blas == VK_NULL_HANDLE) {
                return std::unexpected(VulkanCallError::VulkanCallFailed);
            }

            return Vk::Buffer::Create(
                       impl->allocator.Get(), b.sizes.build_scratch_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       VMA_MEMORY_USAGE_GPU_ONLY
            )
                .transform_error([](VkResult res) -> Error { return {res}; })
                .transform([b = std::move(b)](auto&& buffer) mutable {
                    b.scratch = std::forward<decltype(buffer)>(buffer);
                    return std::move(b); // <-- Explicitly move to trigger the move constructor
                });
        })
        .and_then([&](BuildContext b) -> std::expected<void, Error> {
            Vk::CommandPool<Vk::QueueType::Graphics> tempPool(impl->ctx.Device(), impl->ctx.PhysicalInfo().graphics_family);
            auto                                     alloc_res = tempPool.Allocate(1);
            if (!alloc_res) [[unlikely]] {
                return std::unexpected(alloc_res.error());
            }

            VkCommandBuffer tempCmd = tempPool[0];
            {
                Vk::CommandBufferGuard guard(tempCmd);
                impl->pendingAcquires.Drain(tempCmd);

                Vk::MemoryBarrier(
                    tempCmd, {.src_stage  = VK_PIPELINE_STAGE_2_COPY_BIT,
                              .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              .dst_stage  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                              .dst_access = VK_ACCESS_2_SHADER_READ_BIT}
                );
                impl->rtCtx.CmdBuildBlas(tempCmd, b.geom, b.blas, Vk::GetBufferAddress(impl->ctx.Device(), b.scratch.Handle()), b.primitiveCount);
            }

            return Vk::SubmitAndWait(
                       impl->ctx.GraphicsQueue(), tempCmd, impl->transferRingBuffer.GetSemaphore(), impl->transferRingBuffer.GetCurrentValue(),
                       VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
            )
                .transform_error([](auto err) -> Error { return err; })
                .transform([&]() {
                    // Safely update the native mesh object only upon successful GPU submission
                    b.posMesh->blasBuffer  = std::move(b.blasBuffer);
                    b.posMesh->blas        = b.blas;
                    b.posMesh->blasAddress = impl->rtCtx.GetASAddress(b.blas);
                    b.posMesh->device      = impl->ctx.Device();
                    b.posMesh->rtCtx       = &impl->rtCtx;
                });
        });
}

std::expected<void, Error> RenderContext::Impl::InitializeSystemTextures() noexcept {
    ZHLN::Log("[Resource Factory] Registering fallback system texture slots...");

    std::array<uint8_t, 4> blackPixel  = {0, 0, 0, 0};
    std::array<uint8_t, 4> whitePixel  = {255, 255, 255, 255};
    std::array<uint8_t, 4> normalPixel = {128, 128, 255, 255};

    return CreateTextureInternal(blackPixel.data(), 1, 1, false).and_then([&, whitePixel, normalPixel](uint32_t blackIdx) -> std::expected<void, Error> {
        return CreateTextureInternal(whitePixel.data(), 1, 1, true).and_then([&, blackIdx, normalPixel](uint32_t whiteIdx) -> std::expected<void, Error> {
            return CreateTextureInternal(normalPixel.data(), 1, 1, false).and_then([&, blackIdx, whiteIdx](uint32_t normalIdx) -> std::expected<void, Error> {
                // Verify sequential bindless indices to prevent runtime offset issues
                if (blackIdx != 0 || whiteIdx != 1 || normalIdx != 2) {
                    return std::unexpected(RenderInitError::SubsystemAllocationFailed);
                }
                return {};
            });
        });
    });
}

void RenderContext::Impl::RegisterShaderWatcher(const char* path, std::function<void()> callback) {
    if constexpr (isDev) {
        shaderWatchers.push_back({.path = path, .watcher = FileWatcher(path), .reloadCallback = std::move(callback)});
    }
}

auto RenderContext::BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness)
    -> std::expected<uint32_t, Error> {
    return _impl->BakeProceduralTexture(width, height, variantIdx, scale, randomness, 0.0f);
}

void RenderContext::ProvokeDeviceLost() {
    _impl->ProvokeDeviceLostInternal();
}

void RenderContext::Impl::RegisterPipeline(const PipelineRegistration& reg) noexcept {
    reg.build();
    if constexpr (isDev) {
        for (const auto* path: reg.watchPaths) {
            RegisterShaderWatcher(path, reg.build);
        }
    }
}

void RenderContext::Impl::UploadClusterBounds(const JPH::Mat44& proj) {
    ZHLN::Array<ClusterBounds> cpuBounds(static_cast<size_t>(16 * 9 * 24));
    JPH::Mat44                 invProj = proj.Inversed();

    float tsX = 2.0f / 16.0f;
    float tsY = 2.0f / 9.0f;

    auto Unproject = [&](const JPH::Vec4& coord) {
        JPH::Vec4 res = invProj * coord;
        return JPH::Vec3(res.GetX() / res.GetW(), res.GetY() / res.GetW(), res.GetZ() / res.GetW());
    };

    for (uint32_t z = 0; z < 24; ++z) {
        float n     = 0.1f;
        float f     = 1000.0f;
        float sNear = n * std::pow(f / n, (float) z / 24.0f);
        float sFar  = n * std::pow(f / n, (float) (z + 1) / 24.0f);

        float tNear = (sNear - n) / (f - n);
        float tFar  = (sFar - n) / (f - n);

        for (uint32_t y = 0; y < 9; ++y) {
            for (uint32_t x = 0; x < 16; ++x) {
                uint32_t cIdx = x + (y * 16) + (z * 144);

                std::array<JPH::Vec4, 4> ndc {
                    {JPH::Vec4(-1.0f + x * tsX, -1.0f + y * tsY, 0.0f, 1.0f), JPH::Vec4(-1.0f + (x + 1) * tsX, -1.0f + y * tsY, 0.0f, 1.0f),
                     JPH::Vec4(-1.0f + (x + 1) * tsX, -1.0f + (y + 1) * tsY, 0.0f, 1.0f), JPH::Vec4(-1.0f + x * tsX, -1.0f + (y + 1) * tsY, 0.0f, 1.0f)}
                };

                std::array<JPH::Vec3, 4> pNear {};
                std::array<JPH::Vec3, 4> pFar {};
                for (int i = 0; i < 4; ++i) {
                    pNear[i] = Unproject(JPH::Vec4(ndc[i].GetX(), ndc[i].GetY(), 0.0f, 1.0f));
                    pFar[i]  = Unproject(JPH::Vec4(ndc[i].GetX(), ndc[i].GetY(), 1.0f, 1.0f));
                }

                JPH::Vec3 pMin(1e30f, 1e30f, 1e30f);
                JPH::Vec3 pMax(-1e30f, -1e30f, -1e30f);

                for (int j = 0; j < 4; ++j) {
                    JPH::Vec3 ptNear = pNear[j] + (pFar[j] - pNear[j]) * tNear;
                    JPH::Vec3 ptFar  = pNear[j] + (pFar[j] - pNear[j]) * tFar;
                    pMin             = JPH::Vec3::sMin(pMin, JPH::Vec3::sMin(ptNear, ptFar));
                    pMax             = JPH::Vec3::sMax(pMax, JPH::Vec3::sMax(ptNear, ptFar));
                }

                cpuBounds[cIdx].minPoint = JPH::Vec4(pMin.GetX(), pMin.GetY(), pMin.GetZ(), 1.0f);
                cpuBounds[cIdx].maxPoint = JPH::Vec4(pMax.GetX(), pMax.GetY(), pMax.GetZ(), 1.0f);
            }
        }
    }

    // Direct staging copy to GPU (Runs on main thread outside active render pass)
    auto stagingAlloc = stagingRingBuffer.Allocate(cpuBounds.size() * sizeof(ClusterBounds));
    std::memcpy(stagingAlloc.mappedData, cpuBounds.data(), cpuBounds.size() * sizeof(ClusterBounds));

    Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) {
        Vk::CopyRingBuffer(cmd, stagingAlloc, clusterBoundsBuffer, cpuBounds.size() * sizeof(ClusterBounds));

        Vk::MemoryBarrier(
            cmd, {.src_stage  = VK_PIPELINE_STAGE_2_COPY_BIT,
                  .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  .dst_stage  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  .dst_access = VK_ACCESS_2_SHADER_READ_BIT}
        );
    });
}

} // namespace ZHLN
