// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/ComputePassResources.hpp
//
// The three standalone compute passes -- step 2 of
// rendercontext-impl-decomposition.md. Unlike the culling or particle-update
// families these are not driven by a graph pass struct: they are dispatched
// straight from Impl (skinning from the frame path, the procedural bake from
// the bake entry point, the hang-GPU pass from the device-lost provoke), which
// is why this header sits next to RenderInternal.hpp rather than under
// passes/.

#pragma once

#include "Rendering.hpp"

namespace ZHLN {

struct ComputePassResources {
    Vk::DynamicComputePass skinning;       // per-draw skinning of vertex buffers
    Vk::DynamicComputePass proceduralBake; // noise/gradient texture generation
    Vk::DynamicComputePass hangGpu;        // deliberate device-lost test dispatch
};

} // namespace ZHLN
