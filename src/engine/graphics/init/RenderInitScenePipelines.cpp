// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/graphics/init/RenderInitScenePipelines.cpp
#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cstring>
#include <vector>

namespace ZHLN {

auto RenderContext::Impl::BuildParticlePipelines() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    // 1. Allocate global default particle buffer to prevent null vkGetBufferDeviceAddress crashes
    size_t particleBufferSize = RenderContext::Impl::kGpuParticleCount * sizeof(Particle);
    auto   pb_res             = Vk::Buffer::Create(
        allocator.Get(), particleBufferSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY
    );
    if (!pb_res) {
        return std::unexpected(pb_res.error());
    }
    particleBuffer = std::move(*pb_res);

    // 2. Build GPU Compute Simulation Pipeline (particle_update.hlsl)
    //    VK_EXT_descriptor_heap: `scene.frame` reads via the PUSH_ADDRESS
    //    mapping; per-dispatch data travels through vkCmdPushDataEXT.
    auto csShader = Vk::CreateShaderDesc(Resource::GetShaderProgram(ParticleUpdate).vertex);

    if (!particleUpdatePass.BuildHeap(ctx.Device(), csShader, &sceneHeapMappings.info)) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    // 3. Build Billboard Graphics Pipeline (particle_render.hlsl)
    particleRenderLayout = emptyPipelineLayout;
    auto renderShaders   = Resource::GetShaderProgram(ParticleRender);
    return LoadAndCreateShaders(
               {.path = Resource::Paths::ParticleRenderVS, .fallback = renderShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::ParticleRenderPS, .fallback = renderShaders.fragment, .entryPoint = "PSMain"}
    )
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
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
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { particleRenderPipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::BuildMeshParticlePipelines() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    // 1. Compute Simulation Pipeline (mesh_particle_update.hlsl)
    //    VK_EXT_descriptor_heap: heap mappings + vkCmdPushDataEXT replace the
    //    descriptor set + push constant range this pipeline used to declare.
    auto csMeshShader = Vk::CreateShaderDesc(Resource::GetShaderProgram(MeshParticleUpdate).vertex);

    if (!meshParticleUpdatePass.BuildHeap(ctx.Device(), csMeshShader, &sceneHeapMappings.info)) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    // 2. All 3D mesh particle graphics pipelines are descriptor-heap pipelines
    //    sharing the empty layout + the scene registry mappings.
    meshParticleRenderLayout = emptyPipelineLayout;

    // 3. G-Buffer Deferred Graphics Pipeline (mesh_particle_render.hlsl)
    auto mpRenderShaders = Resource::GetShaderProgram(MeshParticleRender);
    return LoadAndCreateShaders(
               {.path = Resource::Paths::MeshParticleRenderVS, .fallback = mpRenderShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::MeshParticleRenderPS, .fallback = mpRenderShaders.fragment, .entryPoint = "PSMain"}
    )
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array) // Writes to SceneColor, Velocity, NormRough
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true) // Solid 3D geometry writes depth
                .CullBack()
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { meshParticleRenderPipeline = std::forward<decltype(pipeline)>(pipeline); });
        })
        .and_then([&]() -> std::expected<void, Error> {
            // 4. Directional Shadow Cascade Pipeline (mesh_particle_shadow.hlsl)
            auto mpShadowShaders = Resource::GetShaderProgram(MeshParticleShadow);
            return LoadAndCreateShaders(
                       {.path = Resource::Paths::MeshParticleShadowVS, .fallback = mpShadowShaders.vertex, .entryPoint = "VSMain"},
                       {.path = Resource::Paths::MeshParticleShadowPS, .fallback = mpShadowShaders.fragment, .entryPoint = "PSShadow"}
            )
                .and_then([&](auto&& shaders) -> std::expected<void, Error> {
                    return Vk::PipelineBuilder<0, true> {}
                        .Shaders(shaders)
                        .Layout(emptyPipelineLayout)
                        .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                        .DepthOnly()
                        .DepthFormat(VK_FORMAT_D32_SFLOAT)
                        .ViewMask(Passes::kCascadeViewMask) // Drawn inside the multiview cascade pass.
                        .CullNone()
                        .Build(ctx.Device())
                        .transform([&](auto&& pipeline) -> auto { meshParticleShadowPipeline = std::forward<decltype(pipeline)>(pipeline); });
                });
        });
}

auto RenderContext::Impl::BuildSkinningPipeline() -> std::expected<void, Error> {
    return Vk::PipelineLayoutBuilder(ctx.Device())
        .AddPushConstant(VK_SHADER_STAGE_COMPUTE_BIT, sizeof(SkinningConstants))
        .Build()
        .transform_error([](auto) -> Error { return RenderInitError::PipelineLayoutCreationFailed; })
        .and_then([&](auto&& layout) -> std::expected<void, Error> {
            skinningPass.pipelineLayout = std::forward<decltype(layout)>(layout);
            return LoadAndCreateComputeShader(
                       {.path = Resource::Paths::SkinningCS, .fallback = Resource::skinning_comp}, skinningPass.pipelineLayout.Get(), skinningPass
            )
                .transform([&](auto&& pipeline) -> auto { skinningPass.pipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::AllocateDynamicVertexBuffers(
    size_t                           maxVertices,
    DoubleBuffered<Vk::Buffer>&      bufs,
    DoubleBuffered<VkDeviceAddress>& addrs,
    VkBufferUsageFlags               extraFlags,
    const char*                      label
) noexcept -> std::expected<void, Error> {
    const size_t bufferSize = maxVertices * (sizeof(VertexPosition) + sizeof(VertexAttributes));

    for (int i = 0; i < 2; ++i) {
        auto res = Vk::Buffer::Create(
            allocator.Get(), bufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extraFlags,
            VMA_MEMORY_USAGE_CPU_TO_GPU
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

auto RenderContext::Impl::InitLineBuffers() noexcept -> std::expected<void, Error> {
    return AllocateDynamicVertexBuffers(kMaxLineVertices, frames.lineVbos, frames.lineVboAddresses, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "line");
}

auto RenderContext::Impl::BuildLinePipeline() -> std::expected<void, Error> {
    linePipelineLayout = emptyPipelineLayout;

    // The debug line pipeline rasterises through PSForward, so it needs the
    // Forward geometry variant (the G-buffer one emits motion vectors and a
    // normal frame that PSForward does not read).
    const auto forwardShaders = Resource::GetSceneShaders(Resource::SceneShaderVariant::Forward);

    return LoadAndCreateShaders(
               {.path = Resource::Paths::BasicVSForward, .fallback = forwardShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::ForwardPS, .fallback = forwardShaders.fragment, .entryPoint = "PSForward"}
    )
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
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
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { linePipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::InitShadowResources() -> std::expected<void, Error> {
    using enum RenderInitError;

    auto shadowSamplerBuilder = Vk::SamplerBuilder {}.Linear().ClampToBorder(VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE).DepthCompare();

    return shadowSamplerBuilder.Build(ctx.Device())
        .transform_error([](auto err) -> Error { return err; })

        // 1. Bind Sampler
        .and_then([&](auto&& sampler) -> std::expected<void, Error> {
            shadowSampler     = std::forward<decltype(sampler)>(sampler);
            shadowSamplerInfo = shadowSamplerBuilder.Info();
            return {};
        })

        // 2. Allocate Cascaded Shadow Map Render Target
        .and_then([&]() -> std::expected<void, Error> {
            auto sm_res = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                allocator, ctx, {.width = SHADOW_RES, .height = SHADOW_RES},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = NUM_CASCADES}
            );
            if (!sm_res) {
                return std::unexpected(SubsystemAllocationFailed);
            }
            graphResources.shadowMap = std::move(*sm_res);

            auto smp_res = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                allocator, ctx, {.width = SHADOW_RES, .height = SHADOW_RES},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = NUM_CASCADES}
            );
            if (!smp_res) {
                return std::unexpected(SubsystemAllocationFailed);
            }
            shadowMapPrev = std::move(*smp_res);
            return {};
        })

        // 3. Create Cascade Image Views
        .and_then([&]() -> std::expected<void, Error> {
            shadowCascadeViews.resize(NUM_CASCADES);
            shadowCascadeViewsPrev.resize(NUM_CASCADES);
            for (uint32_t i = 0; i < NUM_CASCADES; ++i) {
                {
                    auto view_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowMap.image.Handle(), i, 1);
                    if (!view_res) {
                        return std::unexpected(SubsystemAllocationFailed);
                    }
                    shadowCascadeViews[i] = std::move(*view_res);
                }
                {
                    auto prev_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), shadowMapPrev.image.Handle(), i, 1);
                    if (!prev_res) {
                        return std::unexpected(SubsystemAllocationFailed);
                    }
                    shadowCascadeViewsPrev[i] = std::move(*prev_res);
                }
                if (!shadowCascadeViews[i].Valid() || !shadowCascadeViewsPrev[i].Valid()) [[unlikely]] {
                    return std::unexpected(SubsystemAllocationFailed);
                }
            }
            return {};
        })

        // 4. Allocate Punctual Shadow Atlas Render Target
        .and_then([&]() -> std::expected<void, Error> {
            auto sa_res = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
                allocator, ctx, {.width = 1024, .height = 1024},
                {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = 24}
            );
            if (!sa_res) [[unlikely]] {
                return std::unexpected(SubsystemAllocationFailed);
            }
            graphResources.shadowAtlas = std::move(*sa_res);
            return {};
        })

        // 5. Create Atlas Image Views
        .and_then([&]() -> std::expected<void, Error> {
            {
                auto cube_res = Vk::CreateViewCubeArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowAtlas.image.Handle(), 24);
                if (!cube_res) {
                    return std::unexpected(SubsystemAllocationFailed);
                }
                shadowAtlasCubeView = std::move(*cube_res);
            }
            {
                auto array_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowAtlas.image.Handle(), 0, 24);
                if (!array_res) {
                    return std::unexpected(SubsystemAllocationFailed);
                }
                shadowAtlas2DView = std::move(*array_res);
            }
            shadowAtlasCubeViewInfo = {
                .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext      = nullptr,
                .flags      = 0,
                .image      = graphResources.shadowAtlas.image.Handle(),
                .viewType   = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY,
                .format     = VK_FORMAT_D32_SFLOAT,
                .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 24},
            };
            shadowAtlas2DViewInfo = {
                .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext      = nullptr,
                .flags      = 0,
                .image      = graphResources.shadowAtlas.image.Handle(),
                .viewType   = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                .format     = VK_FORMAT_D32_SFLOAT,
                .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 24},
            };
            if (!shadowAtlasCubeView.Valid() || !shadowAtlas2DView.Valid()) [[unlikely]] {
                return std::unexpected(SubsystemAllocationFailed);
            }
            return {};
        })

        // 6. Transition Layouts and Recreate Punctual Views
        .and_then([&]() -> std::expected<void, Error> {
            Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) -> void {
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                    cmd, graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                    cmd, shadowMapPrev.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, shadowMapPrev.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                    cmd, graphResources.shadowAtlas.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, graphResources.shadowAtlas.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
                );
            });

            RecreatePunctualShadowViews();
            return {};
        })

        // 7. Allocate Double-Buffered Frame Uniform Buffers
        //    VK_EXT_descriptor_heap: their device addresses feed the scene
        //    registry's PUSH_ADDRESS mappings, so they need
        //    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
        .and_then([&]() -> auto {
            return CreateDoubleBuffered(
                       allocator, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       VMA_MEMORY_USAGE_CPU_TO_GPU
            )
                .transform_error([](auto err) -> Error { return err; });
        })

        // 8. Allocate Double-Buffered Light Storage Buffers (same SDA requirement)
        .and_then([&](auto&& fub) -> auto {
            frames.frameUniformBuffers = std::forward<decltype(fub)>(fub);
            return CreateDoubleBuffered(
                       allocator, sizeof(Light) * 128, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       VMA_MEMORY_USAGE_CPU_TO_GPU
            )
                .transform_error([](auto err) -> Error { return err; });
        })

        // 9. Allocate Double-Buffered Indirect Argument Buffers
        .and_then([&](auto&& lsb) -> auto {
            frames.lightStorageBuffers = std::forward<decltype(lsb)>(lsb);
            return CreateDoubleBuffered(
                       allocator, sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances * 8, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU
            )
                .transform_error([](auto err) -> Error { return err; });
        })

        // 10. Complete pipeline assignment
        .transform([&](auto&& sib) -> auto { frames.shadowIndirectBuffers = std::forward<decltype(sib)>(sib); });
}

auto RenderContext::Impl::BuildDecalPipeline() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    static constexpr std::array<VkFormat, 2> decalFormats = {VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R8G8B8A8_UNORM};

    auto decalShaders = Resource::GetShaderProgram(Decal);

    // Reflects decal.slang set 0 ({texDepth, pointSampler}) and set 1 (the scene
    // parameter block subset). VK_EXT_descriptor_heap: the reflection feeds the
    // mapping tables (decalHeapMappings + decalSceneHeapMappings) that remap
    // both sets onto the heaps at pipeline creation; no descriptor sets exist.
    const Vk::ReflectedStageInput reflectInputs[2] = {
        {.shader = Vk::CreateShaderDesc(decalShaders.vertex), .stage = VK_SHADER_STAGE_VERTEX_BIT},
        {.shader = Vk::CreateShaderDesc(decalShaders.fragment), .stage = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    if (!decalDescLayout.Build(ctx.Device(), std::span {reflectInputs})) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
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
               {.path = Resource::Paths::DecalVS, .fallback = decalShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::DecalPS, .fallback = decalShaders.fragment, .entryPoint = "PSMain"}
    )
        .and_then([&](auto&& shaders) -> std::expected<void, Error> {
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
                .Build(ctx.Device())
                .transform([&](auto&& pipeline) -> auto { decalPipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::InitCSGPipelines() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    // We declare the shared shaders in a stack variable so all lambdas can reference it.
    Vk::ShaderStages shaders;

    auto basicShaders = Resource::GetShaderProgram(Basic);

    return LoadAndCreateShaders(
               {.path = Resource::Paths::BasicVS, .fallback = basicShaders.vertex, .entryPoint = "VSMain"},
               {.path = Resource::Paths::BasicPS, .fallback = basicShaders.fragment, .entryPoint = "PSMain"}
    )
        .and_then([&](auto&& compiledShaders) -> auto {
            shaders = std::forward<decltype(compiledShaders)>(compiledShaders);

            csgPipelineLayout             = emptyPipelineLayout;
            VkStencilOpState writeStencil = {
                .failOp      = VK_STENCIL_OP_KEEP,
                .passOp      = VK_STENCIL_OP_REPLACE,
                .depthFailOp = VK_STENCIL_OP_KEEP,
                .compareOp   = VK_COMPARE_OP_ALWAYS,
                .compareMask = 0xFF,
                .writeMask   = 0xFF,
                .reference   = 1
            };

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(false)
                .CullNone()
                .ColorWriteEnable(false)
                .StencilTest(true)
                .StencilOp(writeStencil, writeStencil)
                .Build(ctx.Device())
                .transform_error([](auto e) -> Error { return e; });
        })
        .and_then([&](auto&& writePipeline) -> auto {
            csgWritePipeline = std::forward<decltype(writePipeline)>(writePipeline);

            VkStencilOpState diffStencil = {
                .failOp      = VK_STENCIL_OP_KEEP,
                .passOp      = VK_STENCIL_OP_KEEP,
                .depthFailOp = VK_STENCIL_OP_KEEP,
                .compareOp   = VK_COMPARE_OP_NOT_EQUAL,
                .compareMask = 0xFF,
                .writeMask   = 0x00,
                .reference   = 1
            };

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                .ColorWriteEnable(true)
                .StencilTest(true)
                .StencilOp(diffStencil, diffStencil)
                .Build(ctx.Device())
                .transform_error([](auto e) -> Error { return e; });
        })
        .and_then([&](auto&& diffPipeline) -> auto {
            csgDifferencePipeline = std::forward<decltype(diffPipeline)>(diffPipeline);

            VkStencilOpState intersectStencil = {
                .failOp      = VK_STENCIL_OP_KEEP,
                .passOp      = VK_STENCIL_OP_KEEP,
                .depthFailOp = VK_STENCIL_OP_KEEP,
                .compareOp   = VK_COMPARE_OP_EQUAL,
                .compareMask = 0xFF,
                .writeMask   = 0x00,
                .reference   = 1
            };

            return Vk::PipelineBuilder<ActiveGBuffer::count, true> {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .ColorFormats(ActiveGBuffer::array)
                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT)
                .DepthTest(true)
                .DepthWrite(true)
                .CullBack()
                .ColorWriteEnable(true)
                .StencilTest(true)
                .StencilOp(intersectStencil, intersectStencil)
                .Build(ctx.Device())
                .transform_error([](auto e) -> Error { return e; });
        })
        .transform([&](auto&& intersectPipeline) -> auto {
            csgIntersectionPipeline = std::forward<decltype(intersectPipeline)>(intersectPipeline);

            WatchPipeline(Resource::Paths::BasicVS, Resource::Paths::BasicPS, [this]() -> void {
                auto res = InitCSGPipelines();
                if (!res) {
                    ZHLN::Log("ERROR: Failed to hot-reload CSG stencil pipelines: {}", res.error().Message());
                } else {
                    ZHLN::Log("[Shader Reload] CSG Stencil pipelines hot-reloaded successfully.");
                }
            });
        });
}

auto RenderContext::Impl::BuildHangGpuPipeline() -> std::expected<void, Error> {
    return Vk::PipelineLayoutBuilder(ctx.Device())
        .Build()
        .transform_error([](auto) -> Error { return RenderInitError::PipelineLayoutCreationFailed; })
        .and_then([&](auto&& layout) -> std::expected<void, Error> {
            hangGpuPass.pipelineLayout = std::forward<decltype(layout)>(layout);
            return LoadAndCreateComputeShader(
                       ComputeStageSource {.path = Resource::Paths::HangGpuCS, .fallback = Resource::hang_gpu_comp}, hangGpuPass.pipelineLayout.Get(),
                       hangGpuPass
            )
                .transform([&](auto&& pipeline) -> auto { hangGpuPass.pipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::BuildHiZPipeline() -> std::expected<void, Error> {
    auto shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::HizGenerateComp).vertex);
    if (!hizDescLayout.Build(ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    // VK_EXT_descriptor_heap: one slot span per mip (the pushed index is the
    // mip level). The span is fixed at 16: the HiZ map does not exist yet at
    // pipeline-build time (it is created on the first RecreateTargets).
    constexpr uint32_t kMaxHiZMips = 16;
    Vk::BuildHeapPassBindings(heapManager, hizDescLayout.reflectedSets[0], 0, heapPushDataLayout.heapIndexOffset, kMaxHiZMips, hizHeapBindings);

    return hizGeneratePass.BuildHeap(ctx.Device(), shader, hizHeapBindings.GetInfo(), hizHeapBindings.indexPushOffset);
}

auto RenderContext::Impl::CompileShadowPipeline(VkDevice device, const Resource::ShaderPair& shaderData) -> std::expected<void, Error> {
    // VK_EXT_descriptor_heap: the shadow pass reads the scene registry through
    // the heap; per-draw ObjectConstants travel via vkCmdPushDataEXT.
    shadowPipelineLayout = emptyPipelineLayout;
    return Vk::ShaderStages::Create(device, shaderData, "VSMain", "PSShadow")
        .transform_error([](auto err) -> Error { return err; })
        .and_then([&, device](auto&& shaders) -> std::expected<void, Error> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .DepthOnly()
                .DepthFormat(VK_FORMAT_D32_SFLOAT)
                // Multiview cascades: matches the single layered shadow render
                // pass (viewMask 0x0F); ViewIndex drives the light matrix.
                .ViewMask(Passes::kCascadeViewMask)
                .CullNone()
                .Build(device)
                .transform_error([](auto) -> Error { return RenderInitError::PipelineCreationFailed; })
                .transform([&](auto&& pipeline) -> auto { shadowPipeline = std::forward<decltype(pipeline)>(pipeline); });
        })
        .and_then([&, device]() -> std::expected<void, Error> {
            // VK_EXT_mesh_shader twin of the shadow pipeline. Optional by
            // design: a failure here only means the cascades keep using the
            // indirect vertex draws, so it never fails pipeline compilation.
            // The twin renders inside the multiview cascade pass and its mesh
            // stage reads SV_ViewID, which requires multiviewMeshShader --
            // skip creation entirely when that feature is unavailable.
            if (!ctx.MeshShadersSupported() || !ctx.MultiviewMeshShadingEnabled()) {
                return {};
            }

            const ZHLN_ShaderDesc taskDesc = {.code = Vk::AsSpirV(Resource::basic_task.data()), .size = Resource::basic_task.size(), .entry_point = nullptr};
            // Shadow variant: its varying set must match PSShadow exactly.
            const auto            shadowSet = Resource::GetSceneShaders(Resource::SceneShaderVariant::Shadow);
            const ZHLN_ShaderDesc meshDesc  = {.code = Vk::AsSpirV(shadowSet.mesh.data()), .size = shadowSet.mesh.size(), .entry_point = nullptr};
            const ZHLN_ShaderDesc fragDesc  = {.code = Vk::AsSpirV(shaderData.fragment.data()), .size = shaderData.fragment.size(), .entry_point = "PSShadow"};

            auto shaders = Vk::ShaderStages::CreateMesh(device, taskDesc, meshDesc, fragDesc);
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
                                .ViewMask(Passes::kCascadeViewMask) // Multiview cascades (must match the render pass).
                                .CullNone()
                                .Build(device);
            if (!pipeline) {
                ZHLN::Log("[RenderResources] Shadow mesh pipeline creation failed; cascades keep the vertex pipeline.");
                return {};
            }
            shadowMeshPipeline = std::move(*pipeline);
            return {};
        });
}

auto RenderContext::Impl::CompilePunctualShadowPipeline(VkDevice device, const Resource::ShaderPair& shaderData) -> std::expected<void, Error> {
    // VK_EXT_descriptor_heap variant of the shadow path (same mappings, the
    // per-draw light index travels through push data).
    punctualShadowPipelineLayout = emptyPipelineLayout;
    return Vk::ShaderStages::Create(device, shaderData)
        .transform_error([](auto err) -> Error { return err; })
        .and_then([&, device](auto&& shaders) -> std::expected<void, Error> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders)
                .Layout(emptyPipelineLayout)
                .HeapMappings(&sceneHeapMappings.info, &sceneHeapMappings.info)
                .DepthOnly()
                .DepthFormat(VK_FORMAT_D32_SFLOAT)
                .ViewMask(0x3F)
                .CullNone()
                .Build(device)
                .transform_error([](auto) -> Error { return RenderInitError::PipelineCreationFailed; })
                .transform([&](auto&& pipeline) -> auto { punctualShadowPipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto RenderContext::Impl::InitCullingResources() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    auto cullingShader = Vk::CreateShaderDesc(Resource::culling_comp);
    if (!cullingLayout.Build(ctx.Device(), cullingShader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    auto clusterCullingShader = Vk::CreateShaderDesc(Resource::GetShaderProgram(ClusterCulling).vertex);
    auto clusterDispatch      = Vk::ReflectComputeDispatchSize(clusterCullingShader);
    if (!clusterDispatch) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }
    const size_t numClusters = static_cast<size_t>((*clusterDispatch)[0]) * (*clusterDispatch)[1] * (*clusterDispatch)[2];

    Vk::BuildHeapPassBindings(heapManager, cullingLayout.reflectedSets[0], 0, heapPushDataLayout.heapIndexOffset, 4, cullingHeapBindings);

    constexpr VkBufferUsageFlags kInstanceUsage  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    constexpr VkBufferUsageFlags kIndirectUsage  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    constexpr VkBufferUsageFlags kCandidateUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    constexpr VkBufferUsageFlags kCountUsage     = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    return std::expected<void, Error> {}
        .and_then([&]() -> std::expected<void, Error> {
            return CreateDoubleBuffered(allocator, sizeof(InstanceData) * kGpuCullingMaxInstances, kInstanceUsage, VMA_MEMORY_USAGE_CPU_TO_GPU)
                .and_then([&](auto&& idb) {
                    frames.instanceDataBuffers = std::forward<decltype(idb)>(idb);
                    return CreateDoubleBuffered(allocator, sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances, kIndirectUsage, VMA_MEMORY_USAGE_GPU_ONLY);
                })
                .and_then([&](auto&& icb1) {
                    frames.indirectCommandsBuffers = std::forward<decltype(icb1)>(icb1);
                    return CreateDoubleBuffered(allocator, sizeof(VkDrawIndirectCommand) * kGpuCullingMaxInstances, kIndirectUsage, VMA_MEMORY_USAGE_GPU_ONLY);
                })
                .and_then([&](auto&& icb2) {
                    frames.indirectCommandsBuffersPass2 = std::forward<decltype(icb2)>(icb2);
                    return CreateDoubleBuffered(allocator, sizeof(uint32_t) * kGpuCullingMaxInstances, kCandidateUsage, VMA_MEMORY_USAGE_GPU_ONLY);
                })
                .and_then([&](auto&& spcb) {
                    frames.secondPassCandidatesBuffers = std::forward<decltype(spcb)>(spcb);
                    return CreateDoubleBuffered(allocator, sizeof(uint32_t), kCountUsage, VMA_MEMORY_USAGE_GPU_ONLY);
                })
                .transform([&](auto&& spcnt) { frames.secondPassCountBuffers = std::forward<decltype(spcnt)>(spcnt); });
        })
        .and_then([&]() -> std::expected<void, Error> {
            return cullingPass.BuildHeap(ctx.Device(), cullingShader, cullingHeapBindings.GetInfo(), cullingHeapBindings.indexPushOffset);
        })
        .and_then([&]() -> std::expected<void, Error> {
            auto bounds = Vk::Buffer::Create(
                allocator.Get(), sizeof(GPUTypes::Cluster::ClusterBounds) * numClusters,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY
            );
            if (!bounds) {
                return std::unexpected(bounds.error());
            }
            clusterBoundsBuffer = std::move(*bounds);

            if (!clusterCullingDescLayout.Build(ctx.Device(), clusterCullingShader, VK_SHADER_STAGE_COMPUTE_BIT)) {
                return std::unexpected(RenderInitError::PipelineCreationFailed);
            }
            Vk::BuildHeapPassBindings(
                heapManager, clusterCullingDescLayout.reflectedSets[0], 0, heapPushDataLayout.heapIndexOffset, 2, clusterCullingHeapBindings
            );

            constexpr VkBufferUsageFlags kClusterGridUsage   = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            constexpr VkBufferUsageFlags kLightIndexUsage    = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            constexpr VkBufferUsageFlags kGlobalCounterUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

            return CreateDoubleBuffered(allocator, sizeof(ClusterVolume) * numClusters, kClusterGridUsage, VMA_MEMORY_USAGE_GPU_ONLY)
                .and_then([&](auto&& cgb) {
                    frames.clusterGridBuffers = std::forward<decltype(cgb)>(cgb);
                    return CreateDoubleBuffered(allocator, sizeof(uint32_t) * numClusters * 64, kLightIndexUsage, VMA_MEMORY_USAGE_GPU_ONLY);
                })
                .and_then([&](auto&& lsb) {
                    frames.lightIndexListBuffers = std::forward<decltype(lsb)>(lsb);
                    return CreateDoubleBuffered(allocator, sizeof(uint32_t), kGlobalCounterUsage, VMA_MEMORY_USAGE_GPU_ONLY);
                })
                .transform([&](auto&& gcb) {
                    frames.globalCounterBuffers = std::forward<decltype(gcb)>(gcb);
                    for (uint32_t i = 0; i < 2; ++i) {
                        Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) -> void {
                            Vk::FillBuffer(cmd, frames.clusterGridBuffers[i], 0, 0u);
                            Vk::FillBuffer(cmd, frames.globalCounterBuffers[i], 0, 0u);
                        });
                        heapManager.WriteBindings(
                            ctx, clusterCullingHeapBindings, i, clusterBoundsBuffer, frames.clusterGridBuffers[i], frames.lightIndexListBuffers[i],
                            frames.globalCounterBuffers[i], frames.frameUniformBuffers[i], frames.lightStorageBuffers[i]
                        );
                    }
                });
        })
        .and_then([&]() -> std::expected<void, Error> {
            auto bDesc = Vk::CreateShaderDesc(Resource::GetShaderProgram(ClusterBounds).vertex);
            if (!clusterBoundsDescLayout.Build(ctx.Device(), bDesc, VK_SHADER_STAGE_COMPUTE_BIT)) {
                return std::unexpected(RenderInitError::PipelineCreationFailed);
            }
            Vk::BuildHeapPassBindings(
                heapManager, clusterBoundsDescLayout.reflectedSets[0], 0, heapPushDataLayout.heapIndexOffset, 2, clusterBoundsHeapBindings
            );
            for (int i = 0; i < 2; ++i) {
                heapManager.WriteBindings(ctx, clusterBoundsHeapBindings, i, clusterBoundsBuffer, frames.frameUniformBuffers[i]);
            }
            return clusterBoundsPass.BuildHeap(ctx.Device(), bDesc, clusterBoundsHeapBindings.GetInfo(), clusterBoundsHeapBindings.indexPushOffset);
        })
        .and_then([&]() -> std::expected<void, Error> {
            return clusterCullingPass.BuildHeap(
                ctx.Device(), clusterCullingShader, clusterCullingHeapBindings.GetInfo(), clusterCullingHeapBindings.indexPushOffset
            );
        })
        .and_then([&]() -> std::expected<void, Error> {
            if (!rtCtx.Valid()) {
                return {};
            }
            ZHLN_AccelerationStructureSizes tlasSizes;
            rtCtx.GetTLASSizes(kGpuCullingMaxInstances, tlasSizes);

            return CreateDoubleBuffered(
                       allocator, tlasSizes.acceleration_structure_size,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY
            )
                .and_then([&](auto&& tb) {
                    frames.tlasBuffer = std::forward<decltype(tb)>(tb);
                    return CreateDoubleBuffered(
                        allocator, tlasSizes.build_scratch_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY
                    );
                })
                .and_then([&](auto&& tsb) {
                    frames.tlasScratchBuffer = std::forward<decltype(tsb)>(tsb);
                    return CreateDoubleBuffered(
                        allocator, sizeof(VkAccelerationStructureInstanceKHR) * kGpuCullingMaxInstances,
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VMA_MEMORY_USAGE_GPU_ONLY
                    );
                })
                .and_then([&](auto&& tib) {
                    frames.tlasInstanceBuffers = std::forward<decltype(tib)>(tib);
                    return CreateDoubleBuffered(
                        allocator, sizeof(VkAccelerationStructureInstanceKHR) * kGpuCullingMaxInstances, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VMA_MEMORY_USAGE_CPU_ONLY
                    );
                })
                .transform([&](auto&& tstb) {
                    frames.tlasStagingBuffers = std::forward<decltype(tstb)>(tstb);
                    for (uint32_t i = 0; i < 2; ++i) {
                        frames.tlas[i] =
                            rtCtx.CreateAccelerationStructure(frames.tlasBuffer[i].Handle(), tlasSizes.acceleration_structure_size, ZHLN_AS_TYPE_TOP_LEVEL);
                    }
                });
        })
        .and_then([&]() -> std::expected<void, Error> { return BuildSkinningPipeline(); })
        .transform([&]() -> void {
            if constexpr (isDev) {
                RegisterShaderWatcher(Resource::Paths::SkinningCS, [this]() -> void {
                    auto res = BuildSkinningPipeline();
                    if (!res) {
                        ZHLN::Log("ERROR: Failed to hot-reload Skinning pipeline: {}", res.error().Message());
                    } else {
                        ZHLN::Log("[Shader Reload] Skinning pipeline hot-reloaded successfully.");
                    }
                });
            }
        });
}

} // namespace ZHLN
