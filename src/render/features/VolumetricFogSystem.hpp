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

using VolumetricClearLayout       = Vk::ReflectedLayout;
using VolumetricFogInjectLayout   = Vk::ReflectedLayout;
using VolumetricLightInjectLayout = Vk::ReflectedLayout;
using VolumetricIntegrationLayout = Vk::ReflectedLayout;
using VolumetricTemporalLayout    = Vk::ReflectedLayout;

class VolumetricFogSystem {
  public:
    VolumetricFogSystem()                                            = default;
    ~VolumetricFogSystem()                                           = default;
    VolumetricFogSystem(const VolumetricFogSystem&)                  = delete;
    auto operator=(const VolumetricFogSystem&) -> VolumetricFogSystem& = delete;
    VolumetricFogSystem(VolumetricFogSystem&&) noexcept              = delete;
    auto operator=(VolumetricFogSystem&&) noexcept -> VolumetricFogSystem& = delete;

    [[nodiscard]] auto Build(RenderContext::Impl& impl) -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto InitializeNoise(RenderContext::Impl& impl) -> std::expected<void, ErrorCode>;
    void DestroyNoise(Vk::Allocator& allocator) noexcept;

    void InitSamplers(RenderContext::Impl& impl) noexcept;

    [[nodiscard]] auto Clear() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricClearLayout>&;
    [[nodiscard]] auto FogInject() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricFogInjectLayout>&;
    [[nodiscard]] auto LightInject() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricLightInjectLayout>&;
    [[nodiscard]] auto Integrate() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricIntegrationLayout>&;
    [[nodiscard]] auto Temporal() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricTemporalLayout>&;

    [[nodiscard]] auto NoiseWrite() const noexcept -> Vk::ImageWrite;

    [[nodiscard]] auto VoxelDispatchExtent() const noexcept -> VkExtent3D;

  private:
    Vk::FixedDoubleBufferedComputePass<VolumetricClearLayout>       _clear;
    Vk::FixedDoubleBufferedComputePass<VolumetricFogInjectLayout>   _fogInject;
    Vk::FixedDoubleBufferedComputePass<VolumetricLightInjectLayout> _lightInject;
    Vk::FixedDoubleBufferedComputePass<VolumetricIntegrationLayout> _integrate;
    Vk::FixedDoubleBufferedComputePass<VolumetricTemporalLayout>    _temporal;

    Vk::Image     _noiseImage;
    Vk::ImageView _noiseView {};
};

}
