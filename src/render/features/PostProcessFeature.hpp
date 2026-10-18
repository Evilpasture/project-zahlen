// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include "PassPayloads.hpp"
#include "pipeline/ComputePass.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/RenderContext.hpp>
#include <cstdint>
#include <expected>

namespace ZHLN {

using BloomThresholdCSLayout = Vk::ReflectedLayout;
using BloomDownCSLayout      = Vk::ReflectedLayout;
using BloomUpCSLayout        = Vk::ReflectedLayout;
using HdrDenoiseCSLayout     = Vk::ReflectedLayout;
using GtaoCSLayout           = Vk::ReflectedLayout;
using RtShadowCSLayout       = Vk::ReflectedLayout;
using ShadowDenoiseCSLayout  = Vk::ReflectedLayout;
using RtrHalfCSLayout        = Vk::ReflectedLayout;
using RtgiHalfCSLayout       = Vk::ReflectedLayout;
using RtgiDenoiseCSLayout    = Vk::ReflectedLayout;
using RtgiCompositeCSLayout  = Vk::ReflectedLayout;
using DdgiTraceCSLayout      = Vk::ReflectedLayout;
using DdgiUpdateCSLayout     = Vk::ReflectedLayout;

class PostProcessFeature {
  public:
    PostProcessFeature()                                             = default;
    ~PostProcessFeature()                                            = default;
    PostProcessFeature(const PostProcessFeature&)                    = delete;
    auto operator=(const PostProcessFeature&) -> PostProcessFeature& = delete;
    PostProcessFeature(PostProcessFeature&&) noexcept                = delete;
    auto operator=(PostProcessFeature&&) noexcept -> PostProcessFeature& = delete;

    [[nodiscard]] auto Build(RenderContext::Impl& impl) -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto BakeSMAALUTs(RenderContext::Impl& impl) -> std::expected<void, ErrorCode>;

    void InitSamplers(RenderContext::Impl& impl) noexcept;

    [[nodiscard]] auto BloomThreshold() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto BloomDown() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto BloomUp() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto HdrDenoise() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto Gtao() noexcept -> Vk::TypedComputePass<Shaders::Modules::GtaoCS, GtaoPushConstants>&;
    [[nodiscard]] auto RtShadow() noexcept -> Vk::TypedComputePass<Shaders::Modules::RtShadowCS, RTShadowPushConstants>&;
    [[nodiscard]] auto ShadowDenoise() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto RtrHalf() noexcept -> Vk::TypedComputePass<Shaders::Modules::RtrHalfCS, RtrHalfPushConstants>&;
    [[nodiscard]] auto RtgiHalf() noexcept -> Vk::TypedComputePass<Shaders::Modules::RtgiHalfCS, RtgiHalfPushConstants>&;
    [[nodiscard]] auto RtgiDenoise() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto RtgiComposite() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto DdgiTrace() noexcept -> Vk::TypedComputePass<Shaders::Modules::DdgiTraceCS, DdgiTracePushConstants>&;
    [[nodiscard]] auto DdgiUpdate() noexcept -> Vk::TypedComputePass<Shaders::Modules::DdgiUpdateIrradianceCS, DdgiUpdatePushConstants>&;

    [[nodiscard]] auto BloomThresholdHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto BloomDownHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto BloomUpHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto HdrDenoiseHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto GtaoHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto RtShadowHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto ShadowDenoiseHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto RtrHalfHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto RtgiHalfHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto RtgiDenoiseHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto RtgiCompositeHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto DdgiTraceHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto DdgiUpdateHeapBindings() noexcept -> Vk::HeapPassBindings&;

    [[nodiscard]] auto SmaaAreaTexture() const noexcept -> uint32_t;
    [[nodiscard]] auto SmaaSearchTexture() const noexcept -> uint32_t;

  private:
    [[nodiscard]] auto BuildCompute(
        RenderContext::Impl& impl, Vk::DynamicComputePass& pass, Vk::ReflectedLayout& layout, Vk::HeapPassBindings& bindings, std::span<const uint8_t> spirv
    ) -> std::expected<void, ErrorCode>;

    Vk::DynamicComputePass _bloomThresholdCS;
    Vk::DynamicComputePass _bloomDownCS;
    Vk::DynamicComputePass _bloomUpCS;
    Vk::DynamicComputePass _hdrDenoiseCS;
    Vk::TypedComputePass<Shaders::Modules::GtaoCS, GtaoPushConstants> _gtaoCS;
    Vk::TypedComputePass<Shaders::Modules::RtShadowCS, RTShadowPushConstants> _rtShadowCS;
    Vk::DynamicComputePass _shadowDenoiseCS;
    Vk::TypedComputePass<Shaders::Modules::RtrHalfCS, RtrHalfPushConstants> _rtrHalfCS;
    Vk::TypedComputePass<Shaders::Modules::RtgiHalfCS, RtgiHalfPushConstants> _rtgiHalfCS;
    Vk::DynamicComputePass _rtgiDenoiseCS;
    Vk::DynamicComputePass _rtgiCompositeCS;
    Vk::TypedComputePass<Shaders::Modules::DdgiTraceCS, DdgiTracePushConstants> _ddgiTraceCS;
    Vk::TypedComputePass<Shaders::Modules::DdgiUpdateIrradianceCS, DdgiUpdatePushConstants> _ddgiUpdateCS;

    Vk::HeapPassBindings _bloomThresholdHeapBindings;
    Vk::HeapPassBindings _bloomDownHeapBindings;
    Vk::HeapPassBindings _bloomUpHeapBindings;
    Vk::HeapPassBindings _hdrDenoiseHeapBindings;
    Vk::HeapPassBindings _gtaoHeapBindings;
    Vk::HeapPassBindings _rtShadowHeapBindings;
    Vk::HeapPassBindings _shadowDenoiseHeapBindings;
    Vk::HeapPassBindings _rtrHalfHeapBindings;
    Vk::HeapPassBindings _rtgiHalfHeapBindings;
    Vk::HeapPassBindings _rtgiDenoiseHeapBindings;
    Vk::HeapPassBindings _rtgiCompositeHeapBindings;
    Vk::HeapPassBindings _ddgiTraceHeapBindings;
    Vk::HeapPassBindings _ddgiUpdateHeapBindings;

    Vk::ReflectedLayout _bloomThresholdCSLayout;
    Vk::ReflectedLayout _bloomDownCSLayout;
    Vk::ReflectedLayout _bloomUpCSLayout;
    Vk::ReflectedLayout _hdrDenoiseCSLayout;
    Vk::ReflectedLayout _gtaoCSLayout;
    Vk::ReflectedLayout _rtShadowCSLayout;
    Vk::ReflectedLayout _shadowDenoiseCSLayout;
    Vk::ReflectedLayout _rtrHalfCSLayout;
    Vk::ReflectedLayout _rtgiHalfCSLayout;
    Vk::ReflectedLayout _rtgiDenoiseCSLayout;
    Vk::ReflectedLayout _rtgiCompositeCSLayout;
    Vk::ReflectedLayout _ddgiTraceCSLayout;
    Vk::ReflectedLayout _ddgiUpdateCSLayout;

    uint32_t _smaaAreaTexIdx   = 0;
    uint32_t _smaaSearchTexIdx = 0;
};

}
