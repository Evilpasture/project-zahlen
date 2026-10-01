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

// The volumetric fog subsystem: five dispatches and the scratch they share.
//
// The fog volume is not one pass. Clear, media injection, light injection,
// integration and temporal resolve are five compute passes over the same 3D
// voxel grid, they need a noise texture no other subsystem looks at, and the
// grid's extent is derived from the clear pass's dispatch size rather than
// chosen independently. That is a subsystem, so the pipelines and the noise
// live here instead of being loose members of `RenderContext::Impl`; the passes
// in `src/render/passes/volumetric/` reach them through this object.
class VolumetricFogSystem {
  public:
    VolumetricFogSystem()                                            = default;
    ~VolumetricFogSystem()                                           = default;
    VolumetricFogSystem(const VolumetricFogSystem&)                  = delete;
    auto operator=(const VolumetricFogSystem&) -> VolumetricFogSystem& = delete;
    VolumetricFogSystem(VolumetricFogSystem&&) noexcept              = delete;
    auto operator=(VolumetricFogSystem&&) noexcept -> VolumetricFogSystem& = delete;

    // --- setup --------------------------------------------------------------

    // Builds the five compute pipelines. The clear pass carries the fixed
    // dispatch domain that sizes the voxel targets, so this has to run before
    // `RenderContext::Impl::RecreateTargets` reads `VoxelDispatchExtent()`.
    [[nodiscard]] auto Build(RenderContext::Impl& impl) -> std::expected<void, ErrorCode>;

    // Uploads the tiling 3D noise the media injection pass samples.
    [[nodiscard]] auto InitializeNoise(RenderContext::Impl& impl) -> std::expected<void, ErrorCode>;
    void DestroyNoise(Vk::Allocator& allocator) noexcept;

    // Writes the pass-local sampler descriptors for the three volumetric
    // passes that declare one: the temporal resolve's linear tap, the noise
    // sampler the media injection tiles with, and the shadow sampler the light
    // injection compares the cascade map against.
    void InitSamplers(RenderContext::Impl& impl) noexcept;

    // --- what the passes need ----------------------------------------------

    [[nodiscard]] auto Clear() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricClearLayout>&;
    [[nodiscard]] auto FogInject() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricFogInjectLayout>&;
    [[nodiscard]] auto LightInject() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricLightInjectLayout>&;
    [[nodiscard]] auto Integrate() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricIntegrationLayout>&;
    [[nodiscard]] auto Temporal() noexcept -> Vk::FixedDoubleBufferedComputePass<VolumetricTemporalLayout>&;

    // The noise texture, bound as a sampled image by the media injection pass.
    [[nodiscard]] auto NoiseWrite() const noexcept -> Vk::ImageWrite;

    // The voxel grid extent, taken from the clear pass's dispatch domain.
    [[nodiscard]] auto VoxelDispatchExtent() const noexcept -> VkExtent3D;

  private:
    Vk::FixedDoubleBufferedComputePass<VolumetricClearLayout>       _clear;
    Vk::FixedDoubleBufferedComputePass<VolumetricFogInjectLayout>   _fogInject;
    Vk::FixedDoubleBufferedComputePass<VolumetricLightInjectLayout> _lightInject;
    Vk::FixedDoubleBufferedComputePass<VolumetricIntegrationLayout> _integrate;
    Vk::FixedDoubleBufferedComputePass<VolumetricTemporalLayout>    _temporal;

    Vk::Image              _noiseImage;
    Vk::ImageView _noiseView;
};

}
