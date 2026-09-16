// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/init/RenderInitPostProcess.cpp
#include "../RenderInternal.hpp"
#include "../Resources.hpp"
#include "PassDescriptors.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <cstddef>
#include <tuple>

namespace ZHLN {

auto RenderContext::Impl::BuildTAAPipeline() -> std::expected<void, ErrorCode> {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, taaPass, {.path = Resource::Paths::TaaVS, .fallback = Resource::GetShaderProgram(Taa).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::TaaPS, .fallback = Resource::GetShaderProgram(Taa).fragment, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildFXAAPipeline() -> std::expected<void, ErrorCode> {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, fxaaPass, {.path = Resource::Paths::FxaaVS, .fallback = Resource::GetShaderProgram(Fxaa).vertex},
        {.path = Resource::Paths::FxaaPS, .fallback = Resource::GetShaderProgram(Fxaa).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildMLAAPipeline() -> std::expected<void, ErrorCode> {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, mlaaPass, {.path = Resource::Paths::MlaaVS, .fallback = Resource::GetShaderProgram(Mlaa).vertex},
        {.path = Resource::Paths::MlaaPS, .fallback = Resource::GetShaderProgram(Mlaa).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
    );
}

auto RenderContext::Impl::BuildSMAAPipeline() -> std::expected<void, ErrorCode> {
    using enum Resource::ShaderID;

    return BuildPassHelper(
               this, smaaEdgePass, {.path = Resource::Paths::SmaaEdgeVS, .fallback = Resource::GetShaderProgram(SmaaEdge).vertex},
               {.path = Resource::Paths::SmaaEdgePS, .fallback = Resource::GetShaderProgram(SmaaEdge).fragment}, {VK_FORMAT_R8G8_UNORM}
    )
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BuildPassHelper(
                this, smaaWeightPass,
                {.path = Resource::Paths::SmaaWeightVS, .fallback = Resource::GetShaderProgram(SmaaWeight).vertex},
                {.path = Resource::Paths::SmaaWeightPS, .fallback = Resource::GetShaderProgram(SmaaWeight).fragment}, {VK_FORMAT_R8G8B8A8_UNORM}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BuildPassHelper(
                this, smaaBlendPass,
                {.path = Resource::Paths::SmaaBlendVS, .fallback = Resource::GetShaderProgram(SmaaBlend).vertex},
                {.path = Resource::Paths::SmaaBlendPS, .fallback = Resource::GetShaderProgram(SmaaBlend).fragment}, {VK_FORMAT_R16G16B16A16_SFLOAT}
            );
        });
}

auto RenderContext::Impl::BuildLightingPipeline() -> std::expected<void, ErrorCode> {
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
        this, lightingPass, {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
}

auto RenderContext::Impl::BuildReflectionPipelines() -> std::expected<void, ErrorCode> {
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
        this, reflectionPass, {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
    if (!res) {
        return res;
    }

    return BuildPassVariants(
        this, translucentReflectionPass, {.path = vsPath, .fallback = vsSpan, .entryPoint = "VSMain"},
        {.path = psPath, .fallback = psSpan, .entryPoint = "PSMain"}, {VK_FORMAT_R16G16B16A16_SFLOAT}, specInfos
    );
}

auto RenderContext::Impl::BuildBloomPipelines() -> std::expected<void, ErrorCode> {
    using enum Resource::ShaderID;

    // Dual Kawase bloom as a single compute dispatch chain. Layout authority
    // lives in each compiled module: reflect set 0, bake the PUSH_INDEX
    // mapping, and build three null-layout heap pipelines (threshold / down /
    // up). Every dispatch of a chain allocates its own block from the frame
    // partition, so the binding table carries no per-dispatch count.
    const auto buildCompute = [&](Vk::DynamicComputePass& pass, Vk::ReflectedLayout& layout, Vk::HeapPassBindings& bindings,
                                  std::span<const uint8_t> spirv) -> std::expected<void, ErrorCode> {
        const auto shader = Vk::CreateShaderDesc(spirv);
        if (!layout.Build(ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
            return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
        }
        if (auto built = Vk::BuildHeapPassBindings(
                heapManager, layout.sets[0], 0, heapPushDataLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, bindings
            );
            !built) {
            return std::unexpected(built.error());
        }
        return pass.BuildHeap(ctx.Device(), shader, bindings.GetInfo(), bindings.indexPushOffset, pipelineCache.Get());
    };

    return buildCompute(bloomThresholdCS, bloomThresholdCSLayout, bloomThresholdHeapBindings, Resource::bloom_threshold_cs)
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return buildCompute(bloomDownCS, bloomDownCSLayout, bloomDownHeapBindings, Resource::bloom_down_cs);
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return buildCompute(bloomUpCS, bloomUpCSLayout, bloomUpHeapBindings, Resource::bloom_up_cs);
        })
        // HDR scene A-Trous wavelet denoiser: one pipeline reused for every
        // iteration; tap spacing and edge-stops arrive as push constants and
        // the source/destination swap through the shared heap binding table
        // (3 iterations x 2 parity frames).
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return buildCompute(hdrDenoiseCS, hdrDenoiseCSLayout, hdrDenoiseHeapBindings, Resource::hdr_denoise_atrous_cs, 6);
        })
        // Half-resolution RTR band tracer: one dispatch per frame, so the
        // variant count is just the frame parity. The shader binds an
        // acceleration structure, so the pipeline is only built when the RT
        // context exists.
        .and_then([&]() -> std::expected<void, ErrorCode> {
            if (!rtCtx.Valid()) {
                return {};
            }
            return buildCompute(rtrHalfCS, rtrHalfCSLayout, rtrHalfHeapBindings, Resource::rtr_half_cs, 2);
        })
        // Half-resolution GTAO occlusion: one dispatch per frame (the variant
        // count is the frame parity). Built unconditionally -- the pass is
        // mode-gated at record time, not at init time.
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return buildCompute(gtaoCS, gtaoCSLayout, gtaoHeapBindings, Resource::ao_gtao_cs, 2);
        });
}

auto RenderContext::Impl::BuildBlitPipeline() -> std::expected<void, ErrorCode> {
    using enum Resource::ShaderID;

    return BuildPassHelper(
        this, blitPass, {.path = Resource::Paths::BlitVS, .fallback = Resource::GetShaderProgram(Blit).vertex, .entryPoint = "VSMain"},
        {.path = Resource::Paths::BlitPS, .fallback = Resource::GetShaderProgram(Blit).fragment, .entryPoint = "PSMain"}, {session.presentation.GetPresentFormat()}
    );
}

auto RenderContext::Impl::BuildSpecializedLightingPipelines() -> std::expected<void, ErrorCode> {
    return BuildLightingPipeline().and_then([&]() -> std::expected<void, ErrorCode> { return BuildReflectionPipelines(); });
}

auto RenderContext::Impl::BuildVolumetricPipelines() -> std::expected<void, ErrorCode> {
    auto csClear = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricClear).vertex);
    if (!volumetricClearPass.BuildHeap(ctx.Device(), heapManager, csClear, heapPushDataLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, pipelineCache.Get())) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    auto csFogInject = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricFogInject).vertex);
    if (!volumetricFogInjectPass.BuildHeap(ctx.Device(), heapManager, csFogInject, heapPushDataLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, pipelineCache.Get())) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    auto csLightInject = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricLightInject).vertex);
    if (!volumetricLightInjectPass.BuildHeap(ctx.Device(), heapManager, csLightInject, heapPushDataLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, pipelineCache.Get())) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    auto csIntegrate = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricIntegration).vertex);
    if (!volumetricIntegrationPass.BuildHeap(ctx.Device(), heapManager, csIntegrate, heapPushDataLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, pipelineCache.Get())) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    auto csTemporal = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::VolumetricTemporal).vertex);
    if (!volumetricTemporalPass.BuildHeap(ctx.Device(), heapManager, csTemporal, heapPushDataLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, pipelineCache.Get())) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    return {};
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto RenderContext::Impl::BakeSMAALUTs() -> std::expected<void, ErrorCode> {
    struct SMAALUTPush {
        uint32_t width  = 0;
        uint32_t height = 0;
        uint32_t mode   = 0;
    };
    const ZHLN_ShaderDesc shader = Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::SMAALUTComp).vertex, "CSMain");
    return Vk::CreateHeapComputePass(ctx.Device(), shader, bakeHeapBindings.GetInfo(), bakeHeapBindings.indexPushOffset, pipelineCache.Get())
        .and_then([&](Vk::DynamicComputePass pass) -> std::expected<void, ErrorCode> {
            return BakeComputeTexture2D(pass, 160, 560, VK_FORMAT_R8G8B8A8_UNORM, SMAALUTPush {.width = 160, .height = 560, .mode = 0})
                .and_then([&](uint32_t areaIdx) -> std::expected<uint32_t, ErrorCode> {
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

auto RenderContext::Impl::InitPostProcessing() -> std::expected<void, ErrorCode> {
    using enum Resource::ShaderID;
    using TemplatedDetail::MakeStageSource;

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
                    .pass        = blitPass,
                    .name        = "Blit",
                    .vs          = MakeStageSource<ShaderStage::Vertex>(Resource::Paths::BlitVS, Resource::GetShaderProgram(Blit).vertex, "VSMain"),
                    .ps          = MakeStageSource<ShaderStage::Fragment>(Resource::Paths::BlitPS, Resource::GetShaderProgram(Blit).fragment, "PSMain"),
                    .colorFormat = session.presentation.GetPresentFormat()
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
                {Resource::Paths::LightingVS, Resource::Paths::LightingPS, Resource::Paths::LightingNortVS, Resource::Paths::LightingNortPS,
                 Resource::Paths::ReflectionVS, Resource::Paths::ReflectionPS, Resource::Paths::ReflectionNortVS, Resource::Paths::ReflectionNortPS}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Bloom", [this]() -> std::expected<void, ErrorCode> { return BuildBloomPipelines(); },
                {Resource::Paths::BloomThresholdCS, Resource::Paths::BloomDownCS, Resource::Paths::BloomUpCS, Resource::Paths::HdrDenoiseAtrousCS}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Volumetrics", [this]() -> std::expected<void, ErrorCode> { return BuildVolumetricPipelines(); },
                {Resource::Paths::VolumetricClearCS, Resource::Paths::VolumetricFogInjectCS, Resource::Paths::VolumetricLightInjectCS,
                 Resource::Paths::VolumetricIntegrationCS, Resource::Paths::VolumetricTemporalCS}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Particles", [this]() -> std::expected<void, ErrorCode> { return BuildParticlePipelines(); },
                {Resource::Paths::ParticleUpdateCS, Resource::Paths::ParticleRenderVS, Resource::Paths::ParticleRenderPS}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "3D Mesh Particles", [this]() -> std::expected<void, ErrorCode> { return BuildMeshParticlePipelines(); },
                {Resource::Paths::MeshParticleUpdateCS, Resource::Paths::MeshParticleRenderVS, Resource::Paths::MeshParticleRenderPS,
                 Resource::Paths::MeshParticleShadowVS}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return RegisterAndBuild(
                this, "Decals", [this]() -> std::expected<void, ErrorCode> { return BuildDecalPipeline(); }, {Resource::Paths::DecalVS, Resource::Paths::DecalPS}
            );
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return BakeSMAALUTs();
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            return InitializeVolumetricNoiseTexture();
        })
        .and_then([&]() -> std::expected<void, ErrorCode> {
            InitPassSamplerDescriptors();
            return {};
        });
}

} // namespace ZHLN
