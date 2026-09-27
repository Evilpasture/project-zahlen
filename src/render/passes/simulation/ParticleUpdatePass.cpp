// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/simulation/ParticleUpdatePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

// One emitter: where its particles live, how many there are, the timestep, and
// the emitter's own parameter block.
struct alignas(16) ComputePushConstants {
    VkDeviceAddress       particleBufferAddr;
    uint32_t              particleCount;
    float                 deltaTime;
    ParticleEmitterParams p;
};
static_assert(GpuAbi::ScenePassPayload<ComputePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void ParticleUpdatePass::operator()(VkCommandBuffer cmd) const noexcept {
    if (!impl.particleUpdatePass.pipeline.Valid() || impl.queues.ParticleEmitters().empty()) {
        return;
    }

    impl.BindHeapsAndPushFrame(cmd);

    for (const auto& emitter: impl.queues.ParticleEmitters()) {
        auto* buffer = impl.geometry.Resolve(emitter.gpuBuffer);
        if (!buffer) {
            continue;
        }

        const ComputePushConstants particlePC = {
            .particleBufferAddr = impl.ctx.BufferAddress(buffer->buffer.Handle()),
            .particleCount      = emitter.maxParticles,
            .deltaTime          = dt,
            .p                  = emitter.params
        };

        impl.particleUpdatePass.DispatchHeapThreads<Shaders::Modules::ParticleUpdateCS>(impl.ctx, cmd, emitter.maxParticles, 1, 1, particlePC);
    }
}

}
