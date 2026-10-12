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
    return pass.BuildHeap(impl.ctx.Device(), shader, bindings.GetInfo(), bindings.indexPushOffset, impl.pipelineCache.Get());
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
                return BuildCompute(impl, _rtShadowCS, _rtShadowCSLayout, _rtShadowHeapBindings, Shaders::Modules::RtShadowCS::Bytes());
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
                return BuildCompute(impl, _rtrHalfCS, _rtrHalfCSLayout, _rtrHalfHeapBindings, Shaders::Modules::RtrHalfCS::Bytes());
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                if (!impl.ctx.RayTracingSupported()) {
                    return {};
                }
                return BuildCompute(impl, _rtgiHalfCS, _rtgiHalfCSLayout, _rtgiHalfHeapBindings, Shaders::Modules::RtgiHalfCS::Bytes());
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
                return BuildCompute(
                    impl, _ddgiTraceCS, _ddgiTraceCSLayout, _ddgiTraceHeapBindings, Shaders::Modules::DdgiTraceCS::Bytes()
                );
            }
        )
        .and_then(
            [&]() -> std::expected<void, ErrorCode> {
                if (!impl.ctx.RayTracingSupported()) {
                    return {};
                }
                return BuildCompute(
                    impl, _ddgiUpdateCS, _ddgiUpdateCSLayout, _ddgiUpdateHeapBindings, Shaders::Modules::DdgiUpdateIrradianceCS::Bytes()
                );
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
                return BuildCompute(impl, _gtaoCS, _gtaoCSLayout, _gtaoHeapBindings, Shaders::Modules::GtaoCS::Bytes());
            }
        );
}

// GCC 16 sees a spurious uninitialized value in libstdc++'s
// expected::transform<void> when inlining this checked monadic chain.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto PostProcessFeature::BakeSMAALUTs(RenderContext::Impl& impl) -> std::expected<void, ErrorCode> {
    struct SMAALUTPush {
        uint32_t width  = 0;
        uint32_t height = 0;
        uint32_t mode   = 0;
    };

    const Vk::ShaderDesc shader = Vk::CreateShaderDesc<Shaders::Modules::SmaaLutCS>();
    return Vk::CreateHeapComputePass(impl.ctx.Device(), shader, impl.bakeHeapBindings.GetInfo(), impl.bakeHeapBindings.indexPushOffset, impl.pipelineCache.Get())
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

auto PostProcessFeature::DdgiTrace() noexcept -> Vk::DynamicComputePass& {
    return _ddgiTraceCS;
}

auto PostProcessFeature::DdgiUpdate() noexcept -> Vk::DynamicComputePass& {
    return _ddgiUpdateCS;
}

auto PostProcessFeature::DdgiTraceHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _ddgiTraceHeapBindings;
}

auto PostProcessFeature::DdgiUpdateHeapBindings() noexcept -> Vk::HeapPassBindings& {
    return _ddgiUpdateHeapBindings;
}

auto PostProcessFeature::RtShadow() noexcept -> Vk::DynamicComputePass& {
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

auto PostProcessFeature::Gtao() noexcept -> Vk::DynamicComputePass& {
    return _gtaoCS;
}

auto PostProcessFeature::RtrHalf() noexcept -> Vk::DynamicComputePass& {
    return _rtrHalfCS;
}

auto PostProcessFeature::RtgiHalf() noexcept -> Vk::DynamicComputePass& {
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
