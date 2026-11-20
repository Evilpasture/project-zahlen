// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <audio/Audio.hpp>
#include <engine/SceneResources.hpp>
#include <physics/Physics.hpp>
#include <render/RenderContext.hpp>
#include <span>
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

void Release(Engine& engine, Components::PhysicsComponent& component) {
    Release(engine.Physics(), component);
}

void Release(Engine& engine, Components::RagdollComponent& component) {
    Release(engine.Physics(), component);
}

void Release(AudioContext& audio, Components::AudioSourceComponent& component) {
    audio.StopVoice(std::exchange(component.voiceID, VoiceID::Invalid), component.fadeOut);
}

void Release(Engine& engine, Components::AudioSourceComponent& component) {
    Release(engine.Audio(), component);
}

void Release(Engine& engine, Components::OwnedMeshComponent& component) {
    Release(engine.Render(), component);
}

void Release(Engine& engine, Components::SkeletalMeshComponent& component) {
    if (component.jointCount > 0) {
        engine.FreeJoints(component.jointOffset, component.jointCount);
        component.jointOffset = 0;
        component.jointCount  = 0;
    }
    engine.Render().DestroyBuffer(std::exchange(component.skinnedScratch, BufferID::Invalid));
    component.scratchVertexCount = 0;
}

void Release(Engine& engine, Components::MorphTargetComponent& component) {
    if (component.morphDeltaCount > 0) {
        engine.Render().FreeMorphDeltas(component.offset, component.morphDeltaCount);
        component.offset         = 0;
        component.morphDeltaCount = 0;
    }
}

} // namespace ZHLN::SceneResources
