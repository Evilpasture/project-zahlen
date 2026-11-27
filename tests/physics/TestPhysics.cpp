// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ILogicWorld.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/COM/LogicWorldFactory.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

namespace {

using ZHLN::DVec3;
using ZHLN::FVec3;

[[nodiscard]] auto MakePhysicsWorld() -> ZHLN::COM::Ref<ZHLN::COM::ILogicWorld> {
    auto res = ZHLN::COM::CreateLogicWorld();
    ZHLN::Test::ExpectTrue(res.has_value());
    return std::move(*res);
}

struct PhysicsTestSuite {
    ZHLN::TaskSystem::Scope taskScope {2, 32};

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> dynamic_body_falls_and_settles_on_static_floor() {
            auto  world = MakePhysicsWorld();
            auto& sim   = *world->Simulation();

            const auto floor = sim.CreateStaticBodyRaw(ZHLN::COM::InChain(
                ZHLN::Physics::Box {.halfExtents = {20.0f, 0.5f, 20.0f}},
                ZHLN::Physics::StaticBody {.position = DVec3 {0.0, -0.5, 0.0}}
            ));
            const auto box = sim.CreateBoxBody(
                FVec3 {0.5f, 0.5f, 0.5f},
                ZHLN::Physics::DynamicBody {.position = DVec3 {0.0, 4.0, 0.0}}
            );
            ZHLN::Test::ExpectTrue(floor.HasValue() && box.HasValue());
            ZHLN::Test::ExpectFalse(sim.IsBodyDynamic(floor.value));
            ZHLN::Test::ExpectTrue(sim.IsBodyDynamic(box.value));

            for (int i = 0; i < 180; ++i) {
                sim.Step(1.0f / 60.0f);
            }

            const auto pos = sim.BodyPosition(box.value);
            ZHLN::Test::ExpectTrue(pos.HasValue());
            ZHLN::Test::ExpectTrue(pos.value.y > 0.40 && pos.value.y < 0.65);

            const auto state = sim.BodyState(box.value);
            ZHLN::Test::ExpectTrue(state.HasValue());
            ZHLN::Test::ExpectTrue(std::abs(state.value.currentPosition.y - pos.value.y) < 1e-4);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> impulses_linear_velocity_and_kinematic_pose() {
            auto  world = MakePhysicsWorld();
            auto& sim   = *world->Simulation();

            const auto sphere = sim.CreateSphereBody(
                0.5f, ZHLN::Physics::DynamicBody {.position = DVec3 {0.0, 10.0, 0.0}}
            );
            ZHLN::Test::ExpectTrue(sphere.HasValue());
            sim.SetVelocity(sphere.value, FVec3 {6.0f, 0.0f, 0.0f});
            sim.Step(1.0f / 60.0f);
            const auto vel = sim.BodyLinearVelocity(sphere.value);
            ZHLN::Test::ExpectTrue(vel.HasValue() && std::abs(vel.value.x - 6.0f) < 0.1f);
            const auto moved = sim.BodyPosition(sphere.value);
            ZHLN::Test::ExpectTrue(moved.HasValue() && moved.value.x > 0.05);

            sim.AddImpulse(sphere.value, FVec3 {0.0f, 0.0f, 20.0f});
            sim.Step(1.0f / 60.0f);
            const auto afterImpulse = sim.BodyLinearVelocity(sphere.value);
            ZHLN::Test::ExpectTrue(afterImpulse.HasValue() && afterImpulse.value.z > 0.0f);

            const auto kinematic = sim.CreateKinematicBodyRaw(ZHLN::COM::InChain(
                ZHLN::Physics::Box {.halfExtents = {1.0f, 1.0f, 1.0f}},
                ZHLN::Physics::KinematicBody {.position = DVec3 {0.0, 0.0, 0.0}}
            ));
            ZHLN::Test::ExpectTrue(kinematic.HasValue());
            sim.SetPose(kinematic.value, DVec3 {3.0, 5.0, -2.0}, ZHLN::Quat::Identity());
            sim.Step(1.0f / 60.0f);
            const auto kinPos = sim.BodyPosition(kinematic.value);
            ZHLN::Test::ExpectTrue(kinPos.HasValue() && std::abs(kinPos.value.x - 3.0) < 0.1);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> raycast_sphere_query_and_batch_destroy() {
            auto  world = MakePhysicsWorld();
            auto& sim   = *world->Simulation();

            const auto target = sim.CreateStaticBodyRaw(ZHLN::COM::InChain(
                ZHLN::Physics::Box {.halfExtents = {1.0f, 1.0f, 1.0f}},
                ZHLN::Physics::StaticBody {.position = DVec3 {0.0, 0.0, 5.0}}
            ));
            ZHLN::Test::ExpectTrue(target.HasValue());

            const auto hit = sim.Raycast(DVec3 {0.0, 0.0, 0.0}, FVec3 {0.0f, 0.0f, 1.0f}, 20.0f, ZHLN::Physics::BodyID::Null());
            ZHLN::Test::ExpectTrue(hit.hasHit);
            ZHLN::Test::ExpectEq(hit.bodyID, target.value);
            ZHLN::Test::ExpectTrue(std::abs(hit.position.z - 4.0) < 0.05);

            // Ignoring the target body misses cleanly.
            const auto ignored = sim.Raycast(DVec3 {0.0, 0.0, 0.0}, FVec3 {0.0f, 0.0f, 1.0f}, 20.0f, target.value);
            ZHLN::Test::ExpectFalse(ignored.hasHit);

            const auto nearby = sim.BodiesInSphere(DVec3 {0.0, 0.0, 5.0}, 2.5f);
            ZHLN::Test::ExpectEq(nearby.size(), 1u);

            const std::array<ZHLN::Physics::BodyID, 1> toDestroy {ZHLN::Physics::BodyID(target.value)};
                        sim.DestroyBodies(ZHLN::COM::Span<const ZHLN::Physics::BodyID>(toDestroy.data(), toDestroy.size()));
            ZHLN::Test::ExpectFalse(sim.BodyPosition(target.value).HasValue());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> dual_shape_character_creation_grounding_and_teleport() {
            auto  world = MakePhysicsWorld();
            auto& sim   = *world->Simulation();

            const auto floor = sim.CreateStaticBodyRaw(ZHLN::COM::InChain(
                ZHLN::Physics::Box {.halfExtents = {20.0f, 0.5f, 20.0f}},
                ZHLN::Physics::StaticBody {.position = DVec3 {0.0, -0.5, 0.0}}
            ));
            ZHLN::Test::ExpectTrue(floor.HasValue());

            const ZHLN::Physics::DualShape hull {};
            const auto character = sim.CreateCharacterRaw(ZHLN::COM::InChain(
                hull,
                ZHLN::Physics::CharacterBody {.position = DVec3 {0.0, 0.15, 0.0}}
            ));
            ZHLN::Test::ExpectTrue(character.HasValue());
            ZHLN::Test::ExpectTrue(sim.CharacterGroundOffset(character.value) >= 0.0f);

            sim.SetVelocity(character.value, FVec3 {2.0f, -4.0f, 0.0f});
            for (int i = 0; i < 30; ++i) {
                sim.Step(1.0f / 60.0f);
            }
            ZHLN::Test::ExpectTrue(sim.IsCharacterOnGround(character.value));
            const auto pos = sim.BodyPosition(character.value);
            ZHLN::Test::ExpectTrue(pos.HasValue() && pos.value.x > 0.5);

            sim.SetPose(character.value, DVec3 {-4.0, 2.0, 3.0});
            sim.SetVelocity(character.value, FVec3 {0.0f, 0.0f, 0.0f});
            sim.Step(1.0f / 60.0f);
            const auto teleported = sim.BodyPosition(character.value);
            ZHLN::Test::ExpectTrue(teleported.HasValue() && std::abs(teleported.value.x - (-4.0)) < 1e-3);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> mesh_shape_and_material_registration() {
            auto  world = MakePhysicsWorld();
            auto& sim   = *world->Simulation();

            const auto materialID = sim.RegisterMaterial(ZHLN::Physics::SurfaceMaterial {.friction = 0.8f, .restitution = 0.2f});
            ZHLN::Test::ExpectEq(static_cast<uint32_t>(materialID), 1u);
            // The same fingerprint reuses the handle.
            const auto again = sim.RegisterMaterial(ZHLN::Physics::SurfaceMaterial {.friction = 0.8f, .restitution = 0.2f});
            ZHLN::Test::ExpectEq(static_cast<uint32_t>(again), static_cast<uint32_t>(materialID));
            const std::array<FVec3, 4> verts {
                FVec3 {-5.0f, 0.0f, -5.0f},
                FVec3 { 5.0f, 0.0f, -5.0f},
                FVec3 { 5.0f, 0.0f,  5.0f},
                FVec3 {-5.0f, 0.0f,  5.0f},
            };
            const std::array<uint32_t, 6> indices {0, 2, 1, 0, 3, 2};
            const ZHLN::FVec3 v[] = {{-5.0f, 0.0f, -5.0f}, {5.0f, 0.0f, -5.0f}, {5.0f, 0.0f, 5.0f}, {-5.0f, 0.0f, 5.0f}};
            const uint32_t idx[]  = {0, 2, 1, 0, 3, 2};
            ZHLN::Physics::TriangleMesh meshDesc {};
            meshDesc.vertices = {v, 4};
            meshDesc.indices  = {idx, 6};
            const auto meshBody = sim.CreateStaticBodyRaw(ZHLN::COM::InChain(
                meshDesc,
                ZHLN::Physics::StaticBody {}
            ));
            ZHLN::Test::ExpectTrue(meshBody.HasValue());
            const auto hit = sim.Raycast({0.0, 4.0, 0.0}, {0.0f, -1.0f, 0.0f}, 10.0f, ZHLN::Physics::BodyID::Null());
            ZHLN::Test::ExpectTrue(hit.hasHit);
            sim.ClearMeshShapeCache();
            return {};
        }
    };
};

} // namespace
