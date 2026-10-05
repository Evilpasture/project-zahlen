// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The renderer's view of the generated ABI.
//
// tools/zshader emits every GPU-visible struct from the Slang module -- member
// order, offsets, alignment, the padding lanes and the packed arrays as the
// compiler seated them. Nothing here is hand-written, and nothing under include/
// names a generated type: the engine hands the renderer its own terms
// (ParticleEmitterDesc, LightDesc, FrameData) and src/render/GpuPack.cpp packs
// them. That is what lets the engine build without the shader tool having run.
//
// This header stays in src/render and not in include/: it names generated code,
// and the renderer is its only consumer.

#include <GeneratedGpuTypes.hpp>

namespace ZHLN {

// Structs only the renderer has a name for.
using GeneratedGpu::ClusterBounds;
using GeneratedGpu::ClusterVolume;
using GeneratedGpu::InstanceData;
using GeneratedGpu::Particle3D;

// And the ones both halves of the work have a word for: one particle, an
// emitter's parameters, a light, a frame.
using GeneratedGpu::FrameUniforms;
using GeneratedGpu::Light;
using GeneratedGpu::MeshParticleEmitterParams;
using GeneratedGpu::Particle;
using GeneratedGpu::ParticleEmitterParams;

} // namespace ZHLN
