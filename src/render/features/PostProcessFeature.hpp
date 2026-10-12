// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include "pipeline/ComputePass.hpp"
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
using RtrHalfCSLayout        = Vk::ReflectedLayout;
using RtgiHalfCSLayout       = Vk::ReflectedLayout;
using RtgiDenoiseCSLayout    = Vk::ReflectedLayout;
using RtgiCompositeCSLayout  = Vk::ReflectedLayout;

// The image-space effects that are not a lighting model.
//
// Bloom, the HDR and pre-albedo RTGI A-Trous denoisers, GTAO, the half-resolution
// reflection and RTGI traces, and the diffuse-GI composite are full-screen
// compute passes. The SMAA lookup tables are baked once and then only read.
// These passes share a build path -- reflect a compute layout, allocate
// frame-lifetime heap bindings, build a pipeline -- and nothing outside their
// own passes touches them, so they are collected here rather than left as
// loose pipeline handles on `RenderContext::Impl`.
class PostProcessFeature {
  public:
    PostProcessFeature()                                             = default;
    ~PostProcessFeature()                                            = default;
    PostProcessFeature(const PostProcessFeature&)                    = delete;
    auto operator=(const PostProcessFeature&) -> PostProcessFeature& = delete;
    PostProcessFeature(PostProcessFeature&&) noexcept                = delete;
    auto operator=(PostProcessFeature&&) noexcept -> PostProcessFeature& = delete;

    // --- setup --------------------------------------------------------------

    // Builds every compute pipeline below that this device can run: the
    // half-resolution reflection and RTGI traces need ray tracing; their
    // absence on other devices does not prevent the remaining pipelines.
    [[nodiscard]] auto Build(RenderContext::Impl& impl) -> std::expected<void, ErrorCode>;

    // Bakes the SMAA area and search lookup tables on the GPU.
    [[nodiscard]] auto BakeSMAALUTs(RenderContext::Impl& impl) -> std::expected<void, ErrorCode>;

    // Writes the pass-local sampler descriptors for the passes owned here.
    void InitSamplers(RenderContext::Impl& impl) noexcept;

    // --- what the passes need ----------------------------------------------

    [[nodiscard]] auto BloomThreshold() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto BloomDown() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto BloomUp() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto HdrDenoise() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto Gtao() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto RtrHalf() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto RtgiHalf() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto RtgiDenoise() noexcept -> Vk::DynamicComputePass&;
    [[nodiscard]] auto RtgiComposite() noexcept -> Vk::DynamicComputePass&;

    [[nodiscard]] auto BloomThresholdHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto BloomDownHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto BloomUpHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto HdrDenoiseHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto GtaoHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto RtrHalfHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto RtgiHalfHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto RtgiDenoiseHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto RtgiCompositeHeapBindings() noexcept -> Vk::HeapPassBindings&;

    // Bindless slots of the baked SMAA lookup tables; `SmaaWeightPass` reads
    // both while computing blending weights.
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
    Vk::DynamicComputePass _gtaoCS;
    Vk::DynamicComputePass _rtrHalfCS;
    Vk::DynamicComputePass _rtgiHalfCS;
    Vk::DynamicComputePass _rtgiDenoiseCS;
    Vk::DynamicComputePass _rtgiCompositeCS;

    Vk::HeapPassBindings _bloomThresholdHeapBindings;
    Vk::HeapPassBindings _bloomDownHeapBindings;
    Vk::HeapPassBindings _bloomUpHeapBindings;
    Vk::HeapPassBindings _hdrDenoiseHeapBindings;
    Vk::HeapPassBindings _gtaoHeapBindings;
    Vk::HeapPassBindings _rtrHalfHeapBindings;
    Vk::HeapPassBindings _rtgiHalfHeapBindings;
    Vk::HeapPassBindings _rtgiDenoiseHeapBindings;
    Vk::HeapPassBindings _rtgiCompositeHeapBindings;

    Vk::ReflectedLayout _bloomThresholdCSLayout;
    Vk::ReflectedLayout _bloomDownCSLayout;
    Vk::ReflectedLayout _bloomUpCSLayout;
    Vk::ReflectedLayout _hdrDenoiseCSLayout;
    Vk::ReflectedLayout _gtaoCSLayout;
    Vk::ReflectedLayout _rtrHalfCSLayout;
    Vk::ReflectedLayout _rtgiHalfCSLayout;
    Vk::ReflectedLayout _rtgiDenoiseCSLayout;
    Vk::ReflectedLayout _rtgiCompositeCSLayout;

    uint32_t _smaaAreaTexIdx   = 0;
    uint32_t _smaaSearchTexIdx = 0;
};

}
