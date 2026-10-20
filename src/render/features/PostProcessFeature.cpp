// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "features/PostProcessFeature.hpp"
#include "RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Log.hpp>

namespace ZHLN {

auto PostProcessFeature::BuildCompute(
    RenderContext::Impl& impl, Vk::DynamicComputePass& pass, Vk::ReflectedLayout& layout, Vk::HeapPassBindings& bindings, std::span<const uint8_t> spirv
) -> std::expected<void, ErrorCode> {
    using namespace ZHLN;
    const auto shader = Vk::CreateShaderDesc(spirv);
    if (!layout.Build(impl.ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    if (auto built =
            Vk::BuildHeapPassBindings(impl.heapManager, layout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, bindings);
        !built) {
        return std::unexpected(built.error());
    }
    Vk::HeapMappingArena mappingArena {impl.heapManager};
    return pass.BuildHeap(impl.ctx.Device(), shader, mappingArena.Map(bindings.mappings), bindings.indexPushOffset, &impl.pipelineBinaries);
}

// The typed twin of BuildCompute for Vk::TypedComputePass: the module is the
// template parameter, so the pipeline's workgroup size is a compile-time
// constant (Vk::LocalSizeOf) and no runtime SPIR-V reflection runs at all.
template <Vk::ShaderProgram Module, typename PushConstants>
auto BuildTypedCompute(
    RenderContext::Impl&                         impl,
    Vk::TypedComputePass<Module, PushConstants>& pass,
    Vk::ReflectedLayout&                         layout,
    Vk::HeapPassBindings&                        bindings
) -> std::expected<void, ErrorCode> {
    const auto shader = Vk::CreateShaderDesc<Module>();
    if (!layout.Build(impl.ctx.Device(), shader, VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }
    if (auto built =
            Vk::BuildHeapPassBindings(impl.heapManager, layout.sets[0], 0, GpuAbi::kScenePushLayout.heapIndexOffset, Vk::HeapLifecycle::Frame, bindings);
        !built) {
        return std::unexpected(built.error());
    }
    Vk::HeapMappingArena mappingArena {impl.heapManager};
    return pass.Init(impl.ctx.Device(), mappingArena.Map(bindings.mappings), bindings.indexPushOffset, &impl.pipelineBinaries);
}

auto PostProcessFeature::Build(RenderContext::Impl& impl) -> std::expected<void, ErrorCode> {
    return BuildCompute(impl, _bloomThresholdCS, _bloomThresholdCSLayout, _bloomThresholdHeapBindings, Shaders::Modules::BloomThresholdCS::Bytes())
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                return BuildCompute(impl, _bloomDownCS, _bloomDownCSLayout, _bloomDownHeapBindings, Shaders::Modules::BloomDownCS::Bytes());
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                return BuildCompute(impl, _bloomUpCS, _bloomUpCSLayout, _bloomUpHeapBindings, Shaders::Modules::BloomUpCS::Bytes());
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                return BuildCompute(impl, _hdrDenoiseCS, _hdrDenoiseCSLayout, _hdrDenoiseHeapBindings, Shaders::Modules::HdrDenoiseAtrousCS::Bytes());
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                if (!impl.ctx.RayTracingSupported()) {
                    return {};
                }
                return BuildTypedCompute(impl, _rtShadowCS, _rtShadowCSLayout, _rtShadowHeapBindings);
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                if (!impl.ctx.RayTracingSupported()) {
                    return {};
                }
                return BuildCompute(
                    impl, _shadowDenoiseCS, _shadowDenoiseCSLayout, _shadowDenoiseHeapBindings, Shaders::Modules::ShadowDenoiseAtrousCS::Bytes()
                );
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                if (!impl.ctx.RayTracingSupported()) {
                    return {};
                }
                return BuildTypedCompute(impl, _rtrHalfCS, _rtrHalfCSLayout, _rtrHalfHeapBindings);
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                if (!impl.ctx.RayTracingSupported()) {
                    return {};
                }
                return BuildTypedCompute(impl, _rtgiHalfCS, _rtgiHalfCSLayout, _rtgiHalfHeapBindings);
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                if (!impl.ctx.RayTracingSupported()) {
                    return {};
                }
                return BuildCompute(
                    impl, _rtgiDenoiseCS, _rtgiDenoiseCSLayout, _rtgiDenoiseHeapBindings, Shaders::Modules::RtgiDenoiseCS::Bytes()
                );
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                if (!impl.ctx.RayTracingSupported()) {
                    return {};
                }
                return BuildTypedCompute(impl, _ddgiTraceCS, _ddgiTraceCSLayout, _ddgiTraceHeapBindings);
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                if (!impl.ctx.RayTracingSupported()) {
                    return {};
                }
                return BuildTypedCompute(impl, _ddgiUpdateCS, _ddgiUpdateCSLayout, _ddgiUpdateHeapBindings);
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                return BuildCompute(
                    impl, _rtgiCompositeCS, _rtgiCompositeCSLayout, _rtgiCompositeHeapBindings, Shaders::Modules::RtgiCompositeCS::Bytes()
                );
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                return BuildTypedCompute(impl, _gtaoCS, _gtaoCSLayout, _gtaoHeapBindings);
            }
        );
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto PostProcessFeature::BakeSMAALUTs(RenderContext::Impl& impl) -> std::expected<void, ErrorCode> {
using SMAALUTPush = Slang::SMAALUTPush;

    const Vk::ShaderDesc shader = Vk::CreateShaderDesc<Shaders::Modules::SmaaLutCS>();
    Vk::HeapMappingArena mappingArena {impl.heapManager};
    return Vk::CreateHeapComputePass(
               impl.ctx.Device(), shader, mappingArena.Map(impl.bakeHeapBindings.mappings), impl.bakeHeapBindings.indexPushOffset, &impl.pipelineBinaries
           )
        .and_then([&](Vk::DynamicComputePass pass) -> std::expected<void, ErrorCode> {
            return impl.BakeComputeTexture2D<Shaders::Bake, Shaders::Modules::SmaaLutCS>(
                       pass, 160, 560, VK_FORMAT_R8G8B8A8_UNORM, SMAALUTPush {.width = 160, .height = 560, .mode = 0}
            )
                .and_then([&](uint32_t areaIdx) -> std::expected<uint32_t, ErrorCode> {
                    _smaaAreaTexIdx = areaIdx;
                    return impl.BakeComputeTexture2D<Shaders::Bake, Shaders::Modules::SmaaLutCS>(
                        pass, 64, 16, VK_FORMAT_R8G8B8A8_UNORM, SMAALUTPush {.width = 64, .height = 16, .mode = 1}
                    );
                })
                .transform([&](uint32_t searchIdx) -> void {
                    _smaaSearchTexIdx = searchIdx;
                    ZHLN::Log("[SMAA] Area and search LUTs baked on GPU.");
                });
        });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

void PostProcessFeature::InitSamplers(RenderContext::Impl& impl) noexcept {
    const Vk::SamplerConfig defaultConfig   = impl.defaultSamplerConfig;
    const Vk::SamplerConfig pointConfig     = impl.pointSamplerConfig;
    const Vk::SamplerConfig blueNoiseConfig = impl.blueNoiseSamplerConfig;
    const Vk::SamplerConfig clampConfig     = impl.clampSamplerConfig;

    Vk::InitHeapPassSamplers<Shaders::Gtao>(impl.heapManager, _gtaoHeapBindings, Vk::SamplerSlot<"pointSampler">(pointConfig));
    Vk::InitHeapPassSamplers<Shaders::BloomThreshold>(impl.heapManager, _bloomThresholdHeapBindings, Vk::SamplerSlot<"smp">(defaultConfig));
    Vk::InitHeapPassSamplers<Shaders::BloomDown>(impl.heapManager, _bloomDownHeapBindings, Vk::SamplerSlot<"smp">(defaultConfig));
    Vk::InitHeapPassSamplers<Shaders::BloomUp>(impl.heapManager, _bloomUpHeapBindings, Vk::SamplerSlot<"smp">(defaultConfig));
    if (impl.ctx.RayTracingSupported()) {
        Vk::InitHeapPassSamplers<Shaders::RtShadow>(
            impl.heapManager, _rtShadowHeapBindings, Vk::SamplerSlot<"blueNoiseSampler">(blueNoiseConfig)
        );
        Vk::InitHeapPassSamplers<Shaders::RtrHalf>(
            impl.heapManager, _rtrHalfHeapBindings, Vk::SamplerSlot<"smp">(defaultConfig), Vk::SamplerSlot<"pointSampler">(pointConfig),
            Vk::SamplerSlot<"blueNoiseSampler">(blueNoiseConfig)
        );
        Vk::InitHeapPassSamplers<Shaders::RtgiHalf>(
            impl.heapManager, _rtgiHalfHeapBindings, Vk::SamplerSlot<"blueNoiseSampler">(blueNoiseConfig), Vk::SamplerSlot<"clampSampler">(clampConfig)
        );
        Vk::InitHeapPassSamplers<Shaders::DdgiTrace>(
            impl.heapManager, _ddgiTraceHeapBindings, Vk::SamplerSlot<"clampSampler">(clampConfig)
        );
    }
}

auto PostProcessFeature::DdgiTrace() noexcept -> Vk::TypedComputePass<Shaders::Modules::DdgiTraceCS, DdgiTracePushConstants>& {
    return _ddgiTraceCS;
}

auto PostProcessFeature::DdgiUpdate() noexcept -> Vk::TypedComputePass<Shaders::Modules::DdgiUpdateIrradianceCS, DdgiUpdatePushConstants>& {
    return _ddgiUpdateCS;
}

auto PostProcessFeature::DdgiTraceHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _ddgiTraceHeapBindings;
}

auto PostProcessFeature::DdgiUpdateHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _ddgiUpdateHeapBindings;
}

auto PostProcessFeature::RtShadow() noexcept -> Vk::TypedComputePass<Shaders::Modules::RtShadowCS, RTShadowPushConstants>& {
    return _rtShadowCS;
}

auto PostProcessFeature::ShadowDenoise() noexcept -> Vk::DynamicComputePass& {
    return _shadowDenoiseCS;
}

auto PostProcessFeature::RtShadowHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _rtShadowHeapBindings;
}

auto PostProcessFeature::ShadowDenoiseHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _shadowDenoiseHeapBindings;
}

auto PostProcessFeature::BloomThreshold() noexcept -> Vk::DynamicComputePass& {
    return _bloomThresholdCS;
}

auto PostProcessFeature::BloomDown() noexcept -> Vk::DynamicComputePass& {
    return _bloomDownCS;
}

auto PostProcessFeature::BloomUp() noexcept -> Vk::DynamicComputePass& {
    return _bloomUpCS;
}

auto PostProcessFeature::HdrDenoise() noexcept -> Vk::DynamicComputePass& {
    return _hdrDenoiseCS;
}

auto PostProcessFeature::Gtao() noexcept -> Vk::TypedComputePass<Shaders::Modules::GtaoCS, GtaoPushConstants>& {
    return _gtaoCS;
}

auto PostProcessFeature::RtrHalf() noexcept -> Vk::TypedComputePass<Shaders::Modules::RtrHalfCS, RtrHalfPushConstants>& {
    return _rtrHalfCS;
}

auto PostProcessFeature::RtgiHalf() noexcept -> Vk::TypedComputePass<Shaders::Modules::RtgiHalfCS, RtgiHalfPushConstants>& {
    return _rtgiHalfCS;
}

auto PostProcessFeature::RtgiDenoise() noexcept -> Vk::DynamicComputePass& {
    return _rtgiDenoiseCS;
}

auto PostProcessFeature::RtgiComposite() noexcept -> Vk::DynamicComputePass& {
    return _rtgiCompositeCS;
}

auto PostProcessFeature::BloomThresholdHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _bloomThresholdHeapBindings;
}

auto PostProcessFeature::BloomDownHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _bloomDownHeapBindings;
}

auto PostProcessFeature::BloomUpHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _bloomUpHeapBindings;
}

auto PostProcessFeature::HdrDenoiseHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _hdrDenoiseHeapBindings;
}

auto PostProcessFeature::GtaoHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _gtaoHeapBindings;
}

auto PostProcessFeature::RtrHalfHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _rtrHalfHeapBindings;
}

auto PostProcessFeature::RtgiHalfHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _rtgiHalfHeapBindings;
}

auto PostProcessFeature::RtgiDenoiseHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _rtgiDenoiseHeapBindings;
}

auto PostProcessFeature::RtgiCompositeHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _rtgiCompositeHeapBindings;
}

auto PostProcessFeature::SmaaAreaTexture() const noexcept -> uint32_t {
    return _smaaAreaTexIdx;
}

auto PostProcessFeature::SmaaSearchTexture() const noexcept -> uint32_t {
    return _smaaSearchTexIdx;
}

}
