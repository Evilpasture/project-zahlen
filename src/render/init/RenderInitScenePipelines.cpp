// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cstring>
#include <format>
#include <span>
#include <vector>

namespace ZHLN {

auto RenderContext::Impl::BuildEmissivePipeline() -> std::expected<void, ErrorCode> {
    Vk::HeapMappingArena mappingArena {heapManager};
    const auto*           sceneMappings = mappingArena.Map(sceneHeapMappings);
    return LoadAndCreateShaders(Vk::MakeStageSource<Shaders::Modules::BasicVSEmissive>(), Vk::MakeStageSource<Shaders::Modules::EmissivePS>())
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders.View())
                .HeapMappings(sceneMappings, sceneMappings)
                .ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .DepthCompare(VK_COMPARE_OP_LESS_OR_EQUAL)
                .DynamicCullMode()
                .AdditiveBlend()
                // LightingGI.a is ambient occlusion, not emissive opacity.
                // PSEmissive outputs alpha=0; writing it here erased AO for
                // every emissive mesh pixel, zeroing the following IBL pass.
                .ColorWriteMask(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT)
                .Binaries(&pipelineBinaries)
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { forward.emissive = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::BuildParticlePipelines() -> std::expected<void, ErrorCode> {
    auto csShader = Vk::CreateShaderDesc<Shaders::Modules::ParticleUpdateCS>();

    Vk::HeapMappingArena mappingArena {heapManager};
    const auto*           sceneMappings = mappingArena.Map(sceneHeapMappings);

    if (auto built = particleUpdate.particles.BuildHeap(ctx.Device(), csShader, sceneMappings, 0, &pipelineBinaries); !built) {
        return std::unexpected(built.error());
    }

    return LoadAndCreateShaders(Vk::MakeStageSource<Shaders::Modules::ParticleRenderVS>(), Vk::MakeStageSource<Shaders::Modules::ParticleRenderPS>())
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders.View())
                .HeapMappings(sceneMappings, sceneMappings)
                .ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .AdditiveBlend()
                .AlphaBlend()
                .CullNone()
                .Binaries(&pipelineBinaries)
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { forward.particles = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::BuildMeshParticlePipelines() -> std::expected<void, ErrorCode> {
    auto csMeshShader = Vk::CreateShaderDesc<Shaders::Modules::MeshParticleUpdateCS>();

    Vk::HeapMappingArena mappingArena {heapManager};
    const auto*           sceneMappings = mappingArena.Map(sceneHeapMappings);

    if (!particleUpdate.mesh.BuildHeap(ctx.Device(), csMeshShader, sceneMappings, 0, &pipelineBinaries)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    return LoadAndCreateShaders(Vk::MakeStageSource<Shaders::Modules::MeshParticleRenderVS>(), Vk::MakeStageSource<Shaders::Modules::MeshParticleRenderPS>())
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders.View())
                .HeapMappings(sceneMappings, sceneMappings)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                .Binaries(&pipelineBinaries)
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { meshParticles.render = std::forward<decltype(pipeline)>(pipeline); });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return LoadAndCreateShaders(
                       Vk::MakeStageSource<Shaders::Modules::MeshParticleShadowVS>(), Vk::MakeStageSource<Shaders::Modules::MeshParticleShadowPS>()
            )
                .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
                    return Vk::PipelineBuilder<0, true> {}
                        .Shaders(shaders.View())
                        .HeapMappings(sceneMappings, sceneMappings)
                        .DepthOnly()
                        .DepthFormat(VK_FORMAT_D32_SFLOAT)
                        .ViewMask(ShadowRenderer::kCascadeViewMask)
                        .CullNone()
                        .Binaries(&pipelineBinaries)
                        .Build(ctx.Device())
                        .transform([&](auto&& pipeline) -> auto { meshParticles.shadow = std::forward<decltype(pipeline)>(pipeline); });
                });
        });
}

auto RenderContext::Impl::BuildSkinningPipeline() -> std::expected<void, ErrorCode> {
    const auto source = Vk::MakeStageSource<Shaders::Modules::SkinningCS>();
    const auto loaded = LoadShaderData(source);
    const auto bytes  = loaded.Code();
    if (bytes.empty()) {
        return std::unexpected(Vk::ShaderStageCreationError::ShaderLoadingFailed);
    }

    const Vk::ShaderDesc shader = Vk::CreateShaderDesc(bytes, source.entryPoint);
    return compute.skinning.BuildHeap(ctx.Device(), shader, nullptr, 0, &pipelineBinaries);
}

auto RenderContext::Impl::AllocateLineVertexBuffers(size_t maxVertices, PerFrame<Vk::Buffer>& bufs, PerFrame<VkDeviceAddress>& addrs) noexcept
    -> std::expected<void, ErrorCode> {
    const size_t              bufferSize = maxVertices * sizeof(LineVertex);
    PerFrame<Vk::Buffer>      created;
    PerFrame<VkDeviceAddress> createdAddresses;
    defer                     _([&] {
        for (auto& buffer: created) {
            allocator.DestroyBuffer(buffer);
        }
    });
    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
        auto res = Vk::Buffer::Create(allocator, bufferSize, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::CPUToGPU);
        if (!res) {
            return std::unexpected(res.error());
        }
        created[i]          = std::move(*res);
        createdAddresses[i] = ctx.BufferAddress(created[i].Handle());
    }

    for (auto& buffer: bufs) {
        allocator.DestroyBuffer(buffer);
    }
    bufs  = std::move(created);
    addrs = createdAddresses;
    return {};
}

auto RenderContext::Impl::InitLineBuffers() noexcept -> std::expected<void, ErrorCode> {
    return AllocateLineVertexBuffers(kMaxLineVertices, frames.lineVbos, frames.lineVboAddresses);
}

auto RenderContext::Impl::BuildLinePipeline() -> std::expected<void, ErrorCode> {
    return LoadAndCreateShaders(Vk::MakeStageSource<Shaders::Modules::DebugLineVS>(), Vk::MakeStageSource<Shaders::Modules::DebugLinePS>())
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder<1, true> {}
                .Shaders(shaders.View())
                .ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT})
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .Topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST)
                .CullNone()
                .AlphaBlend()
                .Binaries(&pipelineBinaries)
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { forward.line = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::InitShadowResources() -> std::expected<void, ErrorCode> {
    shadowSamplerConfig = Vk::SamplerConfig::LinearClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE).WithDepthCompare();

    return targets
        .InitShadows()

        .and_then([&]() -> std::expected<void, ErrorCode> { return shadows.InitResources(*this); })

        .and_then([&]() -> auto {
            return CreatePerFrame(allocator, sizeof(FrameUniforms), Vk::BufferUsage::Uniform | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::CPUToGPU)
                .transform_error([](auto err) -> ErrorCode { return err; });
        })

        .and_then([&](auto&& fub) -> auto {
            frames.frameUniformBuffers = std::forward<decltype(fub)>(fub);
            return CreatePerFrame(
                       allocator, sizeof(Light) * kMaxSceneLights, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::CPUToGPU
            )
                .transform_error([](auto err) -> ErrorCode { return err; });
        })

        .and_then([&](auto&& lsb) -> auto {
            frames.lightStorageBuffers = std::forward<decltype(lsb)>(lsb);
            return CreatePerFrame(
                       allocator, sizeof(ShadowTile) * ShadowAtlas::kMaxTiles, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
                       Vk::MemoryUsage::CPUToGPU
            )
                .transform_error([](auto err) -> ErrorCode { return err; });
        })

        .transform([&](auto&& stb) -> void { frames.shadowTileBuffers = std::forward<decltype(stb)>(stb); });
}

auto RenderContext::Impl::BuildDecalPipeline() -> std::expected<void, ErrorCode> {
    static constexpr std::array<VkFormat, 2> decalFormats = {VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R8G8B8A8_UNORM};

    const std::array reflectInputs = {
        Vk::ReflectedStageInput {.shader = Vk::CreateShaderDesc<Shaders::Modules::DecalVS>(), .stage = VK_SHADER_STAGE_VERTEX_BIT},
        Vk::ReflectedStageInput {.shader = Vk::CreateShaderDesc<Shaders::Modules::DecalPS>(), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    if (!decals.layout.Build(ctx.Device(), std::span {reflectInputs})) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    BuildDecalHeapMappings();

    // Set 0 (the decal's own depth + point sampler) followed by the shared Set-1
    // scene block, materialized into one pMappings list.
    Vk::HeapMappingArena mappingArena {heapManager};
    const auto*           decalMappings = mappingArena.Map(decalHeapMappings, sceneSet1HeapMappings);

    return LoadAndCreateShaders(Vk::MakeStageSource<Shaders::Modules::DecalVS>(), Vk::MakeStageSource<Shaders::Modules::DecalPS>())
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder<2, true> {}
                .Shaders(shaders.View())
                .HeapMappings(decalMappings, decalMappings)
                .ColorFormats(decalFormats)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .CullFront()
                .AlphaBlend()
                .Binaries(&pipelineBinaries)
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { decals.pass = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::InitCSGPipelines() -> std::expected<void, ErrorCode> {
    auto shaders = LoadAndCreateShaders(Vk::MakeStageSource<Shaders::Modules::BasicVS>(), Vk::MakeStageSource<Shaders::Modules::BasicPS>());
    if (!shaders) {
        return std::unexpected(shaders.error());
    }

    const auto stages = shaders->View();

    // Three stencil variants, one mapping table: materialize once.
    Vk::HeapMappingArena mappingArena {heapManager};
    const auto*           sceneMappings = mappingArena.Map(sceneHeapMappings);

    return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
        .Shaders(stages)
        .HeapMappings(sceneMappings, sceneMappings)
        .ColorFormats(ActiveGBuffer::array)
        .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
        .DepthTest(true)
        .DepthWrite(false)
        .CullNone()
        .ColorWriteEnable(false)
        .StencilWriteMask(1)
        .Binaries(&pipelineBinaries)
        .Build(ctx.Device())
        .and_then([&](auto&& writePipeline) -> auto {
            csg.write = std::forward<decltype(writePipeline)>(writePipeline);

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(stages)
                .HeapMappings(sceneMappings, sceneMappings)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                .StencilCompareMask(VK_COMPARE_OP_NOT_EQUAL, 1)
                .Binaries(&pipelineBinaries)
                .Build(ctx.Device())
                .transform_error([](auto e) -> ErrorCode { return e; });
        })
        .and_then([&](auto&& diffPipeline) -> auto {
            csg.difference = std::forward<decltype(diffPipeline)>(diffPipeline);

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(stages)
                .HeapMappings(sceneMappings, sceneMappings)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                .StencilCompareMask(VK_COMPARE_OP_EQUAL, 1)
                .Binaries(&pipelineBinaries)
                .Build(ctx.Device())
                .transform_error([](auto e) -> ErrorCode { return e; });
        })
        .transform([&](auto&& intersectPipeline) -> auto {
            csg.intersection = std::forward<decltype(intersectPipeline)>(intersectPipeline);

            diagnostics.shaderReloads.Register("CSGStencil", {Shaders::Modules::BasicVS::Path, Shaders::Modules::BasicPS::Path}, [this]() -> void {
                auto res = InitCSGPipelines();
                if (!res) {
                    ZHLN::LogError("Failed to hot-reload CSG stencil pipelines: {}", res.error());
                } else {
                    ZHLN::Log("[Shader Reload] CSG Stencil pipelines hot-reloaded successfully.");
                }
            });
        });
}

auto RenderContext::Impl::BuildHangGpuPipeline() -> std::expected<void, ErrorCode> {
    auto built = [&]() -> std::expected<void, ErrorCode> {
        const auto source = Vk::MakeStageSource<Shaders::Modules::HangGpuCS>();
        const auto loaded = LoadShaderData(source);
        const auto bytes  = loaded.Code();
        if (bytes.empty()) {
            return std::unexpected(Vk::ShaderStageCreationError::ShaderLoadingFailed);
        }

        const Vk::ShaderDesc shader = Vk::CreateShaderDesc(bytes, source.entryPoint);
        return compute.hangGpu.BuildHeap(ctx.Device(), shader, nullptr, 0, &pipelineBinaries);
    }();
    if (!built) {
        ZHLN::Log("[GPU] hang_gpu pipeline unavailable ({}); ProvokeDeviceLost is a no-op.", built.error());
    }
    return {};
}

auto RenderContext::Impl::BuildHiZPipeline() -> std::expected<void, ErrorCode> {
    auto shader = Vk::CreateShaderDesc<Shaders::Modules::HizGenerateCS>();
    if (!culling.hizDescLayout.Build(ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    if (auto built = Vk::BuildHeapPassBindings(
            heapManager, culling.hizDescLayout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, culling.hizHeapBindings
        );
        !built) {
        return std::unexpected(built.error());
    }

    Vk::HeapMappingArena mappingArena {heapManager};
    return culling.hizGeneratePass.BuildHeap(
        ctx.Device(), shader, mappingArena.Map(culling.hizHeapBindings.mappings), culling.hizHeapBindings.indexPushOffset, &pipelineBinaries
    );
}

auto RenderContext::Impl::InitCullingResources() -> std::expected<void, ErrorCode> {
    auto cullingShader = Vk::CreateShaderDesc<Shaders::Modules::CullingCS>();
    if (!culling.cullingLayout.Build(ctx.Device(), cullingShader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    auto clusterCullingShader = Vk::CreateShaderDesc<Shaders::Modules::ClusterCullingCS>();
    auto clusterDispatch      = Vk::ReflectComputeDispatchSize(clusterCullingShader);
    if (!clusterDispatch) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    const size_t numClusters = static_cast<size_t>((*clusterDispatch)[0]) * (*clusterDispatch)[1] * (*clusterDispatch)[2];

    if (auto built = Vk::BuildHeapPassBindings(
            heapManager, culling.cullingLayout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, culling.cullingHeapBindings
        );
        !built) {
        return std::unexpected(built.error());
    }

    constexpr Vk::BufferUsage kInstanceUsage  = Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress;
    constexpr Vk::BufferUsage kIndirectUsage  = Vk::BufferUsage::Storage | Vk::BufferUsage::Indirect | Vk::BufferUsage::TransferDst |
                                                Vk::BufferUsage::TransferSrc | Vk::BufferUsage::ShaderDeviceAddress;
    constexpr Vk::BufferUsage kCandidateUsage = Vk::BufferUsage::Storage | Vk::BufferUsage::TransferDst | Vk::BufferUsage::ShaderDeviceAddress;
    constexpr Vk::BufferUsage kCountUsage     = Vk::BufferUsage::Storage | Vk::BufferUsage::TransferDst | Vk::BufferUsage::TransferSrc |
                                                Vk::BufferUsage::ShaderDeviceAddress;

    return std::expected<void, ErrorCode> {}
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return CreatePerFrame(allocator, sizeof(InstanceData) * kGpuCullingMaxInstances, kInstanceUsage, Vk::MemoryUsage::CPUToGPU)
                .and_then([&](auto&& idb) {
                    frames.instanceDataBuffers = std::forward<decltype(idb)>(idb);
                    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
                        Vk::Debug::SetBufferName(ctx, frames.instanceDataBuffers[i].Handle(), std::format("InstanceData.Frame{}", i));
                    }
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
            Vk::HeapMappingArena mappingArena {heapManager};
            return culling.cullingPass.BuildHeap(
                ctx.Device(), cullingShader, mappingArena.Map(culling.cullingHeapBindings.mappings), culling.cullingHeapBindings.indexPushOffset,
                &pipelineBinaries
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            const auto&             physInfo     = ctx.PhysicalInfo();
            const std::array        candFamilies = {physInfo.graphicsFamily, physInfo.computeFamily, physInfo.transferFamily};
            std::array<uint32_t, 3> uniqFamilies {};
            uint32_t                uniqCount = 0;
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
            const VkSharingMode       clusterSharing = (uniqCount > 1) ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
            std::span<const uint32_t> clusterFamilySpan {uniqFamilies.data(), uniqCount};

            auto bounds = Vk::Buffer::Create(
                allocator, sizeof(ClusterBounds) * numClusters, Vk::BufferUsage::Storage | Vk::BufferUsage::TransferDst | Vk::BufferUsage::ShaderDeviceAddress,
                Vk::MemoryUsage::GPUOnly, 0, clusterSharing, clusterFamilySpan
            );
            if (!bounds) {
                return std::unexpected(bounds.error());
            }
            clusterBoundsBuffer = std::move(*bounds);

            if (!culling.clusterCullingDescLayout.Build(ctx.Device(), clusterCullingShader, VK_SHADER_STAGE_COMPUTE_BIT)) {
                return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
            }
            if (auto built = Vk::BuildHeapPassBindings(
                    heapManager, culling.clusterCullingDescLayout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame,
                    culling.clusterCullingHeapBindings
                );
                !built) {
                return std::unexpected(built.error());
            }

            constexpr Vk::BufferUsage kClusterGridUsage   = Vk::BufferUsage::Storage | Vk::BufferUsage::TransferDst | Vk::BufferUsage::ShaderDeviceAddress;
            constexpr Vk::BufferUsage kLightIndexUsage    = Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress;
            constexpr Vk::BufferUsage kGlobalCounterUsage = Vk::BufferUsage::Storage | Vk::BufferUsage::TransferDst | Vk::BufferUsage::ShaderDeviceAddress;

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
                .and_then([&](auto&& gcb) -> std::expected<void, ErrorCode> {
                    frames.globalCounterBuffers = std::forward<decltype(gcb)>(gcb);
                    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
                        if (auto immediate = Vk::ExecuteImmediate(
                                ctx, graphicsCmdRing,
                                [&](VkCommandBuffer cmd) -> void {
                                    Vk::FillBuffer(cmd, frames.clusterGridBuffers[i], 0, 0u);
                                    Vk::FillBuffer(cmd, frames.globalCounterBuffers[i], 0, 0u);
                                }
                            );
                            !immediate) [[unlikely]] {
                            return std::unexpected(immediate.error());
                        }
                    }
                    return {};
                });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            auto bDesc = Vk::CreateShaderDesc<Shaders::Modules::ClusterBoundsCS>();
            if (!culling.clusterBoundsDescLayout.Build(ctx.Device(), bDesc, VK_SHADER_STAGE_COMPUTE_BIT)) {
                return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
            }
            if (auto built = Vk::BuildHeapPassBindings(
                    heapManager, culling.clusterBoundsDescLayout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame,
                    culling.clusterBoundsHeapBindings
                );
                !built) {
                return std::unexpected(built.error());
            }
            Vk::HeapMappingArena mappingArena {heapManager};
            return culling.clusterBoundsPass.BuildHeap(
                ctx.Device(), bDesc, mappingArena.Map(culling.clusterBoundsHeapBindings.mappings), culling.clusterBoundsHeapBindings.indexPushOffset,
                &pipelineBinaries
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            Vk::HeapMappingArena mappingArena {heapManager};
            return culling.clusterCullingPass.BuildHeap(
                ctx.Device(), clusterCullingShader, mappingArena.Map(culling.clusterCullingHeapBindings.mappings),
                culling.clusterCullingHeapBindings.indexPushOffset, &pipelineBinaries
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (!ctx.RayTracingSupported()) {
                return {};
            }
            const Vk::AccelerationStructureSizes tlasSizes = Vk::GetTLASSizes(ctx.Device(), kGpuCullingMaxInstances);

            return CreatePerFrame(
                       allocator, tlasSizes.acceleration_structure_size, Vk::BufferUsage::AccelerationStructureStorage | Vk::BufferUsage::ShaderDeviceAddress,
                       Vk::MemoryUsage::GPUOnly
            )
                .and_then([&](auto&& tb) {
                    frames.tlasBuffer = std::forward<decltype(tb)>(tb);
                    return CreatePerFrame(
                        allocator, tlasSizes.build_scratch_size, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly
                    );
                })
                .and_then([&](auto&& tsb) {
                    frames.tlasScratchBuffer = std::forward<decltype(tsb)>(tsb);
                    return CreatePerFrame(
                        allocator, sizeof(VkAccelerationStructureInstanceKHR) * kGpuCullingMaxInstances,
                        Vk::BufferUsage::ShaderDeviceAddress | Vk::BufferUsage::AccelerationStructureBuildInput, Vk::MemoryUsage::CPUToGPU
                    );
                })
                .and_then([&](auto&& tib) -> std::expected<void, ErrorCode> {
                    frames.tlasInstanceBuffers = std::forward<decltype(tib)>(tib);
                    for (uint32_t i = 0; i < Vk::kFramesInFlight; ++i) {
                        auto tlas = Vk::CreateAccelerationStructure(
                            ctx.Device(), frames.tlasBuffer[i].Handle(), tlasSizes.acceleration_structure_size, Vk::AccelerationStructureType::TopLevel
                        );
                        if (!tlas) {
                            return std::unexpected(tlas.error());
                        }
                        frames.tlas[i] = std::move(*tlas);
                        if (!frames.tlas[i].Valid()) {
                            return std::unexpected(Vk::VulkanCallError::VulkanCallFailed);
                        }
                        frames.tlasAddresses[i] = Vk::DeviceAddress<> {Vk::GetAccelerationStructureAddress(ctx.Device(), frames.tlas[i].Get())};
                    }
                    return {};
                });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> { return BuildSkinningPipeline(); })
        .transform([&]() -> void {
            if constexpr (isDev) {
                diagnostics.shaderReloads.Register("Skinning", {Shaders::Modules::SkinningCS::Path}, [this]() -> void {
                    auto res = BuildSkinningPipeline();
                    if (!res) {
                        ZHLN::LogError("Failed to hot-reload Skinning pipeline: {}", res.error());
                    } else {
                        ZHLN::Log("[Shader Reload] Skinning pipeline hot-reloaded successfully.");
                    }
                });
            }
        });
}

} // namespace ZHLN
