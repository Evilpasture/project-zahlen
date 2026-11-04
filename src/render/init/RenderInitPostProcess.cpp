// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include "PassDescriptors.hpp"
#include "pipeline/ComputePass.hpp"
#include "../passes/aa/SMAAVariants.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <span>
#include <tuple>

namespace ZHLN {

auto RenderContext::Impl::BuildFXAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, antiAliasing.fxaa, Vk::MakeStageSource<Shaders::Modules::FxaaVS>(), Vk::MakeStageSource<Shaders::Modules::FxaaPS>(),
        {Vk::Format::R16G16B16A16Sfloat}
    );
}

auto RenderContext::Impl::BuildSMAAPipeline() -> std::expected<void, ErrorCode> {
    // Edge detection and neighborhood blending have no specialization: one
    // pipeline each. Only the weight pass carries the two search-radius
    // vk::constant_id values from SMAA.slang.
    return BuildPassHelper(
               this, antiAliasing.smaaEdge, Vk::MakeStageSource<Shaders::Modules::SmaaEdgeVS>(),
               Vk::MakeStageSource<Shaders::Modules::SmaaEdgePS>(), {Vk::Format::R8G8Unorm}
    )
        .and_then([&]() -> std::expected<void, ErrorCode> {
            // One pair of SPIR-V modules, three VkPipelines. BuildPassVariants
            // -> BuildHeapVariants gives the same VkSpecializationInfo to BOTH
            // stages, so SmaaWeightVS's search window (offset[2]) and
            // SmaaWeightPS's [loop] search bounds always agree. The variants
            // live in FullscreenPass::pipelines; the single `pipeline` field
            // stays empty and the draw must select by index (SMAAPasses.cpp).
            //
            // kSmaaWeightVariants has static storage duration, which is what
            // VkSpecializationInfo::pData needs out to pipeline creation.
            Vk::Specialization<Passes::SmaaWeightSpec> spec;
            Reflect::ForEachFieldInfo<Passes::SmaaWeightSpec>(spec);
            const auto specInfos = spec.Infos(std::span {Passes::kSmaaWeightVariants});

            return BuildPassVariants(
                this, antiAliasing.smaaWeight, Vk::MakeStageSource<Shaders::Modules::SmaaWeightVS>(),
                Vk::MakeStageSource<Shaders::Modules::SmaaWeightPS>(), {Vk::Format::R8G8B8A8Unorm}, specInfos
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BuildPassHelper(
                this, antiAliasing.smaaBlend, Vk::MakeStageSource<Shaders::Modules::SmaaBlendVS>(),
                Vk::MakeStageSource<Shaders::Modules::SmaaBlendPS>(), {Vk::Format::R16G16B16A16Sfloat}
            );
        });
}

namespace {

// Binds one heap-indexed compute pass: reflect the descriptor set, allocate its
// heap rows, then build the pipeline against those rows. The reflected layout is
// a local -- it is consumed entirely inside this call, so nothing keeps a copy of
// it alive on the frame the way the old per-pass layout fields did.
template <Vk::ShaderProgram Module, typename PushConstants>
auto BuildBoundTyped(RenderContext::Impl& impl, Vk::BoundComputePass<Vk::TypedComputePass<Module, PushConstants>>& bound) -> std::expected<void, ErrorCode> {
    const auto          shader = Vk::CreateShaderDesc<Module>();
    Vk::ReflectedLayout layout;
    if (!layout.Build(impl.ctx.Device(), shader, Vk::ShaderStage::Compute)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    if (auto built =
            Vk::BuildHeapPassBindings(impl.heapManager, layout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, bound.bindings);
        !built) {
        return std::unexpected(built.error());
    }
    Vk::HeapMappingArena mappingArena {impl.heapManager};
    return bound.pass.Init(impl.ctx.Device(), mappingArena.Map(bound.bindings.mappings), bound.bindings.indexPushOffset, &impl.pipelineBinaries);
}

// The runtime-SPIR-V twin of BuildBoundTyped, for passes that are not template
// instantiations over a shader module.
auto BuildBoundDynamic(RenderContext::Impl& impl, Vk::BoundComputePass<Vk::DynamicComputePass>& bound, std::span<const uint8_t> spirv)
    -> std::expected<void, ErrorCode> {
    const auto          shader = Vk::CreateShaderDesc(spirv);
    Vk::ReflectedLayout layout;
    if (!layout.Build(impl.ctx.Device(), shader, Vk::ShaderStage::Compute)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    if (auto built =
            Vk::BuildHeapPassBindings(impl.heapManager, layout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, bound.bindings);
        !built) {
        return std::unexpected(built.error());
    }
    Vk::HeapMappingArena mappingArena {impl.heapManager};
    return bound.pass.BuildHeap(impl.ctx.Device(), shader, mappingArena.Map(bound.bindings.mappings), bound.bindings.indexPushOffset, &impl.pipelineBinaries);
}

} // namespace

auto RenderContext::Impl::BuildLightingResources() -> std::expected<void, ErrorCode> {
    return std::expected<void, ErrorCode> {}
        .and_then([&]() -> std::expected<void, ErrorCode> { return BuildBoundTyped(*this, lighting.gtao); })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BuildBoundDynamic(*this, lighting.rcgiResolve, Shaders::Modules::RcgiResolveCS::Bytes());
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            // The ray-traced passes are only reachable on a device that supports RT;
            // the bundles stay empty (and the passes stay gated) when it is absent.
            if (!ctx.RayTracingSupported()) {
                return {};
            }
            return BuildBoundDynamic(*this, lighting.rcgiProbeSetup, Shaders::Modules::RcgiProbeSetupCS::Bytes());
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (!ctx.RayTracingSupported()) {
                return {};
            }
            return BuildBoundDynamic(*this, lighting.rcgiCascadeTraceMerge, Shaders::Modules::RcgiCascadeTraceMergeCS::Bytes());
        });
}

auto RenderContext::Impl::BuildPostProcessResources() -> std::expected<void, ErrorCode> {
    return BuildBoundDynamic(*this, post.bloomThreshold, Shaders::Modules::BloomThresholdCS::Bytes())
        .and_then([&]() -> std::expected<void, ErrorCode> { return BuildBoundDynamic(*this, post.bloomDown, Shaders::Modules::BloomDownCS::Bytes()); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return BuildBoundDynamic(*this, post.bloomUp, Shaders::Modules::BloomUpCS::Bytes()); });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto RenderContext::Impl::BakeSMAALUTs() -> std::expected<void, ErrorCode> {
    using SMAALUTPush = Slang::SMAALUTPush;

    const Vk::ShaderDesc shader = Vk::CreateShaderDesc<Shaders::Modules::SmaaLutCS>();
    Vk::HeapMappingArena mappingArena {heapManager};
    return Vk::CreateHeapComputePass(ctx.Device(), shader, mappingArena.Map(bakeHeapBindings.mappings), bakeHeapBindings.indexPushOffset, &pipelineBinaries)
        .and_then([&](Vk::DynamicComputePass pass) -> std::expected<void, ErrorCode> {
            return BakeComputeTexture2D<Shaders::Bake, Shaders::Modules::SmaaLutCS>(
                       pass, 160, 560, Vk::Format::R8G8B8A8Unorm, SMAALUTPush {.width = 160, .height = 560, .mode = 0}
            )
                .and_then([&](uint32_t areaIdx) -> std::expected<uint32_t, ErrorCode> {
                    antiAliasing.smaaAreaTexIdx = areaIdx;
                    return BakeComputeTexture2D<Shaders::Bake, Shaders::Modules::SmaaLutCS>(
                        pass, 64, 16, Vk::Format::R8G8B8A8Unorm, SMAALUTPush {.width = 64, .height = 16, .mode = 1}
                    );
                })
                .transform([&](uint32_t searchIdx) -> void {
                    antiAliasing.smaaSearchTexIdx = searchIdx;
                    ZHLN::Log("[SMAA] Area and search LUTs baked on GPU.");
                });
        });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

auto RenderContext::Impl::BuildLightingPipeline() -> std::expected<void, ErrorCode> {
    struct SpecData {
        int enableRTR = 0;
    };

    Vk::Specialization<SpecData> spec;
    Reflect::ForEachFieldInfo<SpecData>(spec);

    const std::array variants  = {SpecData {.enableRTR = 0}, SpecData {.enableRTR = 1}};
    const auto       specInfos = spec.Infos(std::span {variants});

    if (ctx.RayTracingSupported()) {
        return BuildPassVariants(
            this, post.lighting, Vk::MakeStageSource<Shaders::Modules::LightingVS>(),
            Vk::MakeStageSource<Shaders::Modules::LightingPS>(), {Vk::Format::R16G16B16A16Sfloat, Vk::Format::R8Unorm}, specInfos
        );
    }
    return BuildPassVariants(
        this, post.lighting, Vk::MakeStageSource<Shaders::Modules::LightingNortVS>(),
        Vk::MakeStageSource<Shaders::Modules::LightingNortPS>(), {Vk::Format::R16G16B16A16Sfloat, Vk::Format::R8Unorm}, specInfos
    );
}

auto RenderContext::Impl::BuildReflectionPipelines() -> std::expected<void, ErrorCode> {
    struct SpecData {
        int enableSSR = 0;
        int enableRTR = 0;
    };

    Vk::Specialization<SpecData> spec;
    Reflect::ForEachFieldInfo<SpecData>(spec);

    const std::array variants = {
        SpecData {.enableSSR = 0, .enableRTR = 0}, SpecData {.enableSSR = 1, .enableRTR = 0}, SpecData {.enableSSR = 0, .enableRTR = 1},
        SpecData {.enableSSR = 1, .enableRTR = 1}
    };
    const auto specInfos = spec.Infos(std::span {variants});

    if (ctx.RayTracingSupported()) {
        return BuildPassVariants(
            this, reflectionPipeline.pass, Vk::MakeStageSource<Shaders::Modules::ReflectionVS>(),
            Vk::MakeStageSource<Shaders::Modules::ReflectionPS>(), {Vk::Format::R16G16B16A16Sfloat}, specInfos, false, &sceneSet1HeapMappings
        );
    }
    return BuildPassVariants(
        this, reflectionPipeline.pass, Vk::MakeStageSource<Shaders::Modules::ReflectionNortVS>(),
        Vk::MakeStageSource<Shaders::Modules::ReflectionNortPS>(), {Vk::Format::R16G16B16A16Sfloat}, specInfos, false, &sceneSet1HeapMappings
    );
}

auto RenderContext::Impl::BuildBlitPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, post.blit, Vk::MakeStageSource<Shaders::Modules::BlitVS>(), Vk::MakeStageSource<Shaders::Modules::BlitPS>(),
        {presenter.GetPresentFormat()}
    );
}

auto RenderContext::Impl::BuildSpecializedLightingPipelines() -> std::expected<void, ErrorCode> {
    return BuildLightingPipeline().and_then([&]() -> std::expected<void, ErrorCode> { return BuildReflectionPipelines(); });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto RenderContext::Impl::InitPostProcessing() -> std::expected<void, ErrorCode> {
    defaultSampler = Vk::Sampler::LinearClampToEdge();
    pointSampler   = Vk::Sampler::NearestClampToEdge();

    WritePointSamplerToHeap(pointSampler);
    return std::expected<void, ErrorCode> {}
        .and_then([&]() -> std::expected<void, ErrorCode> {
            auto passes = std::make_tuple(
                GraphicsPassDesc {
                    .pass        = antiAliasing.fxaa,
                    .name        = "FXAA",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::FxaaVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::FxaaPS>(),
                    .colorFormat = Vk::Format::R16G16B16A16Sfloat
                },
                // The SMAA pack is built by BuildSMAAPipeline() below, not by
                // BuildDescribedPass: the weight pass needs three specialized
                // pipeline variants, and a plain hot-reload rebuild would call
                // BuildPassHelper and silently discard them.
                GraphicsPassDesc {
                    .pass        = post.blit,
                    .name        = "Blit",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::BlitVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::BlitPS>(),
                    .colorFormat = presenter.GetPresentFormat()
                }
            );
            return std::apply(
                [this](auto&&... descs) -> std::expected<void, ErrorCode> {
                    std::expected<void, ErrorCode> fold {};
                    ((fold = fold.and_then([this, &descs]() -> std::expected<void, ErrorCode> { return BuildDescribedPass(this, descs); })), ...);
                    return fold;
                },
                passes
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            // All three SMAA passes rebuild together on a shader reload, so the
            // weight variants are always regenerated with the same tier table.
            return RegisterAndBuild(
                this, "SMAA", [this]() -> std::expected<void, ErrorCode> { return BuildSMAAPipeline(); },
                {Shaders::Modules::SmaaEdgeVS::Path, Shaders::Modules::SmaaEdgePS::Path,
                 Shaders::Modules::SmaaWeightVS::Path, Shaders::Modules::SmaaWeightPS::Path,
                 Shaders::Modules::SmaaBlendVS::Path, Shaders::Modules::SmaaBlendPS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Lighting", [this]() -> std::expected<void, ErrorCode> { return BuildSpecializedLightingPipelines(); },
                {Shaders::Modules::LightingVS::Path, Shaders::Modules::LightingPS::Path, Shaders::Modules::LightingNortVS::Path,
                 Shaders::Modules::LightingNortPS::Path, Shaders::Modules::ReflectionVS::Path, Shaders::Modules::ReflectionPS::Path,
                 Shaders::Modules::ReflectionNortVS::Path, Shaders::Modules::ReflectionNortPS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Ray Traced Lighting", [this]() -> std::expected<void, ErrorCode> { return BuildLightingResources(); },
                {Shaders::Modules::GtaoCS::Path, Shaders::Modules::RcgiProbeSetupCS::Path,
                 Shaders::Modules::RcgiCascadeTraceMergeCS::Path, Shaders::Modules::RcgiResolveCS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Bloom", [this]() -> std::expected<void, ErrorCode> { return BuildPostProcessResources(); },
                {Shaders::Modules::BloomThresholdCS::Path, Shaders::Modules::BloomDownCS::Path, Shaders::Modules::BloomUpCS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Volumetrics", [this]() -> std::expected<void, ErrorCode> { return fog.Build(*this); },
                {Shaders::Modules::VolumetricClearCS::Path, Shaders::Modules::VolumetricFogInjectCS::Path, Shaders::Modules::VolumetricLightInjectCS::Path,
                 Shaders::Modules::VolumetricIntegrationCS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Particles", [this]() -> std::expected<void, ErrorCode> { return BuildParticlePipelines(); },
                {Shaders::Modules::ParticleUpdateCS::Path, Shaders::Modules::ParticleRenderVS::Path, Shaders::Modules::ParticleRenderPS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "3D Mesh Particles", [this]() -> std::expected<void, ErrorCode> { return BuildMeshParticlePipelines(); },
                {Shaders::Modules::MeshParticleUpdateCS::Path, Shaders::Modules::MeshParticleRenderVS::Path, Shaders::Modules::MeshParticleRenderPS::Path,
                 Shaders::Modules::MeshParticleShadowVS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Decals", [this]() -> std::expected<void, ErrorCode> { return BuildDecalPipeline(); },
                {Shaders::Modules::DecalVS::Path, Shaders::Modules::DecalPS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> { return BakeSMAALUTs(); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return fog.InitializeNoise(*this); })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            InitPassSamplerDescriptors();
            return {};
        });
}

} // namespace ZHLN
