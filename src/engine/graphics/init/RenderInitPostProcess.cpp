// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/graphics/init/RenderInitPostProcess.cpp
#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include "PassDescriptors.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cstddef>
#include <tuple>

namespace ZHLN {

auto RenderContext::Impl::BuildTAAPipeline() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, taaPass, "TAA", {.path = Resource::Paths::TaaVS, .fallback = Resource::GetShaderProgram(Taa).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::TaaPS, .fallback = Resource::GetShaderProgram(Taa).fragment, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildFXAAPipeline() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, fxaaPass, "FXAA", {.path = Resource::Paths::FxaaVS, .fallback = Resource::GetShaderProgram(Fxaa).vertex},
        {.path = Resource::Paths::FxaaPS, .fallback = Resource::GetShaderProgram(Fxaa).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildMLAAPipeline() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, mlaaPass, "MLAA", {.path = Resource::Paths::MlaaVS, .fallback = Resource::GetShaderProgram(Mlaa).vertex},
        {.path = Resource::Paths::MlaaPS, .fallback = Resource::GetShaderProgram(Mlaa).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildSMAAPipeline() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    return BuildPassHelper(
               this, smaaEdgePass, "SMAA Edge Detection", {.path = Resource::Paths::SmaaEdgeVS, .fallback = Resource::GetShaderProgram(SmaaEdge).vertex},
               {.path = Resource::Paths::SmaaEdgePS, .fallback = Resource::GetShaderProgram(SmaaEdge).fragment}, {VK_FORMAT_R8G8_UNORM}
    )
        .and_then([&]() -> std::expected<void, Error> {
            return BuildPassHelper(
                this, smaaWeightPass, "SMAA Blending Weight",
                {.path = Resource::Paths::SmaaWeightVS, .fallback = Resource::GetShaderProgram(SmaaWeight).vertex},
                {.path = Resource::Paths::SmaaWeightPS, .fallback = Resource::GetShaderProgram(SmaaWeight).fragment}, {VK_FORMAT_R8G8B8A8_UNORM}
            );
        })
        .and_then([&]() -> std::expected<void, Error> {
            return BuildPassHelper(
                this, smaaBlendPass, "SMAA Neighborhood Blend",
                {.path = Resource::Paths::SmaaBlendVS, .fallback = Resource::GetShaderProgram(SmaaBlend).vertex},
                {.path = Resource::Paths::SmaaBlendPS, .fallback = Resource::GetShaderProgram(SmaaBlend).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
            );
        });
}

auto RenderContext::Impl::BuildAmbientPipeline() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, ambientPass, "Ambient", {.path = Resource::Paths::AmbientVS, .fallback = Resource::GetShaderProgram(Ambient).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::AmbientPS, .fallback = Resource::GetShaderProgram(Ambient).fragment, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildLightingPipeline() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    struct SpecData {
        int enableRTR;
    };
    std::array<VkSpecializationMapEntry, 1> specEntries = {{{.constantID = 0, .offset = offsetof(SpecData, enableRTR), .size = sizeof(int)}}};

    std::array<SpecData, 2>             variants = {{{.enableRTR = 0}, {.enableRTR = 1}}};
    std::array<VkSpecializationInfo, 2> specInfos {};
    for (int i = 0; i < 2; ++i) {
        specInfos[i] = {.mapEntryCount = 1, .pMapEntries = specEntries.data(), .dataSize = sizeof(SpecData), .pData = &variants[i]};
    }

    bool        hasRt  = rtCtx.Valid();
    const char* vsPath = hasRt ? Resource::Paths::LightingVS : Resource::Paths::LightingNortVS;
    const char* psPath = hasRt ? Resource::Paths::LightingPS : Resource::Paths::LightingNortPS;

    auto vsSpan = hasRt ? Resource::GetShaderProgram(Lighting).vertex : Resource::GetShaderProgram(LightingNort).vertex;
    auto psSpan = hasRt ? Resource::GetShaderProgram(Lighting).fragment : Resource::GetShaderProgram(LightingNort).fragment;

    return BuildPassVariants(
        this, lightingPass, "Lighting", {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
}

auto RenderContext::Impl::BuildReflectionPipelines() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    struct SpecData {
        int enableSSR;
        int enableRTR;
    };
    std::array<VkSpecializationMapEntry, 2> specEntries = {
        {{.constantID = 0, .offset = offsetof(SpecData, enableSSR), .size = sizeof(int)},
         {.constantID = 1, .offset = offsetof(SpecData, enableRTR), .size = sizeof(int)}}
    };

    std::array<SpecData, 4> variants = {
        {{.enableSSR = 0, .enableRTR = 0}, {.enableSSR = 1, .enableRTR = 0}, {.enableSSR = 0, .enableRTR = 1}, {.enableSSR = 1, .enableRTR = 1}}
    };
    std::array<VkSpecializationInfo, 4> specInfos {};
    for (int i = 0; i < 4; ++i) {
        specInfos[i] = {.mapEntryCount = 2, .pMapEntries = specEntries.data(), .dataSize = sizeof(SpecData), .pData = &variants[i]};
    }

    bool        hasRt  = rtCtx.Valid();
    const char* vsPath = hasRt ? Resource::Paths::ReflectionVS : Resource::Paths::ReflectionNortVS;
    const char* psPath = hasRt ? Resource::Paths::ReflectionPS : Resource::Paths::ReflectionNortPS;

    auto vsSpan = hasRt ? Resource::GetShaderProgram(Reflection).vertex : Resource::GetShaderProgram(Resource::ShaderID::ReflectionNort).vertex;
    auto psSpan = hasRt ? Resource::GetShaderProgram(Reflection).fragment : Resource::GetShaderProgram(Resource::ShaderID::ReflectionNort).fragment;

    auto res = BuildPassVariants(
        this, reflectionPass, "Reflection", {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
    if (!res) {
        return res;
    }

    return BuildPassVariants(
        this, translucentReflectionPass, "Translucent Reflection", {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
}

auto RenderContext::Impl::BuildBloomPipelines() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    auto res = BuildPassHelper(
        this, bloomThresholdPass, "Bloom Threshold", {.path = Resource::Paths::BloomThresholdVS, .fallback = Resource::GetShaderProgram(BloomThreshold).vertex},
        {.path = Resource::Paths::BloomThresholdPS, .fallback = Resource::GetShaderProgram(BloomThreshold).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );

    for (int i = 0; i < 3; ++i) {
        res = res.and_then(
                     [&, i]() -> std::expected<void, Error> {
                         std::string downName = std::format("Bloom Downsample {}", i);
                         return BuildPassHelper(
                             this, bloomDownPass[i], downName.c_str(),
                             {.path = Resource::Paths::BloomBlurVS, .fallback = Resource::GetShaderProgram(BloomBlur).vertex},
                             {.path = Resource::Paths::BloomBlurPS, .fallback = Resource::GetShaderProgram(BloomBlur).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
                         );
                     }
        ).and_then([&, i]() -> std::expected<void, Error> {
            std::string upName = std::format("Bloom Upsample {}", i);
            return BuildPassHelper(
                this, bloomUpPass[i], upName.c_str(), {.path = Resource::Paths::BloomBlurVS, .fallback = Resource::GetShaderProgram(BloomBlur).vertex},
                {.path = Resource::Paths::BloomBlurPS, .fallback = Resource::GetShaderProgram(BloomBlur).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
            );
        });
    }

    return res;
}

auto RenderContext::Impl::BuildBlitPipeline() -> std::expected<void, Error> {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, blitPass, "Blit", {.path = Resource::Paths::BlitVS, .fallback = Resource::GetShaderProgram(Blit).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::BlitPS, .fallback = Resource::GetShaderProgram(Blit).fragment, .entryPoint = "PSMain"}, {presentation.GetPresentFormat()}
    );
}

auto RenderContext::Impl::BuildSpecializedLightingPipelines() -> std::expected<void, Error> {
    return BuildLightingPipeline().and_then([&]() -> std::expected<void, Error> { return BuildReflectionPipelines(); });
}

auto RenderContext::Impl::BuildVolumetricPipelines() -> std::expected<void, Error> {
    auto csClear = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricClear).vertex);
    if (!volumetricClearPass.BuildHeap(ctx.Device(), heapManager, csClear, heapPushDataLayout.heapIndexOffset)) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    auto csFogInject = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricFogInject).vertex);
    if (!volumetricFogInjectPass.BuildHeap(ctx.Device(), heapManager, csFogInject, heapPushDataLayout.heapIndexOffset)) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    auto csLightInject = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricLightInject).vertex);
    if (!volumetricLightInjectPass.BuildHeap(ctx.Device(), heapManager, csLightInject, heapPushDataLayout.heapIndexOffset)) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    auto csIntegrate = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricIntegration).vertex);
    if (!volumetricIntegrationPass.BuildHeap(ctx.Device(), heapManager, csIntegrate, heapPushDataLayout.heapIndexOffset)) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    auto csTemporal = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricTemporal).vertex);
    if (!volumetricTemporalPass.BuildHeap(ctx.Device(), heapManager, csTemporal, heapPushDataLayout.heapIndexOffset)) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }

    return {};
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto RenderContext::Impl::BakeSMAALUTs() -> std::expected<void, Error> {
    struct SMAALUTPush {
        uint32_t width  = 0;
        uint32_t height = 0;
        uint32_t mode   = 0;
    };
    const ZHLN_ShaderDesc shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::SMAALUTComp).vertex, "CSMain");
    return Vk::CreateHeapComputePass(ctx.Device(), shader, bakeHeapBindings.GetInfo(), bakeHeapBindings.indexPushOffset)
        .and_then([&](Vk::ComputePass pass) -> std::expected<void, Error> {
            return BakeComputeTexture2D(pass, 160, 560, VK_FORMAT_R8G8B8A8_UNORM, SMAALUTPush {.width = 160, .height = 560, .mode = 0})
                .and_then([&](uint32_t areaIdx) -> std::expected<uint32_t, Error> {
                    smaaAreaTexIdx = areaIdx;
                    return BakeComputeTexture2D(pass, 64, 16, VK_FORMAT_R8G8B8A8_UNORM, SMAALUTPush {.width = 64, .height = 16, .mode = 1});
                })
                .transform([&](uint32_t searchIdx) -> void {
                    smaaSearchTexIdx = searchIdx;
                    ZHLN::Log("[SMAA] Area and search LUTs baked on GPU.");
                });
        });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

auto RenderContext::Impl::InitPostProcessing() -> std::expected<void, Error> {
    using enum Resource::ShaderID;
    using Detail::MakeStageSource;

    auto defaultSamplerBuilder = Vk::SamplerBuilder {}.Linear().ClampToEdge();
    return std::expected<void, Error> {}
        .and_then([&]() -> std::expected<void, Error> {
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
        .and_then([&]() -> std::expected<void, Error> {
            auto passes = std::make_tuple(
                GraphicsPassDesc {
                    .pass        = taaPass,
                    .name        = "TAA",
                    .vs          = MakeStageSource<ShaderStage::Vertex>(Resource::Paths::TaaVS, Resource::GetShaderProgram(Taa).vertex, "VSMain"),
                    .ps          = MakeStageSource<ShaderStage::Fragment>(Resource::Paths::TaaPS, Resource::GetShaderProgram(Taa).fragment, "PSMain"),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = fxaaPass,
                    .name        = "FXAA",
                    .vs          = MakeStageSource<ShaderStage::Vertex>(Resource::Paths::FxaaVS, Resource::GetShaderProgram(Fxaa).vertex),
                    .ps          = MakeStageSource<ShaderStage::Fragment>(Resource::Paths::FxaaPS, Resource::GetShaderProgram(Fxaa).fragment),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = mlaaPass,
                    .name        = "MLAA",
                    .vs          = MakeStageSource<ShaderStage::Vertex>(Resource::Paths::MlaaVS, Resource::GetShaderProgram(Mlaa).vertex),
                    .ps          = MakeStageSource<ShaderStage::Fragment>(Resource::Paths::MlaaPS, Resource::GetShaderProgram(Mlaa).fragment),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = smaaEdgePass,
                    .name        = "SMAA Edge Detection",
                    .vs          = MakeStageSource<ShaderStage::Vertex>(Resource::Paths::SmaaEdgeVS, Resource::GetShaderProgram(SmaaEdge).vertex),
                    .ps          = MakeStageSource<ShaderStage::Fragment>(Resource::Paths::SmaaEdgePS, Resource::GetShaderProgram(SmaaEdge).fragment),
                    .colorFormat = VK_FORMAT_R8G8_UNORM
                },
                GraphicsPassDesc {
                    .pass        = smaaWeightPass,
                    .name        = "SMAA Blending Weight",
                    .vs          = MakeStageSource<ShaderStage::Vertex>(Resource::Paths::SmaaWeightVS, Resource::GetShaderProgram(SmaaWeight).vertex),
                    .ps          = MakeStageSource<ShaderStage::Fragment>(Resource::Paths::SmaaWeightPS, Resource::GetShaderProgram(SmaaWeight).fragment),
                    .colorFormat = VK_FORMAT_R8G8B8A8_UNORM
                },
                GraphicsPassDesc {
                    .pass        = smaaBlendPass,
                    .name        = "SMAA Neighborhood Blend",
                    .vs          = MakeStageSource<ShaderStage::Vertex>(Resource::Paths::SmaaBlendVS, Resource::GetShaderProgram(SmaaBlend).vertex),
                    .ps          = MakeStageSource<ShaderStage::Fragment>(Resource::Paths::SmaaBlendPS, Resource::GetShaderProgram(SmaaBlend).fragment),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = ambientPass,
                    .name        = "Ambient",
                    .vs          = MakeStageSource<ShaderStage::Vertex>(Resource::Paths::AmbientVS, Resource::GetShaderProgram(Ambient).vertex, "VSMain"),
                    .ps          = MakeStageSource<ShaderStage::Fragment>(Resource::Paths::AmbientPS, Resource::GetShaderProgram(Ambient).fragment, "PSMain"),
                    .colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    .pass        = blitPass,
                    .name        = "Blit",
                    .vs          = MakeStageSource<ShaderStage::Vertex>(Resource::Paths::BlitVS, Resource::GetShaderProgram(Blit).vertex, "VSMain"),
                    .ps          = MakeStageSource<ShaderStage::Fragment>(Resource::Paths::BlitPS, Resource::GetShaderProgram(Blit).fragment, "PSMain"),
                    .colorFormat = presentation.GetPresentFormat()
                }
            );
            return std::apply(
                [this](auto&&... descs) -> std::expected<void, Error> {
                    std::expected<void, Error> fold {};
                    ((fold = fold.and_then([this, &descs]() -> std::expected<void, Error> { return BuildDescribedPass(this, descs); })), ...);
                    return fold;
                },
                passes
            );
        })
        .and_then([&]() -> std::expected<void, Error> {
            return RegisterAndBuild(
                this, "Lighting", [this]() -> std::expected<void, Error> { return BuildSpecializedLightingPipelines(); },
                {Resource::Paths::LightingVS, Resource::Paths::LightingPS, Resource::Paths::LightingNortVS, Resource::Paths::LightingNortPS,
                 Resource::Paths::ReflectionVS, Resource::Paths::ReflectionPS, Resource::Paths::ReflectionNortVS, Resource::Paths::ReflectionNortPS}
            );
        })
        .and_then([&]() -> std::expected<void, Error> {
            return RegisterAndBuild(
                this, "Bloom", [this]() -> std::expected<void, Error> { return BuildBloomPipelines(); },
                {Resource::Paths::BloomThresholdVS, Resource::Paths::BloomThresholdPS, Resource::Paths::BloomBlurVS, Resource::Paths::BloomBlurPS}
            );
        })
        .and_then([&]() -> std::expected<void, Error> {
            return RegisterAndBuild(
                this, "Volumetrics", [this]() -> std::expected<void, Error> { return BuildVolumetricPipelines(); },
                {Resource::Paths::VolumetricClearCS, Resource::Paths::VolumetricFogInjectCS, Resource::Paths::VolumetricLightInjectCS,
                 Resource::Paths::VolumetricIntegrationCS, Resource::Paths::VolumetricTemporalCS}
            );
        })
        .and_then([&]() -> std::expected<void, Error> {
            return RegisterAndBuild(
                this, "Particles", [this]() -> std::expected<void, Error> { return BuildParticlePipelines(); },
                {Resource::Paths::ParticleUpdateCS, Resource::Paths::ParticleRenderVS, Resource::Paths::ParticleRenderPS}
            );
        })
        .and_then([&]() -> std::expected<void, Error> {
            return RegisterAndBuild(
                this, "3D Mesh Particles", [this]() -> std::expected<void, Error> { return BuildMeshParticlePipelines(); },
                {Resource::Paths::MeshParticleUpdateCS, Resource::Paths::MeshParticleRenderVS, Resource::Paths::MeshParticleRenderPS,
                 Resource::Paths::MeshParticleShadowVS}
            );
        })
        .and_then([&]() -> std::expected<void, Error> {
            return RegisterAndBuild(
                this, "Decals", [this]() -> std::expected<void, Error> { return BuildDecalPipeline(); }, {Resource::Paths::DecalVS, Resource::Paths::DecalPS}
            );
        })
        .and_then([&]() -> std::expected<void, Error> { return BakeSMAALUTs(); })
        .and_then([&]() -> std::expected<void, Error> { return InitializeVolumetricNoiseTexture(); })
        .and_then([&]() -> std::expected<void, Error> {
            InitPassSamplerDescriptors();
            WriteVolumetricNoiseDescriptor();
            return {};
        });
}

} // namespace ZHLN
