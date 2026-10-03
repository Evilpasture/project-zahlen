// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Description -> GPU layout. Renderer-internal: this is the boundary the public
// headers stop at.
//
// The conversions are field-by-field on purpose. A shader field that disappears
// breaks the build here, in one file, instead of silently reshaping a public
// type; a field that appears is picked up here, where the layout is known.

#include <Zahlen/Render/RenderData.hpp>
#include "GpuLayout.hpp"

namespace ZHLN {

// Named plainly in ZHLN rather than in a detail namespace: configure/check_namespace_governance.py
// allows a name-carrying detail namespace only in headers that need one to hide
// *template* code, and these are four non-template functions in a header the
// renderer does not export.

[[nodiscard]] auto ToGpu(const ParticleDesc& desc) noexcept -> GeneratedGpu::Particle;
[[nodiscard]] auto ToGpu(const ParticleEmitterDesc& desc) noexcept -> GeneratedGpu::ParticleEmitterParams;
[[nodiscard]] auto ToGpu(const MeshParticleEmitterDesc& desc) noexcept -> GeneratedGpu::MeshParticleEmitterParams;
[[nodiscard]] auto ToGpu(const LightDesc& desc) noexcept -> GeneratedGpu::Light;
[[nodiscard]] auto ToGpu(const FrameViewData& view) noexcept -> GeneratedGpu::FrameUniforms;

} // namespace ZHLN
