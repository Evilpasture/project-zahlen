// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/SceneResources.hpp>
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <utility>

namespace ZHLN::SceneResources {

void Release(PhysicsContext& physics, Components::PhysicsComponent& component) {
    physics.DestroyBody(std::exchange(component.physicsID, Physics::BodyID::Null()));
}

void Release(PhysicsContext& physics, Components::RagdollComponent& component) {
    physics.DestroyRagdoll(std::exchange(component.ragdollID, Physics::RagdollID::Invalid));
    component.isAddedToPhysics = false;
}

void Release(RenderContext& render, Components::OwnedMeshComponent& component) {
    if (component.meshAsset != InvalidAssetID) {
        render.UnregisterGPUMesh(component.meshAsset);
    }
    render.DestroyMesh(std::exchange(component.mesh, Mesh {}));
    component.meshAsset = InvalidAssetID;
}

void Release(GraphicsDevice graphics, Components::OwnedMeshComponent& component) {
    if (component.meshAsset != InvalidAssetID) {
        graphics.UnregisterMesh(component.meshAsset);
    }
    graphics.DestroyMesh(std::exchange(component.mesh, Mesh {}));
    component.meshAsset = InvalidAssetID;
}

void Release(Engine& engine, Components::PhysicsComponent& component) {
    engine.Physics().DestroyBody(std::exchange(component.physicsID, Physics::BodyID::Null()));
}

void Release(Engine& engine, Components::RagdollComponent& component) {
    const auto id = std::exchange(component.ragdollID, Physics::RagdollID::Invalid);
    engine.Physics().DestroyRagdolls(std::span {&id, 1});
    component.isAddedToPhysics = false;
}

void Release(AudioContext& audio, Components::AudioSourceComponent& component) {
    audio.StopVoice(std::exchange(component.voiceID, VoiceID::Invalid), component.fadeOut);
}

void Release(Engine& engine, Components::AudioSourceComponent& component) {
    engine.Audio().StopVoice(std::exchange(component.voiceID, VoiceID::Invalid), component.fadeOut);
}

void Release(Engine& engine, Components::OwnedMeshComponent& component) {
    Release(engine.Graphics(), component);
}

void Release(Engine& engine, Components::SkeletalMeshComponent& component) {
    engine.Graphics().DestroyBuffer(std::exchange(component.skinnedScratch, BufferID::Invalid));
    component.scratchVertexCount = 0;
}

} // namespace ZHLN::SceneResources
