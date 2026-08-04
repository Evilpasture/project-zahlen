// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ParticleSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

// Out-of-line definition resolves buffer cleanup without header pollution
void Components::ParticleEmitterComponent::OnDestroy(ParticleEmitterComponent* p) noexcept {
    if (p->gpuBuffer != BufferHandle::Invalid) {
        if (auto* engine = GetEngineContext()) {
            engine->GetRenderContext().DestroyBuffer(p->gpuBuffer);
        }
        p->gpuBuffer = BufferHandle::Invalid;
    }
}

void ParticleSystem::Update(Engine& engine, float /*dt*/) {
    auto&       reg = engine.GetRegistry();
    auto&       rc  = engine.GetRenderContext();
    const auto& cam = engine.GetCamera();

    auto entities = reg.GetEntitiesWith<Components::ParticleEmitterComponent>();
    auto emitters = reg.GetRawArray<Components::ParticleEmitterComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        auto& emitter = emitters[i];
        if (!emitter.active) {
            continue;
        }

        // 1. Lazy GPU storage buffer allocation
        if (emitter.gpuBuffer == BufferHandle::Invalid) {
            emitter.gpuBuffer = rc.CreateStorageBuffer(emitter.maxParticles * sizeof(Particle));
        }

        // 2. Resolve camera-relative offsets in-place
        ParticleEmitterParams params = emitter.params;
        if (emitter.attachToCamera) {
            params.spawnOrigin = {cam.position.GetX(), cam.position.GetY(), cam.position.GetZ()};
        }

        // 3. Dispatch raw graphics handles directly to the pure render queue
        rc.SubmitParticleEmitter(emitter.gpuBuffer, emitter.maxParticles, params);
    }
}

} // namespace ZHLN
