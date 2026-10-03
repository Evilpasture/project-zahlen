// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The GPU-side structs, under the names the renderer uses internally.
//
// This header lives in src/render and not in include/: it is the one place that
// names generated code, and nothing outside the renderer may include it. The
// engine's own public vocabulary is include/Zahlen/Render/RenderData.hpp, whose
// descriptions are converted here (LayoutConvert.cpp).

#include <GeneratedGpuTypes.hpp>

namespace ZHLN {

using InstanceData              = GeneratedGpu::InstanceData;
using Light                     = GeneratedGpu::Light;
using FrameUniforms             = GeneratedGpu::FrameUniforms;
using ClusterBounds             = GeneratedGpu::ClusterBounds;
using ClusterVolume             = GeneratedGpu::ClusterVolume;
using Particle                  = GeneratedGpu::Particle;
using Particle3D                = GeneratedGpu::Particle3D;
using ParticleEmitterParams     = GeneratedGpu::ParticleEmitterParams;
using MeshParticleEmitterParams = GeneratedGpu::MeshParticleEmitterParams;

}
