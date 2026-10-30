// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "GpuLayout.hpp"
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Render/ParticleEmitterDesc.hpp>
#include <Zahlen/Render/FrameData.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <cstdint>

namespace ZHLN::GpuPack {

[[nodiscard]] auto PackMeshlet(const MeshletDesc& desc) noexcept -> GPUMeshlet;

[[nodiscard]] auto PackParticleEmitter(const ParticleEmitterDesc& desc, uint32_t textureIndex, uint32_t blendMode) noexcept
    -> ParticleEmitterParams;

[[nodiscard]] auto PackMeshParticleEmitter(const MeshParticleEmitterDesc& desc) noexcept -> MeshParticleEmitterParams;

[[nodiscard]] auto PackLight(const LightDesc& desc, const JPH::Mat44& worldToView, int32_t shadowTileBase) noexcept -> Light;

[[nodiscard]] auto PackFrameData(const FrameData& frame) noexcept -> FrameUniforms;

} // namespace ZHLN::GpuPack
