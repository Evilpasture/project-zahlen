// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ParticleSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>

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
    using namespace ZHLN::Ranges;
    auto&       reg = engine.GetRegistry();
    auto&       rc  = engine.GetRenderContext();
    const auto& cam = engine.GetCamera();

    // 1. Garbage Collect Dead 3D Particle Buffers using custom EraseIf pipe
    _active3DEmitters | EraseIf([&](const auto& pair) {
        if (!reg.IsAlive(Entity::Unpack(pair.first))) {
            rc.DestroyBuffer(pair.second);
            return true;
        }
        return false;
    });

    // 2. Process 2D Emitters
    auto entities = reg.GetEntitiesWith<Components::ParticleEmitterComponent>();
    auto emitters = reg.GetRawArray<Components::ParticleEmitterComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        auto& emitter = emitters[i];
        if (!emitter.active) {
            continue;
        }

        if (emitter.gpuBuffer == BufferHandle::Invalid) {
            emitter.gpuBuffer = rc.CreateStorageBuffer(emitter.maxParticles * sizeof(Particle));
        }

        ParticleEmitterParams params = emitter.params;
        if (emitter.attachToCamera) {
            params.spawnOrigin = {cam.position.GetX(), cam.position.GetY(), cam.position.GetZ()};
        }

        rc.SubmitParticleEmitter(emitter.gpuBuffer, emitter.maxParticles, params);
    }

    // 3. Process 3D Mesh Emitters
    auto mesh_entities = reg.GetEntitiesWith<Components::MeshParticleEmitterComponent>();
    auto mesh_emitters = reg.GetRawArray<Components::MeshParticleEmitterComponent>();

    for (size_t i = 0; i < mesh_entities.size(); ++i) { // FIXED Bug 1: mesh_entities.size()
        auto& emitter = mesh_emitters[i];
        if (!emitter.active) {
            continue;
        }

        Entity       e      = mesh_entities[i];
        BufferHandle buffer = BufferHandle::Invalid;

        auto packId = e.Pack();

        buffer = _active3DEmitters | FindOrInsert(packId, [&] { return rc.CreateStorageBuffer(emitter.maxParticles * sizeof(Particle3D)); });

        rc.SubmitMeshParticleEmitter(buffer, emitter.maxParticles, emitter.params, emitter.meshAsset, emitter.materialAsset);
    }
}

} // namespace ZHLN
