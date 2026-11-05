// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Rendering.hpp"
#include "graph/PassExecution.hpp"
#include <vkd3d/graph/PassDecl.hpp>

namespace ZHLN {
struct ParticleUpdateResources;
struct RenderFrame;
} // namespace ZHLN

namespace ZHLN::Passes {

struct MeshParticleUpdatePass: VkD3D::RenderPass<"MeshParticleUpdate"> {
    const ParticleUpdateResources& particles;
    RenderFrame&                   frame;
    float                          dt;

    void operator()(VkCommandBuffer cmd) const noexcept;
};

} // namespace ZHLN::Passes
