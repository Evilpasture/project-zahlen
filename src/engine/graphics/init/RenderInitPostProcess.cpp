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

std::expected<void, Error> RenderContext::Impl::BuildTAAPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, taaPass, "TAA", {.path = Resource::Paths::TaaVS, .fallback = Resource::GetShaderProgram(Taa).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::TaaPS, .fallback = Resource::GetShaderProgram(Taa).fragment, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

std::expected<void, Error> RenderContext::Impl::BuildFXAAPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, fxaaPass, "FXAA", {.path = Resource::Paths::FxaaVS, .fallback = Resource::GetShaderProgram(Fxaa).vertex},
        {.path = Resource::Paths::FxaaPS, .fallback = Resource::GetShaderProgram(Fxaa).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

std::expected<void, Error> RenderContext::Impl::BuildMLAAPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, mlaaPass, "MLAA", {.path = Resource::Paths::MlaaVS, .fallback = Resource::GetShaderProgram(Mlaa).vertex},
        {.path = Resource::Paths::MlaaPS, .fallback = Resource::GetShaderProgram(Mlaa).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

std::expected<void, Error> RenderContext::Impl::BuildSMAAPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
               this, smaaEdgePass, "SMAA Edge Detection", {.path = Resource::Paths::SmaaEdgeVS, .fallback = Resource::GetShaderProgram(SmaaEdge).vertex},
               {.path = Resource::Paths::SmaaEdgePS, .fallback = Resource::GetShaderProgram(SmaaEdge).fragment}, {VK_FORMAT_R8G8_UNORM}
    )
        .and_then([&]() {
            return BuildPassHelper(
                this, smaaWeightPass, "SMAA Blending Weight",
                {.path = Resource::Paths::SmaaWeightVS, .fallback = Resource::GetShaderProgram(SmaaWeight).vertex},
                {.path = Resource::Paths::SmaaWeightPS, .fallback = Resource::GetShaderProgram(SmaaWeight).fragment}, {VK_FORMAT_R8G8B8A8_UNORM}
            );
        })
        .and_then([&]() {
            return BuildPassHelper(
                this, smaaBlendPass, "SMAA Neighborhood Blend",
                {.path = Resource::Paths::SmaaBlendVS, .fallback = Resource::GetShaderProgram(SmaaBlend).vertex},
                {.path = Resource::Paths::SmaaBlendPS, .fallback = Resource::GetShaderProgram(SmaaBlend).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
            );
        });
}

std::expected<void, Error> RenderContext::Impl::BuildAmbientPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, ambientPass, "Ambient", {.path = Resource::Paths::AmbientVS, .fallback = Resource::GetShaderProgram(Ambient).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::AmbientPS, .fallback = Resource::GetShaderProgram(Ambient).fragment, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

std::expected<void, Error> RenderContext::Impl::BuildLightingPipeline() {
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

std::expected<void, Error> RenderContext::Impl::BuildReflectionPipelines() {
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

std::expected<void, Error> RenderContext::Impl::BuildBloomPipelines() {
    using enum Resource::ShaderID;

    auto res = BuildPassHelper(
        this, bloomThresholdPass, "Bloom Threshold", {.path = Resource::Paths::BloomThresholdVS, .fallback = Resource::GetShaderProgram(BloomThreshold).vertex},
        {.path = Resource::Paths::BloomThresholdPS, .fallback = Resource::GetShaderProgram(BloomThreshold).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );

    for (int i = 0; i < 3; ++i) {
        res = res.and_then(
                     [&, i]() {
                         std::string downName = std::format("Bloom Downsample {}", i);
                         return BuildPassHelper(
                             this, bloomDownPass[i], downName.c_str(),
                             {.path = Resource::Paths::BloomBlurVS, .fallback = Resource::GetShaderProgram(BloomBlur).vertex},
                             {.path = Resource::Paths::BloomBlurPS, .fallback = Resource::GetShaderProgram(BloomBlur).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
                         );
                     }
        ).and_then([&, i]() {
            std::string upName = std::format("Bloom Upsample {}", i);
            return BuildPassHelper(
                this, bloomUpPass[i], upName.c_str(), {.path = Resource::Paths::BloomBlurVS, .fallback = Resource::GetShaderProgram(BloomBlur).vertex},
                {.path = Resource::Paths::BloomBlurPS, .fallback = Resource::GetShaderProgram(BloomBlur).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
            );
        });
    }

    return res;
}

std::expected<void, Error> RenderContext::Impl::BuildBlitPipeline() {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, blitPass, "Blit", {.path = Resource::Paths::BlitVS, .fallback = Resource::GetShaderProgram(Blit).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::BlitPS, .fallback = Resource::GetShaderProgram(Blit).fragment, .entryPoint = "PSMain"}, {presentation.GetPresentFormat()}
    );
}

std::expected<void, Error> RenderContext::Impl::BuildSpecializedLightingPipelines() {
    return BuildLightingPipeline().and_then([&]() { return BuildReflectionPipelines(); });
}

std::expected<void, Error> RenderContext::Impl::BuildVolumetricPipelines() {
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

std::expected<void, Error> RenderContext::Impl::BakeSMAALUTs() {
    struct SMAALUTPush {
        uint32_t width  = 0;
        uint32_t height = 0;
        uint32_t mode   = 0;
    };
    const ZHLN_ShaderDesc shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::SMAALUTComp).vertex, "CSMain");
    return Vk::CreateHeapComputePass(ctx.Device(), shader, bakeHeapBindings.GetInfo(), bakeHeapBindings.indexPushOffset)
        .and_then([&](Vk::ComputePass pass) -> std::expected<void, Error> {
            return BakeComputeTexture2D(pass, 160, 560, VK_FORMAT_R8G8B8A8_UNORM, SMAALUTPush {.width = 160, .height = 560, .mode = 0})
                .and_then([&](uint32_t areaIdx) {
                    smaaAreaTexIdx = areaIdx;
                    return BakeComputeTexture2D(pass, 64, 16, VK_FORMAT_R8G8B8A8_UNORM, SMAALUTPush {.width = 64, .height = 16, .mode = 1});
                })
                .transform([&](uint32_t searchIdx) {
                    smaaSearchTexIdx = searchIdx;
                    ZHLN::Log("[SMAA] Area and search LUTs baked on GPU.");
                });
        });
}

std::expected<void, Error> RenderContext::Impl::InitPostProcessing() {
    using enum Resource::ShaderID;
    using Detail::MakeStageSource;

    auto defaultSamplerBuilder = Vk::SamplerBuilder {}.Linear().ClampToEdge();
    return std::expected<void, Error> {}
        .and_then([&]() -> std::expected<void, Error> {
            return defaultSamplerBuilder.Build(ctx.Device())
                .and_then([&](auto defaultResult) -> std::expected<void, VkResult> {
                    defaultSampler     = std::move(defaultResult);
                    defaultSamplerInfo = defaultSamplerBuilder.Info();
                    auto pointBuilder  = Vk::SamplerBuilder {}.Nearest().ClampToEdge();
                    return pointBuilder.Build(ctx.Device()).transform([&](auto pointResult) {
                        pointSampler     = std::move(pointResult);
                        pointSamplerInfo = pointBuilder.Info();
                        WritePointSamplerToHeap(pointBuilder.Info());
                    });
                });
        })
        .and_then([&]() -> std::expected<void, Error> {
            auto passes = std::make_tuple(
                GraphicsPassDesc {
                    taaPass, "TAA", MakeStageSource<ShaderStage::Vertex>(Resource::Paths::TaaVS, Resource::GetShaderProgram(Taa).vertex, "VSMain"),
                    MakeStageSource<ShaderStage::Fragment>(Resource::Paths::TaaPS, Resource::GetShaderProgram(Taa).fragment, "PSMain"),
                    VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    fxaaPass, "FXAA", MakeStageSource<ShaderStage::Vertex>(Resource::Paths::FxaaVS, Resource::GetShaderProgram(Fxaa).vertex),
                    MakeStageSource<ShaderStage::Fragment>(Resource::Paths::FxaaPS, Resource::GetShaderProgram(Fxaa).fragment),
                    VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    mlaaPass, "MLAA", MakeStageSource<ShaderStage::Vertex>(Resource::Paths::MlaaVS, Resource::GetShaderProgram(Mlaa).vertex),
                    MakeStageSource<ShaderStage::Fragment>(Resource::Paths::MlaaPS, Resource::GetShaderProgram(Mlaa).fragment),
                    VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    smaaEdgePass, "SMAA Edge Detection",
                    MakeStageSource<ShaderStage::Vertex>(Resource::Paths::SmaaEdgeVS, Resource::GetShaderProgram(SmaaEdge).vertex),
                    MakeStageSource<ShaderStage::Fragment>(Resource::Paths::SmaaEdgePS, Resource::GetShaderProgram(SmaaEdge).fragment), VK_FORMAT_R8G8_UNORM
                },
                GraphicsPassDesc {
                    smaaWeightPass, "SMAA Blending Weight",
                    MakeStageSource<ShaderStage::Vertex>(Resource::Paths::SmaaWeightVS, Resource::GetShaderProgram(SmaaWeight).vertex),
                    MakeStageSource<ShaderStage::Fragment>(Resource::Paths::SmaaWeightPS, Resource::GetShaderProgram(SmaaWeight).fragment),
                    VK_FORMAT_R8G8B8A8_UNORM
                },
                GraphicsPassDesc {
                    smaaBlendPass, "SMAA Neighborhood Blend",
                    MakeStageSource<ShaderStage::Vertex>(Resource::Paths::SmaaBlendVS, Resource::GetShaderProgram(SmaaBlend).vertex),
                    MakeStageSource<ShaderStage::Fragment>(Resource::Paths::SmaaBlendPS, Resource::GetShaderProgram(SmaaBlend).fragment),
                    VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    ambientPass, "Ambient",
                    MakeStageSource<ShaderStage::Vertex>(Resource::Paths::AmbientVS, Resource::GetShaderProgram(Ambient).vertex, "VSMain"),
                    MakeStageSource<ShaderStage::Fragment>(Resource::Paths::AmbientPS, Resource::GetShaderProgram(Ambient).fragment, "PSMain"),
                    VK_FORMAT_R16G16B16A16_SFLOAT
                },
                GraphicsPassDesc {
                    blitPass, "Blit", MakeStageSource<ShaderStage::Vertex>(Resource::Paths::BlitVS, Resource::GetShaderProgram(Blit).vertex, "VSMain"),
                    MakeStageSource<ShaderStage::Fragment>(Resource::Paths::BlitPS, Resource::GetShaderProgram(Blit).fragment, "PSMain"),
                    presentation.GetPresentFormat()
                }
            );
            return std::apply(
                [this](auto&&... descs) -> std::expected<void, Error> {
                    std::expected<void, Error> fold {};
                    ((fold = fold.and_then([this, &descs]() { return BuildDescribedPass(this, descs); })), ...);
                    return fold;
                },
                passes
            );
        })
        .and_then([&]() {
            return RegisterAndBuild(
                this, "Lighting", [this]() { return BuildSpecializedLightingPipelines(); },
                {Resource::Paths::LightingVS, Resource::Paths::LightingPS, Resource::Paths::LightingNortVS, Resource::Paths::LightingNortPS,
                 Resource::Paths::ReflectionVS, Resource::Paths::ReflectionPS, Resource::Paths::ReflectionNortVS, Resource::Paths::ReflectionNortPS}
            );
        })
        .and_then([&]() {
            return RegisterAndBuild(
                this, "Bloom", [this]() { return BuildBloomPipelines(); },
                {Resource::Paths::BloomThresholdVS, Resource::Paths::BloomThresholdPS, Resource::Paths::BloomBlurVS, Resource::Paths::BloomBlurPS}
            );
        })
        .and_then([&]() {
            return RegisterAndBuild(
                this, "Volumetrics", [this]() { return BuildVolumetricPipelines(); },
                {Resource::Paths::VolumetricClearCS, Resource::Paths::VolumetricFogInjectCS, Resource::Paths::VolumetricLightInjectCS,
                 Resource::Paths::VolumetricIntegrationCS, Resource::Paths::VolumetricTemporalCS}
            );
        })
        .and_then([&]() {
            return RegisterAndBuild(
                this, "Particles", [this]() { return BuildParticlePipelines(); },
                {Resource::Paths::ParticleUpdateCS, Resource::Paths::ParticleRenderVS, Resource::Paths::ParticleRenderPS}
            );
        })
        .and_then([&]() {
            return RegisterAndBuild(
                this, "3D Mesh Particles", [this]() { return BuildMeshParticlePipelines(); },
                {Resource::Paths::MeshParticleUpdateCS, Resource::Paths::MeshParticleRenderVS, Resource::Paths::MeshParticleRenderPS,
                 Resource::Paths::MeshParticleShadowVS}
            );
        })
        .and_then([&]() {
            return RegisterAndBuild(this, "Decals", [this]() { return BuildDecalPipeline(); }, {Resource::Paths::DecalVS, Resource::Paths::DecalPS});
        })
        .and_then([&]() { return BakeSMAALUTs(); })
        .and_then([&]() -> std::expected<void, Error> {
            InitPassSamplerDescriptors();
            return {};
        });
}

} // namespace ZHLN
