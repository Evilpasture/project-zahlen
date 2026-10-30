// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/simulation/MeshParticleUpdatePass.hpp"
#include "RenderFrame.hpp"
#include "Renderer.hpp"
#include "passes/simulation/ParticleUpdateResources.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

using MeshParticleComputePush = Slang::MeshParticleComputePush;
static_assert(GpuAbi::ScenePassPayload<MeshParticleComputePush>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void MeshParticleUpdatePass::operator()(VkCommandBuffer cmd) const noexcept {
    if (!particles.mesh.pipeline.Valid() || frame.renderer.queues.MeshParticleEmitters().empty()) {
        return;
    }

    for (const auto& emitter: frame.renderer.queues.MeshParticleEmitters()) {
        auto* buffer = frame.renderer.geometry.Resolve(emitter.gpuBuffer);
        if (buffer == nullptr) {
            continue;
        }

        const MeshParticleComputePush pushPC = {
            .particleBufferAddr = frame.renderer.ctx.BufferAddress(buffer->buffer.Handle()),
            .particleCount      = emitter.maxParticles,
            .deltaTime          = dt,
            .p                  = emitter.params
        };

        particles.mesh.DispatchHeapThreads<Shaders::Modules::MeshParticleUpdateCS>(cmd, emitter.maxParticles, 1, 1, pushPC);
    }
}

} // namespace ZHLN::Passes
