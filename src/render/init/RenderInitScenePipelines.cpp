// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cstring>
#include <span>
#include <vector>

namespace ZHLN {

auto RenderContext::Impl::BuildParticlePipelines() -> std::expected<void, ErrorCode> {


    size_t particleBufferSize = RenderContext::Impl::kGpuParticleCount * sizeof(Particle);
    auto   pb_res             = Vk::Buffer::Create(
        allocator.Get(), particleBufferSize,
        Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress | Vk::BufferUsage::TransferDst | Vk::BufferUsage::Vertex,
        Vk::MemoryUsage::GPUOnly
    );
    if (!pb_res) {
        return std::unexpected(pb_res.error());
    }
    particleBuffer = std::move(*pb_res);

    auto csShader = Vk::CreateShaderDesc<Shaders::Modules::ParticleUpdateCS>();

    if (auto built = particleUpdatePass.BuildHeap(ctx.Device(), csShader, &sceneHeapMappings.info, 0, pipelineCache.Get()); !built) {
        return std::unexpected(built.error());
    }

    particleRenderLayout = emptyPipelineLayout;

    return LoadAndCreateShaders(
               MakeStageSource<ShaderStage::Vertex, Shaders::Modules::ParticleRenderVS>(),
               MakeStageSource<ShaderStage::Fragment, Shaders::Modules::ParticleRenderPS>()
    )
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders.View())
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .AdditiveBlend()
                .AlphaBlend()
                .CullNone()
                .Cache(pipelineCache.Get())
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { particleRenderPipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::BuildMeshParticlePipelines() -> std::expected<void, ErrorCode> {


    auto csMeshShader = Vk::CreateShaderDesc<Shaders::Modules::MeshParticleUpdateCS>();

    if (!meshParticleUpdatePass.BuildHeap(ctx.Device(), csMeshShader, &sceneHeapMappings.info, 0, pipelineCache.Get())) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    meshParticleRenderLayout = emptyPipelineLayout;


    return LoadAndCreateShaders(
               MakeStageSource<ShaderStage::Vertex, Shaders::Modules::MeshParticleRenderVS>(),
               MakeStageSource<ShaderStage::Fragment, Shaders::Modules::MeshParticleRenderPS>()
    )
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders.View())
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                .Cache(pipelineCache.Get())
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { meshParticleRenderPipeline = std::forward<decltype(pipeline)>(pipeline); });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {

            return LoadAndCreateShaders(
                       MakeStageSource<ShaderStage::Vertex, Shaders::Modules::MeshParticleShadowVS>(),
                       MakeStageSource<ShaderStage::Fragment, Shaders::Modules::MeshParticleShadowPS>()
            )
                .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
                    return Vk::PipelineBuilder<0, true> {}
                        .Shaders(shaders.View())
                        .Layout(emptyPipelineLayout)
                        .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                        .DepthOnly()
                        .DepthFormat(VK_FORMAT_D32_SFLOAT)
                        .ViewMask(ShadowRenderer::kCascadeViewMask)
                        .CullNone()
                        .Cache(pipelineCache.Get())
                        .Build(ctx.Device())
                        .transform([&](auto&& pipeline) -> auto { meshParticleShadowPipeline = std::forward<decltype(pipeline)>(pipeline); });
                });
        });
}

auto RenderContext::Impl::BuildSkinningPipeline() -> std::expected<void, ErrorCode> {
    return Vk::PipelineLayoutBuilder(ctx.Device())
        .AddPushConstant(VK_SHADER_STAGE_COMPUTE_BIT, sizeof(SkinningConstants))
        .Build()
        .transform_error([](auto) -> ErrorCode { return Vk::PipelineBuilderError::LayoutCreationFailed; })
        .and_then([&](auto&& layout) -> std::expected<void, ErrorCode> {
            skinningPass.pipelineLayout = std::forward<decltype(layout)>(layout);
            return LoadAndCreateComputeShader(
                       MakeStageSource<ShaderStage::Compute, Shaders::Modules::SkinningCS>(), skinningPass.pipelineLayout.Get(), skinningPass
            )
                .transform([&](auto&& pipeline) -> auto { skinningPass.pipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::AllocateDynamicVertexBuffers(
    size_t                           maxVertices,
    PerFrame<Vk::Buffer>&            bufs,
    PerFrame<VkDeviceAddress>&       addrs,
    const char*                      label,
    Vk::BufferUsage                  extraFlags
) noexcept -> std::expected<void, ErrorCode> {
    const size_t bufferSize = maxVertices * (sizeof(VertexPosition) + sizeof(VertexAttributes));
    PerFrame<Vk::Buffer> created;
    PerFrame<VkDeviceAddress> createdAddresses;
    defer _([&] {
        for (auto& buffer: created) allocator.DestroyBuffer(buffer);
    });
    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
        auto res = Vk::Buffer::Create(
            allocator.Get(), bufferSize, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress | extraFlags,
            Vk::MemoryUsage::CPUToGPU
        );
        if (!res) return std::unexpected(res.error());
        created[i] = std::move(*res);
        createdAddresses[i] = ctx.BufferAddress(created[i].Handle());
    }
    // This helper is used at initialization; a reinit must be done after idle.
    for (auto& buffer: bufs) allocator.DestroyBuffer(buffer);
    bufs = std::move(created);
    addrs = createdAddresses;
    ZHLN::Log("Allocated per-frame dynamic {} VBOs ({} bytes).", label, bufferSize);
    return {};
}

auto RenderContext::Impl::InitLineBuffers() noexcept -> std::expected<void, ErrorCode> {
    return AllocateDynamicVertexBuffers(kMaxLineVertices, frames.lineVbos, frames.lineVboAddresses, "line", Vk::BufferUsage::Vertex);
}

auto RenderContext::Impl::BuildLinePipeline() -> std::expected<void, ErrorCode> {
    linePipelineLayout = emptyPipelineLayout;



    return LoadAndCreateShaders(
               MakeStageSource<ShaderStage::Vertex, Shaders::Modules::BasicVSForward>(),
               MakeStageSource<ShaderStage::Fragment, Shaders::Modules::ForwardPS>()
    )
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder<1, true> {}
                .Shaders(shaders.View())
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .Topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
                .CullNone()
                .AlphaBlend()
                .Cache(pipelineCache.Get())
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { linePipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::InitShadowResources() -> std::expected<void, ErrorCode> {
    auto shadowSamplerBuilder = Vk::SamplerBuilder {}.Linear().ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE).DepthCompare();

    return shadowSamplerBuilder.Build(ctx.Device())
        .transform_error([](auto err) -> ErrorCode { return err; })

        .and_then([&](auto&& sampler) -> std::expected<void, ErrorCode> {
            shadowSampler     = std::forward<decltype(sampler)>(sampler);
            shadowSamplerInfo = shadowSamplerBuilder.Info();
            return {};
        })

        .and_then([&]() -> std::expected<void, ErrorCode> { return targets.InitShadows(); })

        .and_then([&]() -> std::expected<void, ErrorCode> { return shadows.InitResources(*this); })

        .and_then([&]() -> auto {
            return CreatePerFrame(
                       allocator, sizeof(FrameUniforms), Vk::BufferUsage::Uniform | Vk::BufferUsage::ShaderDeviceAddress,
                       Vk::MemoryUsage::CPUToGPU
            )
                .transform_error([](auto err) -> ErrorCode { return err; });
        })

        .and_then([&](auto&& fub) -> auto {
            frames.frameUniformBuffers = std::forward<decltype(fub)>(fub);
            return CreatePerFrame(
                       allocator, sizeof(Light) * 128, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
                       Vk::MemoryUsage::CPUToGPU
            )
                .transform_error([](auto err) -> ErrorCode { return err; });
        })

        .transform([&](auto&& lsb) -> void { frames.lightStorageBuffers = std::forward<decltype(lsb)>(lsb); });
}

auto RenderContext::Impl::BuildDecalPipeline() -> std::expected<void, ErrorCode> {


    static constexpr std::array<VkFormat, 2> decalFormats = {VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R8G8B8A8_UNORM};



    const Vk::ReflectedStageInput reflectInputs[2] = {
        {.shader = Vk::CreateShaderDesc<Shaders::Modules::DecalVS>(), .stage = VK_SHADER_STAGE_VERTEX_BIT},
        {.shader = Vk::CreateShaderDesc<Shaders::Modules::DecalPS>(), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    if (!decalDescLayout.Build(ctx.Device(), std::span {reflectInputs})) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    BuildDecalHeapMappings();
    decalPipelineLayout = emptyPipelineLayout;

    std::vector<VkDescriptorSetAndBindingMappingEXT> mergedEntries;
    mergedEntries.insert(mergedEntries.end(), decalHeapMappings.entries.begin(), decalHeapMappings.entries.end());
    mergedEntries.insert(mergedEntries.end(), decalSceneHeapMappings.entries.begin(), decalSceneHeapMappings.entries.end());
    const VkShaderDescriptorSetAndBindingMappingInfoEXT mergedInfo = {
        .sType        = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,
        .pNext        = nullptr,
        .mappingCount = static_cast<uint32_t>(mergedEntries.size()),
        .pMappings    = mergedEntries.empty() ? nullptr : mergedEntries.data(),
    };

    return LoadAndCreateShaders(
               MakeStageSource<ShaderStage::Vertex, Shaders::Modules::DecalVS>(),
               MakeStageSource<ShaderStage::Fragment, Shaders::Modules::DecalPS>()
    )
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder<2, true> {}
                .Shaders(shaders.View())
                .Layout(emptyPipelineLayout)
                .HeapMappings(&mergedInfo, &mergedInfo)
                .ColorFormats(decalFormats)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .CullFront()
                .AlphaBlend()
                .Cache(pipelineCache.Get())
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { decalPipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::InitCSGPipelines() -> std::expected<void, ErrorCode> {
    auto shaders = LoadAndCreateShaders(
        MakeStageSource<ShaderStage::Vertex, Shaders::Modules::BasicVS>(),
        MakeStageSource<ShaderStage::Fragment, Shaders::Modules::BasicPS>()
    );
    if (!shaders) {
        return std::unexpected(shaders.error());
    }

    // One owner outlives all three synchronous builds; the view only carries
    // pointers into its current SPIR-V buffers.
    const auto stages = shaders->View();
    csgPipelineLayout = emptyPipelineLayout;
    return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
        .Shaders(stages)
        .Layout(emptyPipelineLayout)
        .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
        .ColorFormats(ActiveGBuffer::array)
        .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
        .DepthTest(true)
        .DepthWrite(false)
        .CullNone()
        .ColorWriteEnable(false)
        .StencilWriteMask(1)
        .Cache(pipelineCache.Get())
        .Build(ctx.Device())
        .and_then([&](auto&& writePipeline) -> auto {
            csgWritePipeline = std::forward<decltype(writePipeline)>(writePipeline);

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(stages)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                .StencilCompareMask(VK_COMPARE_OP_NOT_EQUAL, 1)
                .Cache(pipelineCache.Get())
                .Build(ctx.Device())
                .transform_error([](auto e) -> ErrorCode { return e; });
        })
        .and_then([&](auto&& diffPipeline) -> auto {
            csgDifferencePipeline = std::forward<decltype(diffPipeline)>(diffPipeline);

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(stages)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                .StencilCompareMask(VK_COMPARE_OP_EQUAL, 1)
                .Cache(pipelineCache.Get())
                .Build(ctx.Device())
                .transform_error([](auto e) -> ErrorCode { return e; });
        })
        .transform([&](auto&& intersectPipeline) -> auto {
            csgIntersectionPipeline = std::forward<decltype(intersectPipeline)>(intersectPipeline);

            shaderReloads.Register("CSGStencil", {Shaders::Modules::BasicVS::Path, Shaders::Modules::BasicPS::Path}, [this]() -> void {
                auto res = InitCSGPipelines();
                if (!res) {
                    ZHLN::Log("ERROR: Failed to hot-reload CSG stencil pipelines: {}", res.error());
                } else {
                    ZHLN::Log("[Shader Reload] CSG Stencil pipelines hot-reloaded successfully.");
                }
            });
        });
}

auto RenderContext::Impl::BuildHangGpuPipeline() -> std::expected<void, ErrorCode> {
    auto built = Vk::PipelineLayoutBuilder(ctx.Device())
                     .Build()
                     .transform_error([](auto) -> ErrorCode { return Vk::PipelineBuilderError::LayoutCreationFailed; })
                     .and_then([&](auto&& layout) -> std::expected<void, ErrorCode> {
                         hangGpuPass.pipelineLayout = std::forward<decltype(layout)>(layout);
                         return LoadAndCreateComputeShader(
                                    MakeStageSource<ShaderStage::Compute, Shaders::Modules::HangGpuCS>(),
                                    hangGpuPass.pipelineLayout.Get(), hangGpuPass
                         )
                             .transform([&](auto&& pipeline) -> auto { hangGpuPass.pipeline = std::forward<decltype(pipeline)>(pipeline); });
                     });
    if (!built) {
        ZHLN::Log("[GPU] hang_gpu pipeline unavailable ({}); ProvokeDeviceLost is a no-op.", built.error());
    }
    return {};
}

auto RenderContext::Impl::BuildHiZPipeline() -> std::expected<void, ErrorCode> {
    auto shader = Vk::CreateShaderDesc<Shaders::Modules::HizGenerateCS>();
    if (!hizDescLayout.Build(ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    if (auto built = Vk::BuildHeapPassBindings(
            heapManager, hizDescLayout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, hizHeapBindings
        );
        !built) {
        return std::unexpected(built.error());
    }

    return hizGeneratePass.BuildHeap(ctx.Device(), shader, hizHeapBindings.GetInfo(), hizHeapBindings.indexPushOffset, pipelineCache.Get());
}

auto RenderContext::Impl::InitCullingResources() -> std::expected<void, ErrorCode> {


    auto cullingShader = Vk::CreateShaderDesc<Shaders::Modules::CullingCS>();
    if (!cullingLayout.Build(ctx.Device(), cullingShader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    auto clusterCullingShader = Vk::CreateShaderDesc<Shaders::Modules::ClusterCullingCS>();
    auto clusterDispatch      = Vk::ReflectComputeDispatchSize(clusterCullingShader);
    if (!clusterDispatch) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    const size_t numClusters = static_cast<size_t>((*clusterDispatch)[0]) * (*clusterDispatch)[1] * (*clusterDispatch)[2];

    if (auto built = Vk::BuildHeapPassBindings(
            heapManager, cullingLayout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, cullingHeapBindings
        );
        !built) {
        return std::unexpected(built.error());
    }

    constexpr Vk::BufferUsage kInstanceUsage  = Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress;
    constexpr Vk::BufferUsage kIndirectUsage  = Vk::BufferUsage::Storage | Vk::BufferUsage::Indirect | Vk::BufferUsage::TransferDst |
                                                   Vk::BufferUsage::TransferSrc | Vk::BufferUsage::ShaderDeviceAddress;
    constexpr Vk::BufferUsage kCandidateUsage = Vk::BufferUsage::Storage | Vk::BufferUsage::TransferDst |
                                                   Vk::BufferUsage::ShaderDeviceAddress;
    constexpr Vk::BufferUsage kCountUsage     = Vk::BufferUsage::Storage | Vk::BufferUsage::TransferDst | Vk::BufferUsage::TransferSrc |
                                                   Vk::BufferUsage::ShaderDeviceAddress;

    return std::expected<void, ErrorCode> {}
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return CreatePerFrame(allocator, sizeof(InstanceData) * kGpuCullingMaxInstances, kInstanceUsage, Vk::MemoryUsage::CPUToGPU)
                .and_then([&](auto&& idb) {
                    frames.instanceDataBuffers = std::forward<decltype(idb)>(idb);
                    return CreatePerFrame(allocator, sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances, kIndirectUsage, Vk::MemoryUsage::GPUOnly);
                })
                .and_then([&](auto&& icb1) {
                    frames.indirectCommandsBuffers = std::forward<decltype(icb1)>(icb1);
                    return CreatePerFrame(allocator, sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances, kIndirectUsage, Vk::MemoryUsage::GPUOnly);
                })
                .and_then([&](auto&& icb2) {
                    frames.indirectCommandsBuffersPass2 = std::forward<decltype(icb2)>(icb2);
                    return CreatePerFrame(allocator, sizeof(uint32_t) * kGpuCullingMaxInstances, kCandidateUsage, Vk::MemoryUsage::GPUOnly);
                })
                .and_then([&](auto&& spcb) {
                    frames.secondPassCandidatesBuffers = std::forward<decltype(spcb)>(spcb);
                    return CreatePerFrame(allocator, sizeof(uint32_t), kCountUsage, Vk::MemoryUsage::GPUOnly);
                })
                .transform([&](auto&& spcnt) { frames.secondPassCountBuffers = std::forward<decltype(spcnt)>(spcnt); });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return cullingPass.BuildHeap(ctx.Device(), cullingShader, cullingHeapBindings.GetInfo(), cullingHeapBindings.indexPushOffset, pipelineCache.Get());
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            const auto&    physInfo     = ctx.PhysicalInfo();
            const uint32_t candFamilies[3] = {physInfo.graphics_family, physInfo.compute_family, physInfo.transfer_family};
            uint32_t       uniqFamilies[3];
            uint32_t       uniqCount = 0;
            for (uint32_t cand: candFamilies) {
                bool seen = false;
                for (uint32_t j = 0; j < uniqCount; ++j) {
                    if (uniqFamilies[j] == cand) {
                        seen = true;
                        break;
                    }
                }
                if (!seen) {
                    uniqFamilies[uniqCount++] = cand;
                }
            }
            const VkSharingMode clusterSharing = (uniqCount > 1) ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
            std::span<const uint32_t> clusterFamilySpan {uniqFamilies, uniqCount};

            auto bounds = Vk::Buffer::Create(
                allocator.Get(), sizeof(ClusterBounds) * numClusters,
                Vk::BufferUsage::Storage | Vk::BufferUsage::TransferDst | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly, 0,
                clusterSharing, clusterFamilySpan
            );
            if (!bounds) {
                return std::unexpected(bounds.error());
            }
            clusterBoundsBuffer = std::move(*bounds);

            if (!clusterCullingDescLayout.Build(ctx.Device(), clusterCullingShader, VK_SHADER_STAGE_COMPUTE_BIT)) {
                return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
            }
            if (auto built = Vk::BuildHeapPassBindings(
                    heapManager, clusterCullingDescLayout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame,
                    clusterCullingHeapBindings
                );
                !built) {
                return std::unexpected(built.error());
            }

            constexpr Vk::BufferUsage kClusterGridUsage   = Vk::BufferUsage::Storage | Vk::BufferUsage::TransferDst |
                                                               Vk::BufferUsage::ShaderDeviceAddress;
            constexpr Vk::BufferUsage kLightIndexUsage    = Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress;
            constexpr Vk::BufferUsage kGlobalCounterUsage = Vk::BufferUsage::Storage | Vk::BufferUsage::TransferDst |
                                                               Vk::BufferUsage::ShaderDeviceAddress;


            auto createClusterPerFrame = [&](size_t size, Vk::BufferUsage usage) {
                return CreatePerFrame(allocator, size, usage, Vk::MemoryUsage::GPUOnly, VkDeviceSize {0}, clusterSharing, clusterFamilySpan);
            };

            return createClusterPerFrame(sizeof(ClusterVolume) * numClusters, kClusterGridUsage)
                .and_then([&](auto&& cgb) {
                    frames.clusterGridBuffers = std::forward<decltype(cgb)>(cgb);
                    return createClusterPerFrame(sizeof(uint32_t) * numClusters * 64, kLightIndexUsage);
                })
                .and_then([&](auto&& lsb) {
                    frames.lightIndexListBuffers = std::forward<decltype(lsb)>(lsb);
                    return createClusterPerFrame(sizeof(uint32_t), kGlobalCounterUsage);
                })
                .transform([&](auto&& gcb) {
                    frames.globalCounterBuffers = std::forward<decltype(gcb)>(gcb);
                    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
                        Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) -> void {
                            Vk::FillBuffer(cmd, frames.clusterGridBuffers[i], 0, 0u);
                            Vk::FillBuffer(cmd, frames.globalCounterBuffers[i], 0, 0u);
                        });
                    }
                });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            auto bDesc = Vk::CreateShaderDesc<Shaders::Modules::ClusterBoundsCS>();
            if (!clusterBoundsDescLayout.Build(ctx.Device(), bDesc, VK_SHADER_STAGE_COMPUTE_BIT)) {
                return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
            }
            if (auto built = Vk::BuildHeapPassBindings(
                    heapManager, clusterBoundsDescLayout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame,
                    clusterBoundsHeapBindings
                );
                !built) {
                return std::unexpected(built.error());
            }
            return clusterBoundsPass.BuildHeap(
                ctx.Device(), bDesc, clusterBoundsHeapBindings.GetInfo(), clusterBoundsHeapBindings.indexPushOffset, pipelineCache.Get()
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return clusterCullingPass.BuildHeap(
                ctx.Device(), clusterCullingShader, clusterCullingHeapBindings.GetInfo(), clusterCullingHeapBindings.indexPushOffset,
                pipelineCache.Get()
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (!ctx.RayTracingSupported()) {
                return {};
            }
            ZHLN_AccelerationStructureSizes tlasSizes;
            Vk::GetTLASSizes(ctx.Device(), kGpuCullingMaxInstances, tlasSizes);

            return CreatePerFrame(
                       allocator, tlasSizes.acceleration_structure_size,
                       Vk::BufferUsage::AccelerationStructureStorage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly
            )
                .and_then([&](auto&& tb) {
                    frames.tlasBuffer = std::forward<decltype(tb)>(tb);
                    return CreatePerFrame(
                        allocator, tlasSizes.build_scratch_size, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
                        Vk::MemoryUsage::GPUOnly
                    );
                })
                .and_then([&](auto&& tsb) {
                    frames.tlasScratchBuffer = std::forward<decltype(tsb)>(tsb);
                    return CreatePerFrame(
                        allocator, sizeof(VkAccelerationStructureInstanceKHR) * kGpuCullingMaxInstances,
                        Vk::BufferUsage::ShaderDeviceAddress | Vk::BufferUsage::AccelerationStructureBuildInput,
                        Vk::MemoryUsage::CPUToGPU
                    );
                })
                .and_then([&](auto&& tib) -> std::expected<void, ErrorCode> {
                    frames.tlasInstanceBuffers = std::forward<decltype(tib)>(tib);
                    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
                        frames.tlas[i] = Vk::AccelerationStructure(
                            ctx.Device(),
                            Vk::CreateAccelerationStructure(
                                ctx.Device(), frames.tlasBuffer[i].Handle(), tlasSizes.acceleration_structure_size, ZHLN_AS_TYPE_TOP_LEVEL
                            )
                        );
                        if (!frames.tlas[i].Valid()) return std::unexpected(Vk::VulkanCallError::VulkanCallFailed);
                    }
                    return {};
                });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> { return BuildSkinningPipeline(); })
        .transform([&]() -> void {
            if constexpr (isDev) {
                shaderReloads.Register("Skinning", {Shaders::Modules::SkinningCS::Path}, [this]() -> void {
                    auto res = BuildSkinningPipeline();
                    if (!res) {
                        ZHLN::Log("ERROR: Failed to hot-reload Skinning pipeline: {}", res.error());
                    } else {
                        ZHLN::Log("[Shader Reload] Skinning pipeline hot-reloaded successfully.");
                    }
                });
            }
        });
}

}
