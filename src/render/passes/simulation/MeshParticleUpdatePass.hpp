// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include "graph/RenderPassDecl.hpp"

namespace ZHLN {
struct ParticleUpdateResources;
struct RenderFrameContext;
} // namespace ZHLN

namespace ZHLN::Passes {

struct MeshParticleUpdatePass: Vk::RenderPass<"MeshParticleUpdate"> {
    const ParticleUpdateResources& particles;
    RenderFrameContext&            frame;
    float                          dt;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
