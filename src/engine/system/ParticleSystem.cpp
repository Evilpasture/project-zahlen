// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/system/ParticleSystem.cpp
#include "ParticleSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

void ParticleSystem::Update(Engine& engine, float /*dt*/) {
    using namespace ZHLN::Ranges;
    auto&       reg = engine.GetRegistry();
    auto&       rc  = engine.GetRenderContext();
    const auto& cam = engine.GetCamera();

    // Pull the general-purpose tracked buffers from the RenderContext
    auto& active2D = rc.GetTracked2DEmitters();
    auto& active3D = rc.GetTracked3DEmitters();

    // 1. Garbage Collect Dead 2D and 3D Particle Buffers using custom EraseIf pipe
    active2D | EraseIf([&](const auto& pair) {
        if (!reg.IsAlive(Entity::Unpack(pair.first))) {
            rc.DestroyBuffer(pair.second);
            return true;
        }
        return false;
    });

    active3D | EraseIf([&](const auto& pair) {
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

        Entity       e      = entities[i];
        BufferHandle buffer = BufferHandle::Invalid;

        auto packId = e.Pack();

        buffer = active2D | FindOrInsert(packId, [&] { return rc.CreateStorageBuffer(emitter.maxParticles * sizeof(Particle)); });

        ParticleEmitterParams params = emitter.params;
        if (emitter.attachToCamera) {
            params.spawnOrigin = {cam.position.GetX(), cam.position.GetY(), cam.position.GetZ()};
        }

        rc.SubmitParticleEmitter(buffer, emitter.maxParticles, params);
    }

    // 3. Process 3D Mesh Emitters
    auto mesh_entities = reg.GetEntitiesWith<Components::MeshParticleEmitterComponent>();
    auto mesh_emitters = reg.GetRawArray<Components::MeshParticleEmitterComponent>();

    for (size_t i = 0; i < mesh_entities.size(); ++i) {
        auto& emitter = mesh_emitters[i];
        if (!emitter.active) {
            continue;
        }

        Entity       e      = mesh_entities[i];
        BufferHandle buffer = BufferHandle::Invalid;

        auto packId = e.Pack();

        buffer = active3D | FindOrInsert(packId, [&] { return rc.CreateStorageBuffer(emitter.maxParticles * sizeof(Particle3D)); });

        rc.SubmitMeshParticleEmitter(buffer, emitter.maxParticles, emitter.params, emitter.meshAsset, emitter.materialAsset);
    }
}

} // namespace ZHLN
