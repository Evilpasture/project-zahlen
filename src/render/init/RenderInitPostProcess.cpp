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
#include <tuple>

namespace ZHLN {

auto RenderContext::Impl::BuildTAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, taaPass, Vk::MakeStageSource<Shaders::Modules::TaaVS>(), Vk::MakeStageSource<Shaders::Modules::TaaPS>(),
        {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildFXAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, fxaaPass, Vk::MakeStageSource<Shaders::Modules::FxaaVS>(), Vk::MakeStageSource<Shaders::Modules::FxaaPS>(),
        {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildMLAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, mlaaPass, Vk::MakeStageSource<Shaders::Modules::MlaaVS>(), Vk::MakeStageSource<Shaders::Modules::MlaaPS>(),
        {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildSMAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
               this, smaaEdgePass, Vk::MakeStageSource<Shaders::Modules::SmaaEdgeVS>(),
               Vk::MakeStageSource<Shaders::Modules::SmaaEdgePS>(), {VK_FORMAT_R8G8_UNORM}
    )
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BuildPassHelper(
                this, smaaWeightPass, Vk::MakeStageSource<Shaders::Modules::SmaaWeightVS>(),
                Vk::MakeStageSource<Shaders::Modules::SmaaWeightPS>(), {VK_FORMAT_R8G8B8A8_UNORM}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BuildPassHelper(
                this, smaaBlendPass, Vk::MakeStageSource<Shaders::Modules::SmaaBlendVS>(),
                Vk::MakeStageSource<Shaders::Modules::SmaaBlendPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}
            );
        });
}

auto RenderContext::Impl::BuildLightingPipeline() -> std::expected<void, ErrorCode> {
    struct SpecData {
        int enableRTR = 0;
    };

    Vk::Specialization<SpecData> spec;
    Reflect::ForEachFieldInfo<SpecData>(spec);

    const std::array variants  = {SpecData {.enableRTR = 0}, SpecData {.enableRTR = 1}};
    const auto       specInfos = spec.Infos(variants);

    if (ctx.RayTracingSupported()) {
        return BuildPassVariants(
            this, lightingPass, Vk::MakeStageSource<Shaders::Modules::LightingVS>(),
            Vk::MakeStageSource<Shaders::Modules::LightingPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
        );
    }
    return BuildPassVariants(
        this, lightingPass, Vk::MakeStageSource<Shaders::Modules::LightingNortVS>(),
        Vk::MakeStageSource<Shaders::Modules::LightingNortPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
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
    const auto specInfos = spec.Infos(variants);

    if (ctx.RayTracingSupported()) {
        return BuildPassVariants(
            this, reflectionPipeline.pass, Vk::MakeStageSource<Shaders::Modules::ReflectionVS>(),
            Vk::MakeStageSource<Shaders::Modules::ReflectionPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
        );
    }
    return BuildPassVariants(
        this, reflectionPipeline.pass, Vk::MakeStageSource<Shaders::Modules::ReflectionNortVS>(),
        Vk::MakeStageSource<Shaders::Modules::ReflectionNortPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
}

auto RenderContext::Impl::BuildBlitPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, blitPass, Vk::MakeStageSource<Shaders::Modules::BlitVS>(), Vk::MakeStageSource<Shaders::Modules::BlitPS>(),
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
    defaultSamplerConfig = Vk::SamplerConfig::LinearClampToEdge();
    return std::expected<void, ErrorCode> {}
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return defaultSamplerConfig.Create(ctx.Device()).and_then([&](auto defaultResult) -> auto {
                defaultSampler = std::move(defaultResult);
                pointSamplerConfig = Vk::SamplerConfig::NearestRepeat().WithAddressMode(Vk::SamplerAddressMode::ClampToEdge);
                return pointSamplerConfig.Create(ctx.Device()).transform([&](auto pointResult) -> auto {
                    pointSampler = std::move(pointResult);
                    WritePointSamplerToHeap(pointSamplerConfig);
                });
            });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            auto passes = std::make_tuple(
                GraphicsPassDesc {
                    .pass        = taaPass,
                    .name        = "TAA",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::TaaVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::TaaPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = fxaaPass,
                    .name        = "FXAA",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::FxaaVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::FxaaPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = mlaaPass,
                    .name        = "MLAA",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::MlaaVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::MlaaPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = smaaEdgePass,
                    .name        = "SMAA Edge Detection",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::SmaaEdgeVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::SmaaEdgePS>(),
                    .colorFormat = VK_FORMAT_R8G8_UNORM
                },
                GraphicsPassDesc {
                    .pass        = smaaWeightPass,
                    .name        = "SMAA Blending Weight",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::SmaaWeightVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::SmaaWeightPS>(),
                    .colorFormat = VK_FORMAT_R8G8B8A8_UNORM
                },
                GraphicsPassDesc {
                    .pass        = smaaBlendPass,
                    .name        = "SMAA Neighborhood Blend",
                    .vs          = Vk::MakeStageSource<Shaders::Modules::SmaaBlendVS>(),
                    .ps          = Vk::MakeStageSource<Shaders::Modules::SmaaBlendPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = blitPass,
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
                this, "Bloom", [this]() -> std::expected<void, ErrorCode> { return postProcess.Build(*this); },
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
        .and_then([&]() -> std::expected<void, ErrorCode> { return postProcess.BakeSMAALUTs(*this); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return fog.InitializeNoise(*this); })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            InitPassSamplerDescriptors();
            return {};
        });
}

} // namespace ZHLN
