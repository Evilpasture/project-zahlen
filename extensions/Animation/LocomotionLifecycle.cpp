// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
// clang-format off
#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Physics/Jolt.hpp>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Camera/TargetCamera.hpp>
#include <CharacterController/CharacterComponents.hpp>
#include <FreeCam/FreeCam.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

#include <Zahlen/Engine/Capabilities/Entities.hpp>

module ZHLN.Locomotion;

namespace ZHLN::Locomotion {
auto SpawnCharacter(Engine& engine, const FVec3& spawnPosition, const Physics::DualShapeConfig& config, float speed, float jumpForce) -> Entity {
    auto entities = engine.Entities();
    auto pc = engine.Physics();

    // Create physics first, then publish the complete gameplay entity in one
    // atomic ECS insertion. No system can observe a partially assembled player.
    // The two-part hull is authored here (a gameplay decision) and passed to
    // the generic core API; the supporting volume keeps the whole lower
    // lifter sphere in contact with the ground.
    const Physics::CharacterParams characterParams {
        .shape            = Physics::CreateDualShape(config),
        .supportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -config.GetLifterOffsetY())
    };
    const Physics::BodyID charPhys = pc.CreateCharacter({static_cast<double>(spawnPosition.x), static_cast<double>(spawnPosition.y), static_cast<double>(spawnPosition.z)}, characterParams);
    const auto world = ComposeTransform(spawnPosition, Quat::Identity(), FVec3::One());
    const Entity              player   = entities.Spawn(
        Components::PlayerTagComponent {}, Components::NameComponent {.name = String64("Player_VirtualCharacter")},
        Components::TransformComponent {.position = spawnPosition}, Components::WorldTransformComponent {.world = world, .previous = world},
        Character::InputComponent {}, Character::MovementComponent {.speed = speed, .jumpForce = jumpForce},
        Components::PhysicsComponent {.physicsID = charPhys, .isStatic = false}
    );
    // Configure third-person follow camera and strip FreeCam. The rig
    // component lives in extensions/Camera: this is gameplay camera policy the
    // spawner authors explicitly (a host that wants no follow camera removes
    // the component afterwards; nothing in core re-creates one it was given).
    FreeCam::Detach(engine);
    for (Entity camEnt: entities.EntitiesWith<Components::MainCameraTagComponent>()) {
        entities.Attach(
            camEnt,
            CameraRig::TargetCameraComponent {
                .target            = player,
                .distance          = 5.50f,
                .targetDistance    = 5.50f,
                .yaw               = 90.0f,
                .pitch             = -14.0f,
                .targetOffset      = FVec3{0.0f, 0.80f, 0.0f},
                .stiffness         = 24.0f,
                .vignetteIntensity = 1.10f,
                .vignettePower     = 1.50f,
                .fov               = 52.0f,
                .targetFov         = 52.0f
            },
            Character::InputComponent {}
        );
    }

    return player;
}
}
