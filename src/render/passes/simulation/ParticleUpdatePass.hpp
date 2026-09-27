// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"
#include "graph/RenderGraph.hpp"

namespace ZHLN::Passes {

// Advances every billboard particle emitter by one step. It touches only
// storage buffers, so it declares no image usage; what it does declare is a
// dependency-free compute slot in the simulation graph, which is where the
// ordering against the volumetric passes comes from.
struct ParticleUpdatePass: Vk::RenderPass<"ParticleUpdate"> {
    RenderContext::Impl& impl;
    float                dt;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

}
