// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ParticleSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

void ParticleSystem::Update(ECS::Query<const Components::ParticleEmitterComponent, const Components::MeshParticleEmitterComponent> query,
                            ECS::ResMut<RenderContext> render, ECS::Res<Camera> camera) {
    auto& rc = *render;
    const auto& cam = *camera;

    auto entities = query.Entities<Components::ParticleEmitterComponent>();
    auto emitters = query.Raw<Components::ParticleEmitterComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        auto& emitter = emitters[i];
        if (!emitter.active) {
            continue;
        }

        const BufferHandle buffer = rc.GetOrCreateParticleEmitterBuffer(entities[i], emitter.maxParticles);

        ParticleEmitterParams params = emitter.params;
        if (emitter.attachToCamera) {
            params.spawnOrigin = {cam.position.GetX(), cam.position.GetY(), cam.position.GetZ()};
        }

        rc.SubmitParticleEmitter(buffer, emitter.maxParticles, params);
    }

    auto mesh_entities = query.Entities<Components::MeshParticleEmitterComponent>();
    auto mesh_emitters = query.Raw<Components::MeshParticleEmitterComponent>();

    for (size_t i = 0; i < mesh_entities.size(); ++i) {
        auto& emitter = mesh_emitters[i];
        if (!emitter.active) {
            continue;
        }

        const BufferHandle buffer = rc.GetOrCreateMeshParticleEmitterBuffer(mesh_entities[i], emitter.maxParticles);

        rc.SubmitMeshParticleEmitter(buffer, emitter.maxParticles, emitter.params, emitter.meshAsset, emitter.materialAsset);
    }
}

}
