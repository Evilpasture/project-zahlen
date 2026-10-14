// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/passes/culling/CullingResources.hpp
//
// Depth reduction, the GPU draw cull that consumes it, and the two cluster
// passes -- step 2 of rendercontext-impl-decomposition.md.
//
// One group, not three, because they are one pipeline of work: the HiZ pyramid
// exists to be the rejection test the draw cull runs, the cluster bounds are the
// AABBs that cull reads, and the cluster grid it writes is what the lighting
// pass consumes. init/RenderInitScenePipelines.cpp already builds all four in a
// single InitCullingResources, and the order they are built in does not matter
// -- every member here is default-constructed and filled in later.
//
// As with the AA pipelines, build and hot-reload stay where they are: they need
// the Impl for its device, heap and shader loading. This is the ownership half.

#pragma once

#include "Rendering.hpp"

namespace ZHLN {

struct CullingResources {
    // The depth pyramid: one dispatch per mip, each reading the mip above.
    Vk::HeapPassBindings   hizHeapBindings;
    Vk::ReflectedLayout    hizDescLayout;
    Vk::DynamicComputePass hizGeneratePass;

    // The GPU draw cull: one dispatch fills the indirect command buffer the
    // GBuffer passes then draw from. Reads the pyramid above.
    Vk::HeapPassBindings   cullingHeapBindings;
    Vk::ReflectedLayout    cullingLayout;
    Vk::DynamicComputePass cullingPass;

    // Cluster culling: the visible-cluster grid and light index list the
    // lighting pass reads.
    Vk::HeapPassBindings clusterCullingHeapBindings;
    Vk::ReflectedLayout  clusterCullingDescLayout;
    Vk::FixedComputePass clusterCullingPass;

    // Cluster bounds: the AABBs the cull tests against, refreshed only when
    // frameState.clusterBoundsDirty says they moved.
    Vk::HeapPassBindings clusterBoundsHeapBindings;
    Vk::ReflectedLayout  clusterBoundsDescLayout;
    Vk::FixedComputePass clusterBoundsPass;
};

} // namespace ZHLN
