// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <Animation/Components.hpp>
#include <Animation/PrefabRig.hpp>
#include <Camera/TargetCamera.hpp>
#include <CharacterController/CharacterController.hpp>
#include <FreeCam/FreeCam.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/COM/ILogger.hpp>
#include <algorithm>
#include <cmath>
#include <vector>
#include <Zahlen/COM/LoggerView.hpp>


module ZHLN.Locomotion;

namespace ZHLN::Locomotion {

namespace {

[[nodiscard]] auto CreateCharacterBody(COM::ISimulation& sim, FVec3 footPosition, const DualShape& config) -> Physics::CharacterID {
    const Physics::CharacterBody role {
        .position                 = footPosition.Cast<double>(),
        .maxSlopeAngle            = DegF {50.0f}.ToRadians(),
        .maxStrength              = 120.0f,
        .padding                  = 0.02f,
        .penetrationRecoverySpeed = 1.0f,
        // The lifter centre sits one radius above the sole; contacts up to it
        // count as floor support (the plane the DualShape recipe always
        // used), spelled explicitly now that the default is bounds-derived.
        .supportingVolume = Physics::Plane {{0.0f, 1.0f, 0.0f}, -config.GetLifterOffsetY()},
    };
    const auto res = sim.CreateBody(MakeDualCollider(config), role);
    return res.HasValue() ? res.value : Physics::CharacterID::Null();
}

[[nodiscard]] auto FindActiveCamera(const COM::IEntities& entities) -> Entity {
    const auto cameras = entities.EntitiesWith<Components::MainCameraTagComponent>();
    return cameras.size() > 0 ? cameras[0] : Entity::Null();
}

void DespawnHierarchy(COM::IEntities& entities, Entity root) {
    std::vector<Entity> stack {root};
    std::vector<Entity> order;
    while (!stack.empty()) {
        const Entity current = stack.back();
        stack.pop_back();
        order.push_back(current);
        for (const Entity child : entities.EntitiesWith<Components::HierarchyComponent>()) {
            if (const auto* hier = entities.Get<Components::HierarchyComponent>(child);
                hier != nullptr && hier->parent == current) {
                stack.push_back(child);
            }
        }
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (entities.IsAlive(*it)) entities.Despawn(*it);
    }
}

} // namespace

auto SpawnCharacter(
    COM::IEngine&         engine,
    AssetID               prefabHash,
    std::string_view      modelVirtualPath,
    FVec3                 spawnFootPosition,
    DegF                  initialYaw,
    const CharacterSetup& setup) -> CharacterSpawnResult {
    auto* entities = engine.Entities();
    auto* assets   = engine.Assets();
    auto* sim      = engine.Simulation();
    if (entities == nullptr || assets == nullptr || sim == nullptr) return {};

    if (prefabHash == InvalidAssetID) {
        prefabHash = assets->PrefabHash(COM::StringView {modelVirtualPath.data(), modelVirtualPath.size()});
    }
    const auto rigOpt = Animation::LoadPrefabRig(*assets, prefabHash);
    if (!rigOpt.has_value()) {
        ZHLN::COM::LoggerView{engine.Logger()}.Error("Cannot spawn character: prefab '{}' is not cached.", modelVirtualPath);
        return {};
    }
    const Animation::PrefabRig& prefab = *rigOpt;

    const PrefabBounds unscaledBounds = MeasurePrefabBounds(prefab);
    const float        unscaledHeight = unscaledBounds.GetSize().y;
    const float        uniformScale   = (unscaledBounds.valid && unscaledHeight > 1.0e-4f && setup.targetHeight > 0.0f)
                                            ? setup.targetHeight / unscaledHeight
                                            : 1.0f;
    const FVec3        scale          = FVec3::Splat(uniformScale);
    const PrefabBounds scaledBounds   = MeasurePrefabBounds(prefab, scale);
    const float        groundOffset   = scaledBounds.valid ? std::max(0.0f, -scaledBounds.min.y) : 0.0f;
    const FVec3        entityPosition = spawnFootPosition + FVec3 {0.0f, groundOffset, 0.0f};
    const auto         rotation       = Quat::FromAxisAngle(FVec3::UnitY(), initialYaw.ToRadians());

    const PrefabFactory::SpawnParams spawnParams {
        .position      = entityPosition.Cast<double>(),
        .rotation      = rotation,
        .scale         = scale,
        .createPhysics = false,
        .isAnimated    = true,
    };
    const auto spawned = assets->Instantiate(prefabHash, spawnParams);
    if (spawned.empty() || spawned.front() == Entity::Null()) {
        ZHLN::COM::LoggerView{engine.Logger()}.Error("Failed to spawn character entity for '{}'.", modelVirtualPath);
        return {};
    }
    const Entity entity = spawned.front();

    const DualShape            shapeConfig = FitDualShapeToBounds(scaledBounds, setup);
    const Physics::CharacterID characterID = CreateCharacterBody(*sim, spawnFootPosition, shapeConfig);
    const float                sprintMult  = setup.walkSpeed > 1.0e-4f ? setup.sprintSpeed / setup.walkSpeed : 1.65f;
    entities->Attach(entity, Components::CharacterBodyComponent {.bodyID = characterID});
    Character::Attach(
        *entities,
        entity,
        Character::MovementComponent {
            .orientation      = rotation,
            .prevOrientation  = rotation,
            .speed            = setup.walkSpeed,
            .sprintMultiplier = sprintMult,
            .jumpForce        = setup.jumpVelocity,
        },
        Character::ControllerComponent {});

    if ((!prefab.animations.empty() || !prefab.skeletons.empty()) && !entities->Has<Animation::AnimatorComponent>(entity)) {
        entities->Attach(entity, Animation::AnimatorComponent {
            .prefabAsset     = prefabHash,
            .currentTrackIdx = prefab.animations.empty() ? -1 : 0,
        });
    }

    return CharacterSpawnResult {
        .entity        = entity,
        .scale         = scale,
        .boundsSize    = scaledBounds.GetSize(),
        .height        = scaledBounds.valid ? scaledBounds.GetSize().y : setup.targetHeight,
        .groundOffset  = groundOffset,
        .colliderShape = shapeConfig,
        .valid         = true,
    };
}

auto SpawnCharacter(
    COM::IEngine&         engine,
    std::string_view      modelVirtualPath,
    FVec3                 spawnFootPosition,
    DegF                  initialYaw,
    const CharacterSetup& setup) -> CharacterSpawnResult {
    auto* assets = engine.Assets();
    if (assets == nullptr) return {};
    const auto hashRes = assets->FindPrefab(COM::StringView {modelVirtualPath.data(), modelVirtualPath.size()});
    if (!hashRes.HasValue()) {
        ZHLN::COM::LoggerView{engine.Logger()}.Error("Prefab '{}' is not cached (import it first).", modelVirtualPath);
        return {};
    }
    return SpawnCharacter(engine, hashRes.value, modelVirtualPath, spawnFootPosition, initialYaw, setup);
}

void DestroyCharacter(COM::IEngine& engine, Entity characterEntity) {
    auto* entities = engine.Entities();
    if (entities == nullptr || !entities->IsAlive(characterEntity)) return;

    const Entity activeCam = FindActiveCamera(*entities);
    if (const auto* rig = entities->Get<CameraRig::TargetComponent>(activeCam);
        rig != nullptr && rig->target == characterEntity) {
        entities->Detach<CameraRig::TargetComponent>(activeCam);
    }
    if (const auto* physicsComp = entities->Get<Components::CharacterBodyComponent>(characterEntity);
        physicsComp != nullptr && physicsComp->bodyID != Physics::CharacterID::Null()) {
        if (auto* sim = engine.Simulation()) {
            const Physics::BodyID id = physicsComp->bodyID;
            sim->DestroyBodies(COM::Span<const Physics::BodyID> {&id, 1});
        }
    }
    DespawnHierarchy(*entities, characterEntity);
}

void SwitchToFollowCamera(COM::IEngine& engine, Entity characterEntity, const CharacterSetup& setup) {
    auto* entities = engine.Entities();
    if (entities == nullptr) return;
    FreeCam::Detach(*entities);

    const Entity activeCam = FindActiveCamera(*entities);
    const auto*  camComp   = entities->Get<Components::CameraComponent>(activeCam);
    if (camComp == nullptr) return;

    CameraRig::TargetComponent rig {
        .target         = characterEntity,
        .distance       = setup.cameraDistance,
        .targetDistance = setup.cameraDistance,
        .yaw            = camComp->camera.yaw.value,
        .pitch          = std::clamp(camComp->camera.pitch.value, -25.0f, 15.0f),
        .targetOffset   = {setup.cameraShoulderOffset, setup.cameraHeightOffset, 0.0f},
        .stiffness      = setup.cameraPositionSmoothing,
    };

    if (const auto* transform = entities->Get<Components::TransformComponent>(characterEntity)) {
        const FVec3 forward = transform->rotation * FVec3::UnitZ();
        if (std::hypot(forward.x, forward.z) > 1.0e-4f) {
            rig.yaw   = Atan2(forward.z, forward.x).ToDegrees().value;
            rig.pitch = -12.0f;
        }
        rig.smoothTargetPos     = transform->position + FVec3 {0.0f, rig.targetOffset.y, 0.0f};
        rig.hasInitSmoothTarget = 1u;
    }
    entities->Attach(activeCam, rig);
}

void RecenterFollowCamera(COM::IEngine& engine, Entity characterEntity) {
    auto* entities = engine.Entities();
    if (entities == nullptr) return;
    const Entity activeCam = FindActiveCamera(*entities);
    auto*        rig       = entities->Get<CameraRig::TargetComponent>(activeCam);
    const auto*  transform = entities->Get<Components::TransformComponent>(characterEntity);
    if (rig == nullptr || rig->target != characterEntity || transform == nullptr) return;
    const FVec3 forward = transform->rotation * FVec3::UnitZ();
    if (std::hypot(forward.x, forward.z) > 1.0e-4f) {
        rig->yaw = Atan2(forward.z, forward.x).ToDegrees().value;
    }
}

} // namespace ZHLN::Locomotion
