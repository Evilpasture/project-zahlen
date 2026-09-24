// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/init/RenderInitScenePipelines.cpp
#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cstring>
#include <vector>

namespace ZHLN {

auto RenderContext::Impl::BuildParticlePipelines() -> std::expected<void, ErrorCode> {


    // 1. Allocate global default particle buffer to prevent null vkGetBufferDeviceAddress crashes
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

    // 2. Build GPU Compute Simulation Pipeline (particle_update.hlsl)
    //    VK_EXT_descriptor_heap: `scene.frame` reads via the PUSH_ADDRESS
    //    mapping; per-dispatch data travels through vkCmdPushDataEXT.
    auto csShader = Vk::CreateShaderDesc<Shaders::Modules::ParticleUpdateCS>();

    if (auto built = particleUpdatePass.BuildHeap(ctx.Device(), csShader, &sceneHeapMappings.info, 0, pipelineCache.Get()); !built) {
        return std::unexpected(built.error());
    }

    // 3. Build Billboard Graphics Pipeline (particle_render.hlsl)
    particleRenderLayout = emptyPipelineLayout;

    return LoadAndCreateShaders(
               MakeStageSource<ShaderStage::Vertex, Shaders::Modules::ParticleRenderVS>(),
               MakeStageSource<ShaderStage::Fragment, Shaders::Modules::ParticleRenderPS>()
    )
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT}) // <-- FIXED: Changed from R16G16B16_SFLOAT
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


    // 1. Compute Simulation Pipeline (mesh_particle_update.hlsl)
    //    VK_EXT_descriptor_heap: heap mappings + vkCmdPushDataEXT replace the
    //    descriptor set + push constant range this pipeline used to declare.
    auto csMeshShader = Vk::CreateShaderDesc<Shaders::Modules::MeshParticleUpdateCS>();

    if (!meshParticleUpdatePass.BuildHeap(ctx.Device(), csMeshShader, &sceneHeapMappings.info, 0, pipelineCache.Get())) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    // 2. All 3D mesh particle graphics pipelines are descriptor-heap pipelines
    //    sharing the empty layout + the scene registry mappings.
    meshParticleRenderLayout = emptyPipelineLayout;

    // 3. G-Buffer Deferred Graphics Pipeline (mesh_particle_render.hlsl)

    return LoadAndCreateShaders(
               MakeStageSource<ShaderStage::Vertex, Shaders::Modules::MeshParticleRenderVS>(),
               MakeStageSource<ShaderStage::Fragment, Shaders::Modules::MeshParticleRenderPS>()
    )
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array) // Writes to SceneColor, Velocity, NormRough
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true) // Solid 3D geometry writes depth
                .CullBack()
                .Cache(pipelineCache.Get())
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { meshParticleRenderPipeline = std::forward<decltype(pipeline)>(pipeline); });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            // 4. Directional Shadow Cascade Pipeline (mesh_particle_shadow.hlsl)

            return LoadAndCreateShaders(
                       MakeStageSource<ShaderStage::Vertex, Shaders::Modules::MeshParticleShadowVS>(),
                       MakeStageSource<ShaderStage::Fragment, Shaders::Modules::MeshParticleShadowPS>()
            )
                .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
                    return Vk::PipelineBuilder<0, true> {}
                        .Shaders(shaders)
                        .Layout(emptyPipelineLayout)
                        .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                        .DepthOnly()
                        .DepthFormat(VK_FORMAT_D32_SFLOAT)
                        .ViewMask(Passes::ShadowPass::kCascadeViewMask) // Drawn inside the multiview cascade pass.
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
    DoubleBuffered<Vk::Buffer>&      bufs,
    DoubleBuffered<VkDeviceAddress>& addrs,
    const char*                      label,
    Vk::BufferUsage                  extraFlags
) noexcept -> std::expected<void, ErrorCode> {
    const size_t bufferSize = maxVertices * (sizeof(VertexPosition) + sizeof(VertexAttributes));

    for (int i = 0; i < 2; ++i) {
        auto res = Vk::Buffer::Create(
            allocator.Get(), bufferSize, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress | extraFlags,
            Vk::MemoryUsage::CPUToGPU
        );
        if (!res) {
            return std::unexpected(res.error());
        }
        bufs[i]  = std::move(*res);
        addrs[i] = ctx.BufferAddress(bufs[i].Handle());
    }
    ZHLN::Log("Allocated double-buffered dynamic {} VBOs ({} bytes).", label, bufferSize);
    return {};
}

auto RenderContext::Impl::InitLineBuffers() noexcept -> std::expected<void, ErrorCode> {
    return AllocateDynamicVertexBuffers(kMaxLineVertices, frames.lineVbos, frames.lineVboAddresses, "line", Vk::BufferUsage::Vertex);
}

auto RenderContext::Impl::BuildLinePipeline() -> std::expected<void, ErrorCode> {
    linePipelineLayout = emptyPipelineLayout;

    // The debug line pipeline rasterises through PSForward, so it needs the
    // Forward geometry variant (the G-buffer one emits motion vectors and a
    // normal frame that PSForward does not read).


    return LoadAndCreateShaders(
               MakeStageSource<ShaderStage::Vertex, Shaders::Modules::BasicVSForward>(),
               MakeStageSource<ShaderStage::Fragment, Shaders::Modules::ForwardPS>()
    )
        .and_then([&](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder<1, true> {}
                .Shaders(shaders)
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

        // 1. Bind Sampler
        .and_then([&](auto&& sampler) -> std::expected<void, ErrorCode> {
            shadowSampler     = std::forward<decltype(sampler)>(sampler);
            shadowSamplerInfo = shadowSamplerBuilder.Info();
            return {};
        })

        // 2. Cascade shadow map pair, per-cascade views, the punctual atlas and
        //    its two views. TargetManager owns the shadow geometry, so the
        //    resolution, cascade count and atlas layering are its constants and
        //    not this function's concern.
        .and_then([&]() -> std::expected<void, ErrorCode> { return targets.InitShadows(); })

        // 3. Allocate Double-Buffered Frame Uniform Buffers
        //    VK_EXT_descriptor_heap: their device addresses feed the scene
        //    registry's PUSH_ADDRESS mappings, so they need
        //    Vk::BufferUsage::ShaderDeviceAddress.
        .and_then([&]() -> auto {
            return CreateDoubleBuffered(
                       allocator, sizeof(FrameUniforms), Vk::BufferUsage::Uniform | Vk::BufferUsage::ShaderDeviceAddress,
                       Vk::MemoryUsage::CPUToGPU
            )
                .transform_error([](auto err) -> ErrorCode { return err; });
        })

        // 4. Allocate Double-Buffered Light Storage Buffers (same SDA requirement)
        .and_then([&](auto&& fub) -> auto {
            frames.frameUniformBuffers = std::forward<decltype(fub)>(fub);
            return CreateDoubleBuffered(
                       allocator, sizeof(Light) * 128, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
                       Vk::MemoryUsage::CPUToGPU
            )
                .transform_error([](auto err) -> ErrorCode { return err; });
        })

        // 5. Allocate Double-Buffered Indirect Argument Buffers
        .and_then([&](auto&& lsb) -> auto {
            frames.lightStorageBuffers = std::forward<decltype(lsb)>(lsb);
            return CreateDoubleBuffered(
                       allocator, sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances * 8, Vk::BufferUsage::Indirect, Vk::MemoryUsage::CPUToGPU
            )
                .transform_error([](auto err) -> ErrorCode { return err; });
        })

        // 6. Complete pipeline assignment
        .transform([&](auto&& sib) -> auto { frames.shadowIndirectBuffers = std::forward<decltype(sib)>(sib); });
}

auto RenderContext::Impl::BuildDecalPipeline() -> std::expected<void, ErrorCode> {


    static constexpr std::array<VkFormat, 2> decalFormats = {VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R8G8B8A8_UNORM};



    // Reflects decal.slang set 0 ({texDepth, pointSampler}) and set 1 (the scene
    // parameter block subset). VK_EXT_descriptor_heap: the reflection feeds the
    // mapping tables (decalHeapMappings + decalSceneHeapMappings) that remap
    // both sets onto the heaps at pipeline creation; no descriptor sets exist.
    const Vk::ReflectedStageInput reflectInputs[2] = {
        {.shader = Vk::CreateShaderDesc<Shaders::Modules::DecalVS>(), .stage = VK_SHADER_STAGE_VERTEX_BIT},
        {.shader = Vk::CreateShaderDesc<Shaders::Modules::DecalPS>(), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    if (!decalDescLayout.Build(ctx.Device(), std::span {reflectInputs})) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    BuildDecalHeapMappings();
    decalPipelineLayout = emptyPipelineLayout;

    // Merge decal set 0 + scene set 1 into one mapping chain per stage.
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
            return Vk::PipelineBuilder<2, true> {} // Updated from 3 to 2 attachments
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&mergedInfo, &mergedInfo)
                .ColorFormats(decalFormats) // Explicit 2-format array
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


    // We declare the shared shaders in a stack variable so all lambdas can reference it.
    Vk::ShaderStages shaders;



    return LoadAndCreateShaders(
               MakeStageSource<ShaderStage::Vertex, Shaders::Modules::BasicVS>(),
               MakeStageSource<ShaderStage::Fragment, Shaders::Modules::BasicPS>()
    )
        .and_then([&](auto&& compiledShaders) -> auto {
            shaders = std::forward<decltype(compiledShaders)>(compiledShaders);

            csgPipelineLayout = emptyPipelineLayout;

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .CullNone()
                // CSG Write: stamp reference 1 into the stencil wherever the
                // volume passes, and colour nothing while doing it.
                .ColorWriteEnable(false)
                .StencilWriteMask(1)
                .Cache(pipelineCache.Get())
                .Build(ctx.Device())
                .transform_error([](auto e) -> ErrorCode { return e; });
        })
        .and_then([&](auto&& writePipeline) -> auto {
            csgWritePipeline = std::forward<decltype(writePipeline)>(writePipeline);

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                // CSG Difference: the target draws only where the cutters did
                // not stamp reference 1, so their volume is subtracted from it.
                .StencilCompareMask(VK_COMPARE_OP_NOT_EQUAL, 1)
                .Cache(pipelineCache.Get())
                .Build(ctx.Device())
                .transform_error([](auto e) -> ErrorCode { return e; });
        })
        .and_then([&](auto&& diffPipeline) -> auto {
            csgDifferencePipeline = std::forward<decltype(diffPipeline)>(diffPipeline);

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                // CSG Intersection: the target draws only where the cutters did
                // stamp reference 1, so only the overlap survives.
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
    // Optional: hang_gpu.slang writes through a null page to force TDR.
    // Pipeline creation must not take down engine init; ProvokeDeviceLost
    // then no-ops.
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

    // VK_EXT_descriptor_heap: the HiZ map does not exist yet at pipeline-build
    // time (it is created on the first RecreateTargets) and the pass writes one
    // block per mip as it records them, so the binding table needs no count.
    if (auto built = Vk::BuildHeapPassBindings(
            heapManager, hizDescLayout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, hizHeapBindings
        );
        !built) {
        return std::unexpected(built.error());
    }

    return hizGeneratePass.BuildHeap(ctx.Device(), shader, hizHeapBindings.GetInfo(), hizHeapBindings.indexPushOffset, pipelineCache.Get());
}

auto RenderContext::Impl::CompileShadowPipeline(VkDevice device, const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag) -> std::expected<void, ErrorCode> {
    // VK_EXT_descriptor_heap: the shadow pass reads the scene registry through
    // the heap; the per-draw push block travels via vkCmdPushDataEXT.
    shadowPipelineLayout = emptyPipelineLayout;
    return Vk::ShaderStages::Create(device, vert, frag)
        .transform_error([](auto err) -> ErrorCode { return err; })
        .and_then([&, device](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .DepthOnly()
                .DepthFormat(VK_FORMAT_D32_SFLOAT)
                // Multiview cascades: matches the single layered shadow render
                // pass (viewMask 0x0F); ViewIndex drives the light matrix.
                .ViewMask(Passes::ShadowPass::kCascadeViewMask)
                .CullNone()
                .Cache(pipelineCache.Get())
                .Build(device)
                .transform_error([](auto) -> ErrorCode { return Vk::PipelineBuilderError::PipelineCreationFailed; })
                .transform([&](auto&& pipeline) -> auto { shadowPipeline = std::forward<decltype(pipeline)>(pipeline); });
        })
        .and_then([&, device]() -> std::expected<void, ErrorCode> {
            // VK_EXT_mesh_shader twin of the shadow pipeline. Optional by
            // design: a failure here only means the cascades keep using the
            // indirect vertex draws, so it never fails pipeline compilation.
            // The twin renders inside the multiview cascade pass and its mesh
            // stage reads SV_ViewID, which requires multiviewMeshShader --
            // skip creation entirely when that feature is unavailable.
            const bool multiviewMesh = ctx.HasFeature<VkPhysicalDeviceMeshShaderFeaturesEXT>([](const VkPhysicalDeviceMeshShaderFeaturesEXT& f) -> bool {
                return f.multiviewMeshShader == VK_TRUE;
            });
            if (!ctx.MeshShadersSupported() || !multiviewMesh) {
                return {};
            }

            // Shadow variant: its varying set must match ShadowPS exactly, so the
            // three modules are named together in one call.
            auto shaders = Vk::ShaderStages::CreateMesh<Shaders::Modules::BasicTask, Shaders::Modules::BasicMeshShadow, Shaders::Modules::ShadowPS>(device);
            if (!shaders) {
                ZHLN::Log("[RenderResources] Shadow mesh-stage creation failed; cascades keep the vertex pipeline.");
                return {};
            }

            auto pipeline = Vk::PipelineBuilder {}
                                .Shaders(*shaders)
                                .Layout(emptyPipelineLayout)
                                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                                .DepthOnly()
                                .DepthFormat(VK_FORMAT_D32_SFLOAT)
                                .ViewMask(Passes::ShadowPass::kCascadeViewMask) // Multiview cascades (must match the render pass).
                                .CullNone()
                                .Cache(pipelineCache.Get())
                                .Build(device);
            if (!pipeline) {
                ZHLN::Log("[RenderResources] Shadow mesh pipeline creation failed; cascades keep the vertex pipeline.");
                return {};
            }
            shadowMeshPipeline = std::move(*pipeline);
            return {};
        });
}

auto RenderContext::Impl::CompilePunctualShadowPipeline(VkDevice device, const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag) -> std::expected<void, ErrorCode> {
    // VK_EXT_descriptor_heap variant of the shadow path (same mappings, the
    // per-draw light index travels through push data).
    punctualShadowPipelineLayout = emptyPipelineLayout;
    return Vk::ShaderStages::Create(device, vert, frag)
        .transform_error([](auto err) -> ErrorCode { return err; })
        .and_then([&, device](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .DepthOnly()
                .DepthFormat(VK_FORMAT_D32_SFLOAT)
                .ViewMask(0x3F)
                .CullNone()
                .Cache(pipelineCache.Get())
                .Build(device)
                .transform_error([](auto) -> ErrorCode { return Vk::PipelineBuilderError::PipelineCreationFailed; })
                .transform([&](auto&& pipeline) -> auto { punctualShadowPipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
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
            return CreateDoubleBuffered(allocator, sizeof(InstanceData) * kGpuCullingMaxInstances, kInstanceUsage, Vk::MemoryUsage::CPUToGPU)
                .and_then([&](auto&& idb) {
                    frames.instanceDataBuffers = std::forward<decltype(idb)>(idb);
                    return CreateDoubleBuffered(allocator, sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances, kIndirectUsage, Vk::MemoryUsage::GPUOnly);
                })
                .and_then([&](auto&& icb1) {
                    frames.indirectCommandsBuffers = std::forward<decltype(icb1)>(icb1);
                    return CreateDoubleBuffered(allocator, sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances, kIndirectUsage, Vk::MemoryUsage::GPUOnly);
                })
                .and_then([&](auto&& icb2) {
                    frames.indirectCommandsBuffersPass2 = std::forward<decltype(icb2)>(icb2);
                    return CreateDoubleBuffered(allocator, sizeof(uint32_t) * kGpuCullingMaxInstances, kCandidateUsage, Vk::MemoryUsage::GPUOnly);
                })
                .and_then([&](auto&& spcb) {
                    frames.secondPassCandidatesBuffers = std::forward<decltype(spcb)>(spcb);
                    return CreateDoubleBuffered(allocator, sizeof(uint32_t), kCountUsage, Vk::MemoryUsage::GPUOnly);
                })
                .transform([&](auto&& spcnt) { frames.secondPassCountBuffers = std::forward<decltype(spcnt)>(spcnt); });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return cullingPass.BuildHeap(ctx.Device(), cullingShader, cullingHeapBindings.GetInfo(), cullingHeapBindings.indexPushOffset, pipelineCache.Get());
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            auto bounds = Vk::Buffer::Create(
                allocator.Get(), sizeof(ClusterBounds) * numClusters,
                Vk::BufferUsage::Storage | Vk::BufferUsage::TransferDst | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly
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

            return CreateDoubleBuffered(allocator, sizeof(ClusterVolume) * numClusters, kClusterGridUsage, Vk::MemoryUsage::GPUOnly)
                .and_then([&](auto&& cgb) {
                    frames.clusterGridBuffers = std::forward<decltype(cgb)>(cgb);
                    return CreateDoubleBuffered(allocator, sizeof(uint32_t) * numClusters * 64, kLightIndexUsage, Vk::MemoryUsage::GPUOnly);
                })
                .and_then([&](auto&& lsb) {
                    frames.lightIndexListBuffers = std::forward<decltype(lsb)>(lsb);
                    return CreateDoubleBuffered(allocator, sizeof(uint32_t), kGlobalCounterUsage, Vk::MemoryUsage::GPUOnly);
                })
                .transform([&](auto&& gcb) {
                    frames.globalCounterBuffers = std::forward<decltype(gcb)>(gcb);
                    for (uint32_t i = 0; i < 2; ++i) {
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

            return CreateDoubleBuffered(
                       allocator, tlasSizes.acceleration_structure_size,
                       Vk::BufferUsage::AccelerationStructureStorage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly
            )
                .and_then([&](auto&& tb) {
                    frames.tlasBuffer = std::forward<decltype(tb)>(tb);
                    return CreateDoubleBuffered(
                        allocator, tlasSizes.build_scratch_size, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
                        Vk::MemoryUsage::GPUOnly
                    );
                })
                .and_then([&](auto&& tsb) {
                    frames.tlasScratchBuffer = std::forward<decltype(tsb)>(tsb);
                    // Host-visible instance storage (CPU_TO_GPU, coherent):
                    // BuildTLAS memcpys the scratch straight into the mapped
                    // buffer, dropping the staging buffer, the per-frame
                    // transfer copy, and its barrier. Same pattern as the
                    // instance-data buffers: CPU_TO_GPU + device address +
                    // double-buffered.
                    return CreateDoubleBuffered(
                        allocator, sizeof(VkAccelerationStructureInstanceKHR) * kGpuCullingMaxInstances,
                        Vk::BufferUsage::ShaderDeviceAddress | Vk::BufferUsage::AccelerationStructureBuildInput,
                        Vk::MemoryUsage::CPUToGPU
                    );
                })
                .transform([&](auto&& tib) {
                    frames.tlasInstanceBuffers = std::forward<decltype(tib)>(tib);
                    for (uint32_t i = 0; i < 2; ++i) {
                        frames.tlas[i] = Vk::CreateAccelerationStructure(
                            ctx.Device(), frames.tlasBuffer[i].Handle(), tlasSizes.acceleration_structure_size, ZHLN_AS_TYPE_TOP_LEVEL
                        );
                    }
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

} // namespace ZHLN
