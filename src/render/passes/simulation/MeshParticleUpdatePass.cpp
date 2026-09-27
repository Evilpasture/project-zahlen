// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/simulation/MeshParticleUpdatePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// One mesh-particle emitter: the same shape as the billboard payload, with the
// emitter parameter block that goes with instanced geometry.
struct alignas(16) MeshParticleComputePush {
    VkDeviceAddress           particleBufferAddr;
    uint32_t                  particleCount;
    float                     deltaTime;
    MeshParticleEmitterParams p;
};
static_assert(GpuAbi::ScenePassPayload<MeshParticleComputePush>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void MeshParticleUpdatePass::operator()(VkCommandBuffer cmd) const noexcept {
    if (!impl.meshParticleUpdatePass.pipeline.Valid() || impl.queues.MeshParticleEmitters().empty()) {
        return;
    }

    impl.BindHeapsAndPushFrame(cmd);

    for (const auto& emitter: impl.queues.MeshParticleEmitters()) {
        auto* buffer = impl.geometry.Resolve(emitter.gpuBuffer).value_or(nullptr);
        if (!buffer) {
            continue;
        }

        const MeshParticleComputePush pushPC = {
            .particleBufferAddr = impl.ctx.BufferAddress(buffer->buffer.Handle()),
            .particleCount      = emitter.maxParticles,
            .deltaTime          = dt,
            .p                  = emitter.params
        };

        impl.meshParticleUpdatePass.DispatchHeapThreads<Shaders::Modules::MeshParticleUpdateCS>(impl.ctx, cmd, emitter.maxParticles, 1, 1, pushPC);
    }
}

}
