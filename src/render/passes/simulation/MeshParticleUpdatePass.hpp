// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Advances every mesh-particle emitter. Split from the billboard pass because
// the two have different parameter blocks and different pipelines, and because
// the graph can then fork them -- they touch disjoint buffers.
struct MeshParticleUpdatePass: Vk::RenderPass<"MeshParticleUpdate"> {
    RenderContext::Impl& impl;
    float                dt;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
