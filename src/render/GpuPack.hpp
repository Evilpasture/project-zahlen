// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The boundary: where a scene description becomes the struct a shader reads.
//
// One place, so "who knows the ABI" has an answer the compiler enforces: the renderer
// does, and this is the file. A host hands RenderContext a ParticleEmitterDesc, a
// LightDesc or a FrameData -- Jolt's compute types, no padding, no lane-sharing, no
// field the renderer owns -- and the pack functions here write the shader's struct
// field by field, in the shader's own units.
//
// Packing is not free, and it is not meant to be. The alternative the engine used to
// have was one struct serving both sides: it put hardware padding, view-space state and
// renderer-owned fields (bindless indices, screen resolution, cascade matrices) into
// public headers and ECS components, and made every author know which lane a value rode
// in. A frame's worth of packing is a few thousand stores.
//
// The structs on the right-hand side are the ones tools/zshader emits from the Slang
// module (src/render/GpuLayout.hpp names them): member order, padding and lanes are
// whatever the shader declared, and a shader-side change re-emits them. A field this
// file no longer names is then a compile error here -- where the packing lives --
// rather than a silently reshaped struct in a public header.

#include "GpuLayout.hpp" // the generated structs this file writes, as ZHLN names
#include <Zahlen/ParticleEmitterDesc.hpp>
#include <Zahlen/Render/FrameData.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <cstdint>

namespace ZHLN::GpuPack {

// @p textureIndex is the bindless index the renderer resolved from the host's
// TextureHandle; @p blendMode is 0 for alpha and 1 for additive, which is how the
// forward pass selects its pipeline.
[[nodiscard]] auto PackParticleEmitter(const ParticleEmitterDesc& desc, uint32_t textureIndex, uint32_t blendMode) noexcept
    -> ParticleEmitterParams;

[[nodiscard]] auto PackMeshParticleEmitter(const MeshParticleEmitterDesc& desc) noexcept -> MeshParticleEmitterParams;

// @p worldToView is the frame's view matrix: a light's view-space position is not
// scene data, so the renderer derives it here rather than asking the engine for it.
[[nodiscard]] auto PackLight(const LightDesc& desc, const JPH::Mat44& worldToView) noexcept -> Light;

// The packed frame carries only what the engine authored; the renderer fills the
// lanes only it knows (screen resolution, light count, cascade splits, the SH
// payload, the viewmodel matrix) after this returns.
[[nodiscard]] auto PackFrameData(const FrameData& frame) noexcept -> FrameUniforms;

} // namespace ZHLN::GpuPack
