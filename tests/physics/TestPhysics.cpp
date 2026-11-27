// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ILogicWorld.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/COM/LogicWorldFactory.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Physics/PhysicsShapes.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <numbers>
#include <vector>

namespace {

using ZHLN::DVec3;
using ZHLN::FVec3;

// Compile-time modifier gating & FixedCompound lifetime checks.
static_assert([]<typename B = ZHLN::Physics::Box> {
    return !requires { ZHLN::Physics::MakeCompound(ZHLN::Physics::Placed {B {}}).MakeNodes(); };
}(), "FixedCompound::MakeNodes() must reject rvalues so spans never dangle.");

static_assert([]<typename Sim = ZHLN::COM::ISimulation> {
    return requires(Sim& s, ZHLN::Physics::PhysicsMaterialID mat) {
        s.CreateBody(ZHLN::Physics::Box {}, ZHLN::Physics::StaticBody {}, mat, ZHLN::Physics::MovingBroadphase {});
        s.CreateBody(ZHLN::Physics::Box {}, ZHLN::Physics::DynamicBody {}, mat);
        s.CreateBody(ZHLN::Physics::Box {}, ZHLN::Physics::KinematicBody {}, mat);
        s.CreateBody(ZHLN::Physics::Capsule {}, ZHLN::Physics::CharacterBody {}, ZHLN::Physics::CollisionFilter {});
    };
}());

static_assert([]<typename Sim = ZHLN::COM::ISimulation> {
    return !requires(Sim& s, ZHLN::Physics::SurfaceMaterial surf, ZHLN::Physics::PhysicsMaterialID mat) {
        s.CreateBody(ZHLN::Physics::Box {}, ZHLN::Physics::DynamicBody {}, surf, mat);
    };
}());
static_assert([]<typename Sim = ZHLN::COM::ISimulation> {
    return !requires(Sim& s) {
        s.CreateBody(ZHLN::Physics::Box {}, ZHLN::Physics::DynamicBody {}, ZHLN::Physics::MovingBroadphase {});
    };
}());
static_assert([]<typename Sim = ZHLN::COM::ISimulation> {
    return !requires(Sim& s) {
        s.CreateBody(ZHLN::Physics::Box {}, ZHLN::Physics::KinematicBody {}, ZHLN::Physics::MovingBroadphase {});
    };
}());
static_assert([]<typename Sim = ZHLN::COM::ISimulation> {
    return !requires(Sim& s, ZHLN::Physics::SurfaceMaterial surf) {
        s.CreateBody(ZHLN::Physics::Capsule {}, ZHLN::Physics::CharacterBody {}, surf);
    };
}());
static_assert([]<typename Sim = ZHLN::COM::ISimulation> {
    return !requires(Sim& s, ZHLN::Physics::PhysicsMaterialID mat) {
        s.CreateBody(ZHLN::Physics::Capsule {}, ZHLN::Physics::CharacterBody {}, mat);
    };
}());

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

            const auto floor = sim.CreateBody(
                ZHLN::Physics::Box {.halfExtents = {20.0f, 0.5f, 20.0f}},
                ZHLN::Physics::StaticBody {.position = DVec3 {0.0, -0.5, 0.0}}
            );
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
            ZHLN::Test::ExpectTrue(std::abs(static_cast<double>(state.value.currentPosition.y) - pos.value.y) < 1e-4);
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

            const auto kinematic = sim.CreateBody(
                ZHLN::Physics::Box {.halfExtents = {1.0f, 1.0f, 1.0f}},
                ZHLN::Physics::KinematicBody {.position = DVec3 {0.0, 0.0, 0.0}}
            );
            ZHLN::Test::ExpectTrue(kinematic.HasValue());
            const ZHLN::Quat targetRot = ZHLN::Quat::FromAxisAngle(FVec3 {0.0f, 1.0f, 0.0f}, ZHLN::RadF {std::numbers::pi_v<float> * 0.5f});
            sim.SetPose(kinematic.value, DVec3 {3.0, 5.0, -2.0}, targetRot);
            sim.Step(1.0f / 60.0f);
            const auto kinState = sim.BodyState(kinematic.value);
            ZHLN::Test::ExpectTrue(kinState.HasValue() && std::abs(kinState.value.currentPosition.x - 3.0f) < 0.1f);
            if (kinState.HasValue()) {
                const ZHLN::Quat actualRot {kinState.value.currentRotation};
                const float      alignment = std::abs(actualRot.x * targetRot.x + actualRot.y * targetRot.y +
                                                      actualRot.z * targetRot.z + actualRot.w * targetRot.w);
                ZHLN::Test::ExpectTrue(alignment > 0.999f);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> raycast_sphere_query_and_batch_destroy() {
            auto  world = MakePhysicsWorld();
            auto& sim   = *world->Simulation();

            const auto target = sim.CreateBody(
                ZHLN::Physics::Box {.halfExtents = {1.0f, 1.0f, 1.0f}},
                ZHLN::Physics::StaticBody {.position = DVec3 {0.0, 0.0, 5.0}}
            );
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

            const auto floor = sim.CreateBody(
                ZHLN::Physics::Box {.halfExtents = {20.0f, 0.5f, 20.0f}},
                ZHLN::Physics::StaticBody {.position = DVec3 {0.0, -0.5, 0.0}}
            );
            ZHLN::Test::ExpectTrue(floor.HasValue());

            // Placed CTAD deduces Placed<Sphere> and Placed<Capsule> cleanly.
            const auto hull = ZHLN::Physics::MakeCompound(
                ZHLN::Physics::Placed {ZHLN::Physics::Sphere {0.40F}, {0.0F, 0.40F, 0.0F}},
                ZHLN::Physics::Placed {ZHLN::Physics::Capsule {0.20F, 0.50F}, {0.0F, 0.82F, 0.0F}});
            const auto character = sim.CreateBody(
                hull,
                ZHLN::Physics::CharacterBody {
                    .position         = DVec3 {0.0, 0.15, 0.0},
                    .supportingVolume = ZHLN::Physics::Plane {{0.0f, 1.0f, 0.0f}, -0.40F},
                });
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

            const auto capsuleChar = sim.CreateCapsuleCharacter(0.6f, 0.3f, ZHLN::Physics::CharacterBody {.position = DVec3 {2.0, 1.5, 0.0}});
            ZHLN::Test::ExpectTrue(capsuleChar.HasValue() && capsuleChar.value != ZHLN::Physics::CharacterID::Null());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> mesh_shape_and_material_registration() {
            auto  world = MakePhysicsWorld();
            auto& sim   = *world->Simulation();

            // Jolt's default (0.2, 0.0) resolves straight to PhysicsMaterialID::Default (0)
            // without consuming a new material table slot.
            const auto defaultMat = sim.RegisterMaterial(ZHLN::Physics::SurfaceMaterial {.friction = 0.2f, .restitution = 0.0f});
            ZHLN::Test::ExpectEq(static_cast<uint32_t>(defaultMat), static_cast<uint32_t>(ZHLN::Physics::PhysicsMaterialID::Default));
            ZHLN::Test::ExpectEq(static_cast<uint32_t>(defaultMat), 0u);

            const auto materialID = sim.RegisterMaterial(ZHLN::Physics::SurfaceMaterial {.friction = 0.8f, .restitution = 0.2f});
            ZHLN::Test::ExpectEq(static_cast<uint32_t>(materialID), 1u);
            // The same fingerprint reuses the handle.
            const auto again = sim.RegisterMaterial(ZHLN::Physics::SurfaceMaterial {.friction = 0.8f, .restitution = 0.2f});
            ZHLN::Test::ExpectEq(static_cast<uint32_t>(again), static_cast<uint32_t>(materialID));

            // Pre-registered PhysicsMaterialID works directly on Static, Kinematic, and Dynamic bodies.
            const auto matStatic = sim.CreateBody(
                ZHLN::Physics::Box {{1.0f, 0.5f, 1.0f}}, ZHLN::Physics::StaticBody {.position = {0.0, -2.0, 0.0}}, materialID);
            const auto matDynamic = sim.CreateBody(
                ZHLN::Physics::Sphere {0.4f}, ZHLN::Physics::DynamicBody {.position = {0.0, 6.0, 0.0}}, materialID);
            ZHLN::Test::ExpectTrue(matStatic.HasValue() && matDynamic.HasValue());

            const ZHLN::FVec3 v[] = {{-5.0f, 0.0f, -5.0f}, {5.0f, 0.0f, -5.0f}, {5.0f, 0.0f, 5.0f}, {-5.0f, 0.0f, 5.0f}};
            const uint32_t idx[]  = {0, 2, 1, 0, 3, 2};
            ZHLN::Physics::TriangleMesh meshDesc {};
            meshDesc.vertices = {v, 4};
            meshDesc.indices  = {idx, 6};
            const auto meshBody = sim.CreateBody(meshDesc, ZHLN::Physics::StaticBody {});
            ZHLN::Test::ExpectTrue(meshBody.HasValue());
            const auto hit = sim.Raycast({0.0, 4.0, 0.0}, {0.0f, -1.0f, 0.0f}, 10.0f, ZHLN::Physics::BodyID::Null());
            ZHLN::Test::ExpectTrue(hit.hasHit && hit.bodyID == meshBody.value);
            sim.ClearMeshShapeCache();
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> modifier_gating_and_nested_mesh_asset_validation() {
            auto  world = MakePhysicsWorld();
            auto& sim   = *world->Simulation();

            const auto matID = sim.RegisterMaterial(ZHLN::Physics::SurfaceMaterial {.friction = 0.6f, .restitution = 0.1f});

            // Duplicate material specification (SurfaceMaterial + PhysicsMaterialID) -> DuplicateExtension.
            const auto dupStatic = sim.CreateStaticBodyRaw(ZHLN::COM::InChain(
                ZHLN::Physics::Box {}, ZHLN::Physics::StaticBody {}, ZHLN::Physics::SurfaceMaterial {}, matID));
            ZHLN::Test::ExpectTrue(!dupStatic.HasValue() && dupStatic.error == ZHLN::ErrorCode(ZHLN::COM::Error::DuplicateExtension));

            const auto dupDynamic = sim.CreateDynamicBodyRaw(ZHLN::COM::InChain(
                ZHLN::Physics::Box {}, ZHLN::Physics::DynamicBody {}, ZHLN::Physics::SurfaceMaterial {}, matID));
            ZHLN::Test::ExpectTrue(!dupDynamic.HasValue() && dupDynamic.error == ZHLN::ErrorCode(ZHLN::COM::Error::DuplicateExtension));

            // MovingBroadphase is static-only; Kinematic, Dynamic, and Character reject it.
            const auto movKin = sim.CreateKinematicBodyRaw(ZHLN::COM::InChain(
                ZHLN::Physics::Box {}, ZHLN::Physics::KinematicBody {}, ZHLN::Physics::MovingBroadphase {}));
            ZHLN::Test::ExpectTrue(!movKin.HasValue() && movKin.error == ZHLN::ErrorCode(ZHLN::COM::Error::UnsupportedExtension));

            const auto movDyn = sim.CreateDynamicBodyRaw(ZHLN::COM::InChain(
                ZHLN::Physics::Box {}, ZHLN::Physics::DynamicBody {}, ZHLN::Physics::MovingBroadphase {}));
            ZHLN::Test::ExpectTrue(!movDyn.HasValue() && movDyn.error == ZHLN::ErrorCode(ZHLN::COM::Error::UnsupportedExtension));

            // SurfaceMaterial / PhysicsMaterialID on Character -> UnsupportedExtension.
            const auto charSurf = sim.CreateCharacterRaw(ZHLN::COM::InChain(
                ZHLN::Physics::Capsule {}, ZHLN::Physics::CharacterBody {}, ZHLN::Physics::SurfaceMaterial {}));
            ZHLN::Test::ExpectTrue(!charSurf.HasValue() && charSurf.error == ZHLN::ErrorCode(ZHLN::COM::Error::UnsupportedExtension));

            const auto charMat = sim.CreateCharacterRaw(ZHLN::COM::InChain(
                ZHLN::Physics::Capsule {}, ZHLN::Physics::CharacterBody {}, matID));
            ZHLN::Test::ExpectTrue(!charMat.HasValue() && charMat.error == ZHLN::ErrorCode(ZHLN::COM::Error::UnsupportedExtension));

            // Top-level and nested MeshAsset validation: Invalid ID -> InvalidArgument, uncached valid ID -> AssetNotCached.
            const auto badTopMesh = sim.CreateBody(
                ZHLN::Physics::MeshAsset {.id = ZHLN::CollisionAssetID::Invalid}, ZHLN::Physics::StaticBody {});
            ZHLN::Test::ExpectTrue(!badTopMesh.HasValue() && badTopMesh.error == ZHLN::ErrorCode(ZHLN::COM::Error::InvalidArgument));

            const auto uncachedTopMesh = sim.CreateBody(
                ZHLN::Physics::MeshAsset {.id = static_cast<ZHLN::CollisionAssetID>(99u)}, ZHLN::Physics::StaticBody {});
            ZHLN::Test::ExpectTrue(!uncachedTopMesh.HasValue() && uncachedTopMesh.error == ZHLN::ErrorCode(ZHLN::COM::Error::AssetNotCached));

            const auto invalidNestedCompound = ZHLN::Physics::MakeCompound(
                ZHLN::Physics::Placed {ZHLN::Physics::MeshAsset {.id = ZHLN::CollisionAssetID::Invalid}});
            const auto badNestedRes = sim.CreateBody(invalidNestedCompound, ZHLN::Physics::StaticBody {});
            ZHLN::Test::ExpectTrue(!badNestedRes.HasValue() && badNestedRes.error == ZHLN::ErrorCode(ZHLN::COM::Error::InvalidArgument));

            const auto uncachedNestedCompound = ZHLN::Physics::MakeCompound(
                ZHLN::Physics::Placed {ZHLN::Physics::Box {{0.5f, 0.5f, 0.5f}}},
                ZHLN::Physics::Placed {ZHLN::Physics::MeshAsset {.id = static_cast<ZHLN::CollisionAssetID>(77u)}});
            const auto uncachedNestedRes = sim.CreateBody(uncachedNestedCompound, ZHLN::Physics::StaticBody {});
            ZHLN::Test::ExpectTrue(!uncachedNestedRes.HasValue() && uncachedNestedRes.error == ZHLN::ErrorCode(ZHLN::COM::Error::AssetNotCached));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> nested_compounds_and_compound_builder() {
            auto  world = MakePhysicsWorld();
            auto& sim   = *world->Simulation();

            const auto inner = ZHLN::Physics::MakeCompound(
                ZHLN::Physics::Placed {ZHLN::Physics::Box {{0.4f, 0.4f, 0.4f}}, {-1.0f, 0.0f, 0.0f}},
                ZHLN::Physics::Placed {ZHLN::Physics::Sphere {0.4f}, {1.0f, 0.0f, 0.0f}});
            const auto outer = ZHLN::Physics::MakeCompound(
                ZHLN::Physics::Placed {inner, {0.0f, 1.0f, 0.0f}},
                ZHLN::Physics::Placed {ZHLN::Physics::Capsule {.halfHeight = 0.5f, .radius = 0.25f}, {0.0f, -1.0f, 0.0f}});
            static_assert(ZHLN::Physics::DynamicShape<decltype(outer)>);
            static_assert(ZHLN::Physics::CharacterShape<decltype(outer)>);

            const auto nestedDyn = sim.CreateBody(outer, ZHLN::Physics::DynamicBody {.position = {0.0, 5.0, 0.0}});
            ZHLN::Test::ExpectTrue(nestedDyn.HasValue() && nestedDyn.value != ZHLN::Physics::DynamicBodyID::Null());

            // Runtime CompoundBuilder with nested Compound child.
            const auto                        innerNodes = inner.MakeNodes();
            std::vector<ZHLN::COM::ChainInNode> scratch;
            ZHLN::Physics::CompoundBuilder    builder;
            builder.Add(ZHLN::Physics::Box {{0.5f, 0.5f, 0.5f}}, {0.0f, 0.0f, 2.0f});
            builder.Add(ZHLN::Physics::Compound {.children = innerNodes}, {0.0f, 2.0f, 0.0f});
            const auto builtBody = sim.CreateBody(builder.View(scratch), ZHLN::Physics::StaticBody {.position = {10.0, 0.0, 0.0}});
            ZHLN::Test::ExpectTrue(builtBody.HasValue() && builtBody.value != ZHLN::Physics::StaticBodyID::Null());

            // Raycast hits the nested inner sphere at world (10 + 1, 0 + 2, 0) = (11, 2, 0).
            const auto hit = sim.Raycast(DVec3 {11.0, 6.0, 0.0}, FVec3 {0.0f, -1.0f, 0.0f}, 10.0f);
            ZHLN::Test::ExpectTrue(hit.hasHit && hit.bodyID == builtBody.value);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> shapecast_and_convex_overlap_queries() {
            auto  world = MakePhysicsWorld();
            auto& sim   = *world->Simulation();

            const auto wall = sim.CreateBody(
                ZHLN::Physics::Box {{1.0f, 2.0f, 0.5f}},
                ZHLN::Physics::StaticBody {.position = DVec3 {0.0, 2.0, 6.0}}
            );
            const auto crate = sim.CreateBoxBody(
                FVec3 {0.5f, 0.5f, 0.5f},
                ZHLN::Physics::DynamicBody {.position = DVec3 {0.5, 1.0, 1.0}}
            );
            ZHLN::Test::ExpectTrue(wall.HasValue() && crate.HasValue());

            // Sweep a 0.5m sphere from z = 0 along +Z up to 10m, ignoring the crate at z = 1.
            const auto castRes = sim.Shapecast(
                ZHLN::Physics::Sphere {.radius = 0.5f},
                DVec3 {0.0, 2.0, 0.0}, ZHLN::Quat::Identity(),
                FVec3 {0.0f, 0.0f, 1.0f}, 10.0f, crate.value
            );
            ZHLN::Test::ExpectTrue(castRes.HasValue() && castRes.value.hasHit);
            if (castRes.HasValue()) {
                ZHLN::Test::ExpectEq(castRes.value.bodyID, wall.value);
                ZHLN::Test::ExpectTrue(castRes.value.fraction > 0.45f && castRes.value.fraction < 0.55f);
                ZHLN::Test::ExpectTrue(castRes.value.contactNormal.z < -0.9f);
            }

            // ShapecastRaw error handling.
            const auto nullCast = sim.ShapecastRaw(nullptr, DVec3 {}, ZHLN::Quat::Identity(), FVec3 {0, 0, 1}, 5.0f, {});
            ZHLN::Test::ExpectTrue(!nullCast.HasValue() && nullCast.error == ZHLN::ErrorCode(ZHLN::COM::Error::NullArgument));

            const auto nonConvexCast = sim.ShapecastRaw(
                ZHLN::COM::InChain(ZHLN::Physics::Plane {}), DVec3 {}, ZHLN::Quat::Identity(), FVec3 {0, 0, 1}, 5.0f, {});
            ZHLN::Test::ExpectTrue(!nonConvexCast.HasValue() && nonConvexCast.error == ZHLN::ErrorCode(ZHLN::COM::Error::UnsupportedExtension));

            const auto badDimCast = sim.Shapecast(
                ZHLN::Physics::Sphere {.radius = -1.0f}, DVec3 {}, ZHLN::Quat::Identity(), FVec3 {0, 0, 1}, 5.0f);
            ZHLN::Test::ExpectTrue(!badDimCast.HasValue() && badDimCast.error == ZHLN::ErrorCode(ZHLN::COM::Error::InvalidArgument));

            // Rotated convex Overlap query catches both crate and wall when stretched along Z.
            const ZHLN::Quat yaw90 = ZHLN::Quat::FromAxisAngle(FVec3 {0.0f, 1.0f, 0.0f}, ZHLN::RadF {std::numbers::pi_v<float> * 0.5f});
            const auto       hits  = sim.Overlap(ZHLN::Physics::Box {{4.0f, 2.0f, 1.0f}}, DVec3 {0.0, 1.5, 3.5}, yaw90);
            ZHLN::Test::ExpectTrue(hits.has_value());
            if (hits.has_value()) {
                const auto& vec = *hits;
                ZHLN::Test::ExpectTrue(std::ranges::find(vec, ZHLN::Physics::BodyID(wall.value)) != vec.end());
                ZHLN::Test::ExpectTrue(std::ranges::find(vec, ZHLN::Physics::BodyID(crate.value)) != vec.end());
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> world_space_constraints_and_motor_targets() {
            auto  world = MakePhysicsWorld();
            auto& sim   = *world->Simulation();

            const auto anchor = sim.CreateBody(
                ZHLN::Physics::Box {{0.25f, 0.25f, 0.25f}},
                ZHLN::Physics::StaticBody {.position = DVec3 {0.0, 10.0, 0.0}}
            );
            const auto bob = sim.CreateBoxBody(
                FVec3 {0.25f, 0.25f, 0.25f},
                ZHLN::Physics::DynamicBody {.position = DVec3 {2.0, 10.0, 0.0}}
            );
            const auto sliderBody = sim.CreateBoxBody(
                FVec3 {0.25f, 0.25f, 0.25f},
                ZHLN::Physics::DynamicBody {.position = DVec3 {0.0, 6.0, 0.0}}
            );
            const auto coneBody = sim.CreateBoxBody(
                FVec3 {0.25f, 0.25f, 0.25f},
                ZHLN::Physics::DynamicBody {.position = DVec3 {0.0, 8.0, 4.0}}
            );
            const auto distBody = sim.CreateBoxBody(
                FVec3 {0.25f, 0.25f, 0.25f},
                ZHLN::Physics::DynamicBody {.position = DVec3 {-3.0, 10.0, 0.0}}
            );
            ZHLN::Test::ExpectTrue(anchor.HasValue() && bob.HasValue() && sliderBody.HasValue() && coneBody.HasValue() && distBody.HasValue());

            // Hinge at world (0, 10, 0) around +Z with motor target angle -pi/4.
            const auto hinge = sim.CreateConstraint(
                anchor.value, bob.value,
                ZHLN::Physics::HingeConstraint {
                    .pivot       = FVec3 {0.0f, 10.0f, 0.0f},
                    .axis        = FVec3 {0.0f, 0.0f, 1.0f},
                    .minAngle    = ZHLN::RadF {-std::numbers::pi_v<float> * 0.5f},
                    .maxAngle    = ZHLN::RadF {std::numbers::pi_v<float> * 0.5f},
                    .limitEnabled = true,
                });
            ZHLN::Test::ExpectTrue(hinge.HasValue() && hinge.value != ZHLN::Physics::HingeConstraintID::Null());
            sim.SetTargetAngle(hinge.value, ZHLN::RadF {-0.5f});

            // Slider along +X at world (0, 6, 0) driven to targetPosition = +1.5m.
            const auto sliderAnchor = sim.CreateBody(
                ZHLN::Physics::Box {{0.25f, 0.25f, 0.25f}},
                ZHLN::Physics::StaticBody {.position = DVec3 {0.0, 6.0, 0.0}}
            );
            ZHLN::Test::ExpectTrue(sliderAnchor.HasValue());
            const auto slider = sim.CreateConstraint(
                sliderAnchor.value, sliderBody.value,
                ZHLN::Physics::SliderConstraint {
                    .pivot          = FVec3 {0.0f, 6.0f, 0.0f},
                    .axis           = FVec3 {1.0f, 0.0f, 0.0f},
                    .minPosition    = -2.0f,
                    .maxPosition    = 2.0f,
                    .limitEnabled   = true,
                    .targetPosition = 1.5f,
                });
            ZHLN::Test::ExpectTrue(slider.HasValue() && slider.value != ZHLN::Physics::SliderConstraintID::Null());
            sim.SetTargetPosition(slider.value, 1.5f);

            // Cone constraint at (0, 10, 4) and DistanceConstraint holding distBody within 3.2m of anchor.
            const auto coneAnchor = sim.CreateBody(
                ZHLN::Physics::Box {{0.25f, 0.25f, 0.25f}},
                ZHLN::Physics::StaticBody {.position = DVec3 {0.0, 10.0, 4.0}}
            );
            ZHLN::Test::ExpectTrue(coneAnchor.HasValue());
            const auto cone = sim.CreateConstraint(
                coneAnchor.value, coneBody.value,
                ZHLN::Physics::ConeConstraint {
                    .pivot     = FVec3 {0.0f, 10.0f, 4.0f},
                    .axis      = FVec3 {0.0f, -1.0f, 0.0f},
                    .halfAngle = ZHLN::RadF {0.4f},
                });
            const auto dist = sim.CreateConstraint(
                anchor.value, distBody.value,
                ZHLN::Physics::DistanceConstraint {.minDistance = 2.8f, .maxDistance = 3.2f});
            ZHLN::Test::ExpectTrue(cone.HasValue() && dist.HasValue());

            for (int i = 0; i < 90; ++i) {
                sim.Step(1.0f / 60.0f);
            }

            // Pendulum bob stays ~2m from the hinge pivot (0, 10, 0) instead of free-falling ~22m.
            const auto bobPos = sim.BodyPosition(bob.value);
            ZHLN::Test::ExpectTrue(bobPos.HasValue());
            if (bobPos.HasValue()) {
                const double r = (bobPos.value - DVec3 {0.0, 10.0, 0.0}).Length();
                ZHLN::Test::ExpectTrue(std::abs(r - 2.0) < 0.25);
            }

            // Slider body stayed on Y ~ 6.0 and moved toward +1.5 along X!
            const auto sliderPos = sim.BodyPosition(sliderBody.value);
            ZHLN::Test::ExpectTrue(sliderPos.HasValue());
            if (sliderPos.HasValue()) {
                ZHLN::Test::ExpectTrue(std::abs(sliderPos.value.y - 6.0) < 0.2);
                ZHLN::Test::ExpectTrue(sliderPos.value.x > 0.5);
            }

            // Distance-constrained body stayed within ~3.2m of anchor (0, 10, 0).
            const auto distPos = sim.BodyPosition(distBody.value);
            ZHLN::Test::ExpectTrue(distPos.HasValue());
            if (distPos.HasValue()) {
                const double d = (distPos.value - DVec3 {0.0, 10.0, 0.0}).Length();
                ZHLN::Test::ExpectTrue(d > 2.6 && d < 3.4);
            }

            // Error paths on CreateConstraintRaw.
            const auto selfConstraint = sim.CreateConstraint(anchor.value, anchor.value, ZHLN::Physics::FixedConstraint {});
            ZHLN::Test::ExpectTrue(!selfConstraint.HasValue() && selfConstraint.error == ZHLN::ErrorCode(ZHLN::COM::Error::InvalidArgument));

            const auto nullBodyConstraint = sim.CreateConstraint(ZHLN::Physics::BodyID::Null(), bob.value, ZHLN::Physics::PointConstraint {});
            ZHLN::Test::ExpectTrue(!nullBodyConstraint.HasValue() && nullBodyConstraint.error == ZHLN::ErrorCode(ZHLN::COM::Error::InvalidArgument));

            const auto badLimits = sim.CreateConstraint(
                anchor.value, bob.value, ZHLN::Physics::DistanceConstraint {.minDistance = 5.0f, .maxDistance = 2.0f});
            ZHLN::Test::ExpectTrue(!badLimits.HasValue() && badLimits.error == ZHLN::ErrorCode(ZHLN::COM::Error::InvalidArgument));
            return {};
        }
    };
};

} // namespace
