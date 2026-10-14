// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/passes/SceneDrawResources.hpp
//
// The pipelines the three shared SceneDraw helpers reach for -- step 2 of
// rendercontext-impl-decomposition.md. Two groups, because they are two
// features built by two different init functions: InitCSGPipelines builds the
// stencil trio, BuildMeshParticlePipelines builds the pair.
//
// They are split rather than merged into one SceneDrawResources so that a pass
// says which of the two it actually draws with: GBufferResolvePass needs both,
// ShadowPass needs only the mesh-particle shadow pipeline and should not have
// to advertise the CSG stencil ones.

#pragma once

#include "Rendering.hpp" // Vk::Pipeline

namespace ZHLN {

// Stencil-buffer constructive solid geometry: the cutters write the stencil,
// then the eye mesh is drawn against it to cut or intersect.
struct CsgResources {
    Vk::Pipeline write;        // cutters -> stencil
    Vk::Pipeline difference;   // eye mesh where the stencil was written
    Vk::Pipeline intersection; // eye mesh only where every cutter wrote
};

// Mesh-particle draws: the shaded one for the main view, the depth-only one for
// the shadow passes.
struct MeshParticlePipelines {
    Vk::Pipeline render;
    Vk::Pipeline shadow;
};

} // namespace ZHLN
