// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <audio/Audio.hpp>
#include <engine/ArticulationContext.hpp>
#include <engine/SceneResources.hpp>
#include <engine/World.hpp>
#include <physics/Physics.hpp>
#include <render/RenderContext.hpp>
#include <span>
#include <utility>

namespace ZHLN::SceneResources {

void Release(PhysicsContext& physics, Components::StaticBodyComponent& component) {
    if (const auto id = std::exchange(component.bodyID, Physics::StaticBodyID::Null()); id != Physics::StaticBodyID::Null()) {
        physics.DestroyBody(id);
    }
}

void Release(PhysicsContext& physics, Components::KinematicBodyComponent& component) {
    if (const auto id = std::exchange(component.bodyID, Physics::KinematicBodyID::Null()); id != Physics::KinematicBodyID::Null()) {
        physics.DestroyBody(id);
    }
}

void Release(PhysicsContext& physics, Components::DynamicBodyComponent& component) {
    if (const auto id = std::exchange(component.bodyID, Physics::DynamicBodyID::Null()); id != Physics::DynamicBodyID::Null()) {
        physics.DestroyBody(id);
    }
}

void Release(PhysicsContext& physics, Components::CharacterBodyComponent& component) {
    if (const auto id = std::exchange(component.bodyID, Physics::CharacterID::Null()); id != Physics::CharacterID::Null()) {
        physics.DestroyBody(id);
    }
}

void Release(PhysicsContext& physics, Components::RagdollComponent& component) {
    component.isAddedToPhysics = false;
    if (const auto id = std::exchange(component.ragdollID, Physics::RagdollID::Invalid); id != Physics::RagdollID::Invalid) {
        physics.DestroyRagdoll(id);
    }
}

void Release(Engine& engine, Components::StaticBodyComponent& component) {
    Release(engine.Physics(), component);
}

void Release(Engine& engine, Components::KinematicBodyComponent& component) {
    Release(engine.Physics(), component);
}

void Release(Engine& engine, Components::DynamicBodyComponent& component) {
    Release(engine.Physics(), component);
}

void Release(Engine& engine, Components::CharacterBodyComponent& component) {
    Release(engine.Physics(), component);
}

void Release(Engine& engine, Components::RagdollComponent& component) {
    // Drop per-ragdoll CPU state before releasing the physics ragdoll; guard on
    // Invalid so repeated retirement (e.g. SceneCleanup followed by Registry::Clear)
    // is a guaranteed no-op.
    component.isAddedToPhysics = false;
    if (const auto id = std::exchange(component.ragdollID, Physics::RagdollID::Invalid); id != Physics::RagdollID::Invalid) {
        engine.GetWorld().GetArticulationContext().RemoveJointState(id);
        engine.Physics().DestroyRagdoll(id);
    }
}

void Release(AudioContext& audio, Components::AudioSourceComponent& component) {
    if (const auto id = std::exchange(component.voiceID, VoiceID::Invalid); id != VoiceID::Invalid) {
        audio.StopVoice(id, component.fadeOut);
    }
}

void Release(Engine& engine, Components::AudioSourceComponent& component) {
    Release(engine.Audio(), component);
}

void Release(Engine& engine, Components::SkeletalMeshComponent& component) {
    if (component.jointCount > 0) {
        engine.Render().FreeJoints(component.jointOffset, component.jointCount);
        component.jointOffset = 0;
        component.jointCount  = 0;
    }
    if (const auto scratch = std::exchange(component.skinnedScratch, BufferID::Invalid); scratch != BufferID::Invalid) {
        engine.Render().DestroyBuffer(scratch);
    }
    component.scratchVertexCount = 0;
}

void Release(Engine& engine, Components::MorphTargetComponent& component) {
    if (component.morphDeltaCount > 0) {
        engine.Render().FreeMorphDeltas(component.offset, component.morphDeltaCount);
        component.offset          = 0;
        component.morphDeltaCount = 0;
    }
}

} // namespace ZHLN::SceneResources
