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

// The image-space effects that are not a lighting model.
//
// Bloom, the A-Trous denoiser, GTAO and the half-resolution reflection trace
// are all full-screen compute chains driven by `GraphicsSettings::post`, and
// the SMAA lookup tables are baked once and then only read. They share a build
// path -- reflect a compute layout, allocate frame-lifetime heap bindings,
// build a pipeline -- and nothing outside their own passes touches them, so
// they are collected here rather than left as loose pipeline handles on
// `RenderContext::Impl`.
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
    // half-resolution ray-traced reflection trace needs ray tracing, and
    // failing that one is not a failure of the others.
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

    [[nodiscard]] auto BloomThresholdHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto BloomDownHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto BloomUpHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto HdrDenoiseHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto GtaoHeapBindings() noexcept -> Vk::HeapPassBindings&;
    [[nodiscard]] auto RtrHalfHeapBindings() noexcept -> Vk::HeapPassBindings&;

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

    Vk::HeapPassBindings _bloomThresholdHeapBindings;
    Vk::HeapPassBindings _bloomDownHeapBindings;
    Vk::HeapPassBindings _bloomUpHeapBindings;
    Vk::HeapPassBindings _hdrDenoiseHeapBindings;
    Vk::HeapPassBindings _gtaoHeapBindings;
    Vk::HeapPassBindings _rtrHalfHeapBindings;

    Vk::ReflectedLayout _bloomThresholdCSLayout;
    Vk::ReflectedLayout _bloomDownCSLayout;
    Vk::ReflectedLayout _bloomUpCSLayout;
    Vk::ReflectedLayout _hdrDenoiseCSLayout;
    Vk::ReflectedLayout _gtaoCSLayout;
    Vk::ReflectedLayout _rtrHalfCSLayout;

    uint32_t _smaaAreaTexIdx   = 0;
    uint32_t _smaaSearchTexIdx = 0;
};

}
