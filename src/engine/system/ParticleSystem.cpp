// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ParticleSystem.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Render/GpuLayout.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cstddef>

namespace ZHLN {

namespace {

template <typename ParticleType, typename Emitter>
[[nodiscard]] auto EnsureParticleStorage(RenderContext& rc, Emitter& emitter) -> BufferHandle {
    if (emitter.gpuBuffer != BufferHandle::Invalid && emitter.bufferCapacity != emitter.maxParticles) {
        rc.DestroyBuffer(emitter.gpuBuffer);
        emitter.gpuBuffer      = BufferHandle::Invalid;
        emitter.bufferCapacity = 0;
    }
    if (emitter.gpuBuffer == BufferHandle::Invalid && emitter.maxParticles != 0) {
        emitter.gpuBuffer = rc.CreateStorageBuffer(static_cast<size_t>(emitter.maxParticles) * sizeof(ParticleType));
        if (emitter.gpuBuffer != BufferHandle::Invalid) {
            emitter.bufferCapacity = emitter.maxParticles;
        }
    }
    return emitter.gpuBuffer;
}

}

void ParticleSystem::Update(ECS::Query<Components::ParticleEmitterComponent, Components::MeshParticleEmitterComponent> query,
                            ECS::ResMut<RenderContext> render, ECS::Res<Camera> camera) {
    auto& rc        = *render;
    const auto& cam = *camera;

    auto emitters = query.Raw<Components::ParticleEmitterComponent>();
    for (size_t i = 0; i < emitters.size(); ++i) {
        auto& emitter = emitters[i];
        if (!emitter.active) {
            continue;
        }

        const BufferHandle buffer = EnsureParticleStorage<Particle>(rc, emitter);
        ParticleEmitterParams params = emitter.params;
        if (emitter.attachToCamera) {
            params.spawnOrigin = {cam.position.GetX(), cam.position.GetY(), cam.position.GetZ()};
        }
        rc.SubmitParticleEmitter(buffer, emitter.maxParticles, params);
    }

    auto meshEmitters = query.Raw<Components::MeshParticleEmitterComponent>();
    for (size_t i = 0; i < meshEmitters.size(); ++i) {
        auto& emitter = meshEmitters[i];
        if (!emitter.active) {
            continue;
        }

        const BufferHandle buffer = EnsureParticleStorage<Particle3D>(rc, emitter);
        rc.SubmitMeshParticleEmitter(buffer, emitter.maxParticles, emitter.params, emitter.meshAsset, emitter.materialAsset);
    }
}

}
