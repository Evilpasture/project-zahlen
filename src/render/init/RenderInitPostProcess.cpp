// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include "PassDescriptors.hpp"
#include "pipeline/ComputePass.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <span>
#include <tuple>

namespace ZHLN {

auto RenderContext::Impl::BuildTAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, antiAliasing.taa, Vk::MakeStageSource<Shaders::Modules::TaaVS>(), Vk::MakeStageSource<Shaders::Modules::TaaPS>(),
        {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildFXAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, antiAliasing.fxaa, Vk::MakeStageSource<Shaders::Modules::FxaaVS>(), Vk::MakeStageSource<Shaders::Modules::FxaaPS>(),
        {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildMLAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, antiAliasing.mlaa, Vk::MakeStageSource<Shaders::Modules::MlaaVS>(), Vk::MakeStageSource<Shaders::Modules::MlaaPS>(),
        {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildSMAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
               this, antiAliasing.smaaEdge, Vk::MakeStageSource<Shaders::Modules::SmaaEdgeVS>(),
               Vk::MakeStageSource<Shaders::Modules::SmaaEdgePS>(), {VK_FORMAT_R8G8_UNORM}
    )
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BuildPassHelper(
                this, antiAliasing.smaaWeight, Vk::MakeStageSource<Shaders::Modules::SmaaWeightVS>(),
                Vk::MakeStageSource<Shaders::Modules::SmaaWeightPS>(), {VK_FORMAT_R8G8B8A8_UNORM}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BuildPassHelper(
                this, antiAliasing.smaaBlend, Vk::MakeStageSource<Shaders::Modules::SmaaBlendVS>(),
                Vk::MakeStageSource<Shaders::Modules::SmaaBlendPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}
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
    if (!layout.Build(impl.ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
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
    if (!layout.Build(impl.ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
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
            return BuildBoundDynamic(*this, lighting.rtgiComposite, Shaders::Modules::RtgiCompositeCS::Bytes());
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BuildBoundDynamic(*this, lighting.rcgiResolve, Shaders::Modules::RcgiResolveCS::Bytes());
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            // The ray-traced half is only reachable on a device that supports it;
            // the bundles stay empty (and the passes stay gated) when it is absent.
            if (!ctx.RayTracingSupported()) {
                return {};
            }
            return BuildBoundTyped(*this, lighting.rtShadow);
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (!ctx.RayTracingSupported()) {
                return {};
            }
            return BuildBoundDynamic(*this, lighting.shadowDenoise, Shaders::Modules::ShadowDenoiseAtrousCS::Bytes());
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (!ctx.RayTracingSupported()) {
                return {};
            }
            return BuildBoundTyped(*this, lighting.rtrHalf);
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
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
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (!ctx.RayTracingSupported()) {
                return {};
            }
            return BuildBoundDynamic(*this, lighting.rtgiDenoise, Shaders::Modules::RtgiDenoiseCS::Bytes());
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (!ctx.RayTracingSupported()) {
                return {};
            }
            return BuildBoundTyped(*this, lighting.ddgiTrace);
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (!ctx.RayTracingSupported()) {
                return {};
            }
            return BuildBoundTyped(*this, lighting.ddgiUpdate);
        });
}

auto RenderContext::Impl::BuildPostProcessResources() -> std::expected<void, ErrorCode> {
    return BuildBoundDynamic(*this, post.bloomThreshold, Shaders::Modules::BloomThresholdCS::Bytes())
        .and_then([&]() -> std::expected<void, ErrorCode> { return BuildBoundDynamic(*this, post.bloomDown, Shaders::Modules::BloomDownCS::Bytes()); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return BuildBoundDynamic(*this, post.bloomUp, Shaders::Modules::BloomUpCS::Bytes()); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return BuildBoundDynamic(*this, post.hdrDenoise, Shaders::Modules::HdrDenoiseAtrousCS::Bytes()); });
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
                       pass, 160, 560, VK_FORMAT_R8G8B8A8_UNORM, SMAALUTPush {.width = 160, .height = 560, .mode = 0}
            )
                .and_then([&](uint32_t areaIdx) -> std::expected<uint32_t, ErrorCode> {
                    antiAliasing.smaaAreaTexIdx = areaIdx;
                    return BakeComputeTexture2D<Shaders::Bake, Shaders::Modules::SmaaLutCS>(
                        pass, 64, 16, VK_FORMAT_R8G8B8A8_UNORM, SMAALUTPush {.width = 64, .height = 16, .mode = 1}
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
            Vk::MakeStageSource<Shaders::Modules::LightingPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8_UNORM}, specInfos
        );
    }
    return BuildPassVariants(
        this, post.lighting, Vk::MakeStageSource<Shaders::Modules::LightingNortVS>(),
        Vk::MakeStageSource<Shaders::Modules::LightingNortPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8_UNORM}, specInfos
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
            Vk::MakeStageSource<Shaders::Modules::ReflectionPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos, false, &sceneSet1HeapMappings
        );
    }
    return BuildPassVariants(
        this, reflectionPipeline.pass, Vk::MakeStageSource<Shaders::Modules::ReflectionNortVS>(),
        Vk::MakeStageSource<Shaders::Modules::ReflectionNortPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos, false, &sceneSet1HeapMappings
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
                    .pass        = antiAliasing.taa,
                    .name        = "TAA",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::TaaVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::TaaPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = antiAliasing.fxaa,
                    .name        = "FXAA",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::FxaaVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::FxaaPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = antiAliasing.mlaa,
                    .name        = "MLAA",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::MlaaVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::MlaaPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = antiAliasing.smaaEdge,
                    .name        = "SMAA Edge Detection",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::SmaaEdgeVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::SmaaEdgePS>(),
                    .colorFormat = VK_FORMAT_R8G8_UNORM
                },
                GraphicsPassDesc {
                    .pass        = antiAliasing.smaaWeight,
                    .name        = "SMAA Blending Weight",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::SmaaWeightVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::SmaaWeightPS>(),
                    .colorFormat = VK_FORMAT_R8G8B8A8_UNORM
                },
                GraphicsPassDesc {
                    .pass        = antiAliasing.smaaBlend,
                    .name        = "SMAA Neighborhood Blend",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::SmaaBlendVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::SmaaBlendPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
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
                {Shaders::Modules::GtaoCS::Path, Shaders::Modules::RtShadowCS::Path, Shaders::Modules::ShadowDenoiseAtrousCS::Path,
                 Shaders::Modules::RtrHalfCS::Path, Shaders::Modules::RcgiProbeSetupCS::Path, Shaders::Modules::RcgiCascadeTraceMergeCS::Path,
                 Shaders::Modules::RcgiResolveCS::Path, Shaders::Modules::RtgiDenoiseCS::Path, Shaders::Modules::RtgiCompositeCS::Path,
                 Shaders::Modules::DdgiTraceCS::Path, Shaders::Modules::DdgiUpdateIrradianceCS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Bloom", [this]() -> std::expected<void, ErrorCode> { return BuildPostProcessResources(); },
                {Shaders::Modules::BloomThresholdCS::Path, Shaders::Modules::BloomDownCS::Path, Shaders::Modules::BloomUpCS::Path,
                 Shaders::Modules::HdrDenoiseAtrousCS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Volumetrics", [this]() -> std::expected<void, ErrorCode> { return fog.Build(*this); },
                {Shaders::Modules::VolumetricClearCS::Path, Shaders::Modules::VolumetricFogInjectCS::Path, Shaders::Modules::VolumetricLightInjectCS::Path,
                 Shaders::Modules::VolumetricIntegrationCS::Path, Shaders::Modules::VolumetricTemporalCS::Path}
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
