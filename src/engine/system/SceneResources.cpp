// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/SceneResources.hpp>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <utility>

namespace ZHLN::SceneResources {

void Release(PhysicsContext& physics, Components::PhysicsComponent& component) {
    physics.DestroyBody(std::exchange(component.physicsHandle, Physics::BodyHandle::Null()));
}

void Release(PhysicsContext& physics, Components::RagdollComponent& component) {
    physics.DestroyRagdoll(std::exchange(component.ragdollHandle, Physics::RagdollHandle::Invalid));
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
    Release(engine.GetPhysicsContext(), component);
}

void Release(Engine& engine, Components::RagdollComponent& component) {
    Release(engine.GetPhysicsContext(), component);
}

void Release(AudioContext& audio, Components::AudioSourceComponent& component) {
    audio.StopVoice(std::exchange(component.voiceHandle, AudioHandle::Invalid), component.fadeOut);
}

void Release(Engine& engine, Components::AudioSourceComponent& component) {
    Release(engine.GetAudioContext(), component);
}

void Release(Engine& engine, Components::OwnedMeshComponent& component) {
    Release(engine.GetRenderContext(), component);
}

void Release(Engine& engine, Components::SkeletalMeshComponent& component) {
    engine.GetRenderContext().DestroyBuffer(std::exchange(component.skinnedScratch, BufferHandle::Invalid));
    component.scratchVertexCount = 0;
}

} // namespace ZHLN::SceneResources
