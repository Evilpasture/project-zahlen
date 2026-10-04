// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The renderer's view of the generated ABI.
//
// The structs the engine and the shaders share are defined once, by hand, in
// include/Zahlen/Render/RenderData.hpp -- the generated header aliases those and
// asserts their layout against Slang reflection. What is left here are the
// structs only the renderer has a name for, aliased out of GeneratedGpu so a
// render TU can say `InstanceData` the way it always has.
//
// This header stays in src/render and not in include/: it names generated code,
// and the renderer is its only consumer.

#include <GeneratedGpuTypes.hpp>
#include <Zahlen/Render/RenderData.hpp>

namespace ZHLN {

using InstanceData   = GeneratedGpu::InstanceData;
using ClusterBounds  = GeneratedGpu::ClusterBounds;
using ClusterVolume  = GeneratedGpu::ClusterVolume;
using Particle3D     = GeneratedGpu::Particle3D;

} // namespace ZHLN
