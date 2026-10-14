// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/passes/simulation/ParticleUpdateResources.hpp
//
// The two emitter-advance pipelines: billboard particles and mesh particles --
// step 2 of rendercontext-impl-decomposition.md. Grouped because they are the
// same feature on the same graph slot and are split into two passes only so
// the graph can fork them over disjoint buffers.

#pragma once

#include "Rendering.hpp"

namespace ZHLN {

struct ParticleUpdateResources {
    Vk::DynamicComputePass particles; // billboard emitters
    Vk::DynamicComputePass mesh;      // mesh-particle emitters
};

} // namespace ZHLN
