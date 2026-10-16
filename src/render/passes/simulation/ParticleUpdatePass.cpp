// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "passes/simulation/ParticleUpdatePass.hpp"
#include <ShaderBindings.hpp>

namespace ZHLN::Passes {

struct alignas(16) ComputePushConstants {
    VkDeviceAddress       particleBufferAddr;
    uint32_t              particleCount;
    float                 deltaTime;
    ParticleEmitterParams p;
};
static_assert(GpuAbi::ScenePassPayload<ComputePushConstants>, "a pass payload that outgrew the push blob's prefix, asserted where it is declared");

void ParticleUpdatePass::operator()(VkCommandBuffer cmd) const noexcept {
    if (!particles.particles.pipeline.Valid() || frame.queues.ParticleEmitters().empty()) {
        return;
    }

    frame.BindHeapsAndPushFrame(cmd);

    for (const auto& emitter: frame.queues.ParticleEmitters()) {
        if (!emitter.simulate) {
            continue;
        }
        auto* buffer = frame.geometry.Resolve(emitter.gpuBuffer);
        if (buffer == nullptr) {
            continue;
        }

        const ComputePushConstants particlePC = {
            .particleBufferAddr = frame.ctx.BufferAddress(buffer->buffer.Handle()), .particleCount = emitter.maxParticles, .deltaTime = dt, .p = emitter.params
        };

        particles.particles.DispatchHeapThreads<Shaders::Modules::ParticleUpdateCS>(cmd, emitter.maxParticles, 1, 1, particlePC);
    }
}

} // namespace ZHLN::Passes
