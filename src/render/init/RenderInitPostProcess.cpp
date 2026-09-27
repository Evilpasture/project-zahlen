// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../RenderInternal.hpp"
#include "pipeline/ComputePass.hpp"
#include "../Resources.hpp"
#include "PassDescriptors.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <tuple>

namespace ZHLN {

auto RenderContext::Impl::BuildTAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, taaPass, MakeStageSource<ShaderStage::Vertex, Shaders::Modules::TaaVS>(),
        MakeStageSource<ShaderStage::Fragment, Shaders::Modules::TaaPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildFXAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, fxaaPass, MakeStageSource<ShaderStage::Vertex, Shaders::Modules::FxaaVS>(),
        MakeStageSource<ShaderStage::Fragment, Shaders::Modules::FxaaPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildMLAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, mlaaPass, MakeStageSource<ShaderStage::Vertex, Shaders::Modules::MlaaVS>(),
        MakeStageSource<ShaderStage::Fragment, Shaders::Modules::MlaaPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildSMAAPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
               this, smaaEdgePass, MakeStageSource<ShaderStage::Vertex, Shaders::Modules::SmaaEdgeVS>(),
               MakeStageSource<ShaderStage::Fragment, Shaders::Modules::SmaaEdgePS>(), {VK_FORMAT_R8G8_UNORM}
    )
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BuildPassHelper(
                this, smaaWeightPass,
                MakeStageSource<ShaderStage::Vertex, Shaders::Modules::SmaaWeightVS>(),
                MakeStageSource<ShaderStage::Fragment, Shaders::Modules::SmaaWeightPS>(), {VK_FORMAT_R8G8B8A8_UNORM}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BuildPassHelper(
                this, smaaBlendPass,
                MakeStageSource<ShaderStage::Vertex, Shaders::Modules::SmaaBlendVS>(),
                MakeStageSource<ShaderStage::Fragment, Shaders::Modules::SmaaBlendPS>(), {VK_FORMAT_R16G16B16A16_SFLOAT}
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
            this, lightingPass, MakeStageSource<ShaderStage::Vertex, Shaders::Modules::LightingVS>(), MakeStageSource<ShaderStage::Fragment, Shaders::Modules::LightingPS>(),
            {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
        );
    }
    return BuildPassVariants(
        this, lightingPass, MakeStageSource<ShaderStage::Vertex, Shaders::Modules::LightingNortVS>(), MakeStageSource<ShaderStage::Fragment, Shaders::Modules::LightingNortPS>(),
        {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
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
        auto res = BuildPassVariants(
            this, reflectionPass, MakeStageSource<ShaderStage::Vertex, Shaders::Modules::ReflectionVS>(), MakeStageSource<ShaderStage::Fragment, Shaders::Modules::ReflectionPS>(),
            {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
        );
        if (!res) {
            return res;
        }
        return BuildPassVariants(
            this, translucentReflectionPass, MakeStageSource<ShaderStage::Vertex, Shaders::Modules::ReflectionVS>(), MakeStageSource<ShaderStage::Fragment, Shaders::Modules::ReflectionPS>(),
            {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
        );
    }
    auto res = BuildPassVariants(
        this, reflectionPass, MakeStageSource<ShaderStage::Vertex, Shaders::Modules::ReflectionNortVS>(), MakeStageSource<ShaderStage::Fragment, Shaders::Modules::ReflectionNortPS>(),
        {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
    if (!res) {
        return res;
    }
    return BuildPassVariants(
        this, translucentReflectionPass, MakeStageSource<ShaderStage::Vertex, Shaders::Modules::ReflectionNortVS>(), MakeStageSource<ShaderStage::Fragment, Shaders::Modules::ReflectionNortPS>(),
        {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
}

auto RenderContext::Impl::BuildBlitPipeline() -> std::expected<void, ErrorCode> {
    return BuildPassHelper(
        this, blitPass, MakeStageSource<ShaderStage::Vertex, Shaders::Modules::BlitVS>(),
        MakeStageSource<ShaderStage::Fragment, Shaders::Modules::BlitPS>(), {presenter.GetPresentFormat()}
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

    auto defaultSamplerBuilder = Vk::SamplerBuilder {}.Linear().ClampToEdge();
    return std::expected<void, ErrorCode> {}
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return defaultSamplerBuilder.Build(ctx.Device()).and_then([&](auto defaultResult) -> auto {
                defaultSampler     = std::move(defaultResult);
                defaultSamplerInfo = defaultSamplerBuilder.Info();
                auto pointBuilder  = Vk::SamplerBuilder {}.Nearest().ClampToEdge();
                return pointBuilder.Build(ctx.Device()).transform([&](auto pointResult) -> auto {
                    pointSampler     = std::move(pointResult);
                    pointSamplerInfo = pointBuilder.Info();
                    WritePointSamplerToHeap(pointBuilder.Info());
                });
            });
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            auto passes = std::make_tuple(
                GraphicsPassDesc {
                    .pass        = taaPass,
                    .name        = "TAA",
                    .vs          = MakeStageSource<ShaderStage::Vertex, Shaders::Modules::TaaVS>(),
                    .ps          = MakeStageSource<ShaderStage::Fragment, Shaders::Modules::TaaPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = fxaaPass,
                    .name        = "FXAA",
                    .vs          = MakeStageSource<ShaderStage::Vertex, Shaders::Modules::FxaaVS>(),
                    .ps          = MakeStageSource<ShaderStage::Fragment, Shaders::Modules::FxaaPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = mlaaPass,
                    .name        = "MLAA",
                    .vs          = MakeStageSource<ShaderStage::Vertex, Shaders::Modules::MlaaVS>(),
                    .ps          = MakeStageSource<ShaderStage::Fragment, Shaders::Modules::MlaaPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = smaaEdgePass,
                    .name        = "SMAA Edge Detection",
                    .vs          = MakeStageSource<ShaderStage::Vertex, Shaders::Modules::SmaaEdgeVS>(),
                    .ps          = MakeStageSource<ShaderStage::Fragment, Shaders::Modules::SmaaEdgePS>(),
                    .colorFormat = VK_FORMAT_R8G8_UNORM
                },
                GraphicsPassDesc {
                    .pass        = smaaWeightPass,
                    .name        = "SMAA Blending Weight",
                    .vs          = MakeStageSource<ShaderStage::Vertex, Shaders::Modules::SmaaWeightVS>(),
                    .ps          = MakeStageSource<ShaderStage::Fragment, Shaders::Modules::SmaaWeightPS>(),
                    .colorFormat = VK_FORMAT_R8G8B8A8_UNORM
                },
                GraphicsPassDesc {
                    .pass        = smaaBlendPass,
                    .name        = "SMAA Neighborhood Blend",
                    .vs          = MakeStageSource<ShaderStage::Vertex, Shaders::Modules::SmaaBlendVS>(),
                    .ps          = MakeStageSource<ShaderStage::Fragment, Shaders::Modules::SmaaBlendPS>(),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = blitPass,
                    .name        = "Blit",
                    .vs          = MakeStageSource<ShaderStage::Vertex, Shaders::Modules::BlitVS>(),
                    .ps          = MakeStageSource<ShaderStage::Fragment, Shaders::Modules::BlitPS>(),
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
                {Shaders::Modules::LightingVS::Path, Shaders::Modules::LightingPS::Path, Shaders::Modules::LightingNortVS::Path, Shaders::Modules::LightingNortPS::Path,
                 Shaders::Modules::ReflectionVS::Path, Shaders::Modules::ReflectionPS::Path, Shaders::Modules::ReflectionNortVS::Path, Shaders::Modules::ReflectionNortPS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Bloom", [this]() -> std::expected<void, ErrorCode> { return postProcess.Build(*this); },
                {Shaders::Modules::BloomThresholdCS::Path, Shaders::Modules::BloomDownCS::Path, Shaders::Modules::BloomUpCS::Path, Shaders::Modules::HdrDenoiseAtrousCS::Path}
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
                this, "Decals", [this]() -> std::expected<void, ErrorCode> { return BuildDecalPipeline(); }, {Shaders::Modules::DecalVS::Path, Shaders::Modules::DecalPS::Path}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> { return postProcess.BakeSMAALUTs(*this); })
        .and_then([&]() -> std::expected<void, ErrorCode> { return fog.InitializeNoise(*this); })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            InitPassSamplerDescriptors();
            return {};
        });
}

}
