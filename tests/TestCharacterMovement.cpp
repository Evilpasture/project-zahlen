// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Zahlen/Buffer.h>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <expected>
#include <vector>

// ============================================================================
// Test Suite Error Codes & Descriptions
// ============================================================================

enum class CharacterMovementTestError : uint32_t {
    Success = 0,
    DisplacementFailed[[= ZHLN::Reflect::Description("Character failed to traverse forward at expected velocity.")]],
    VerticalJitterDetected[[= ZHLN::Reflect::Description("Vertical position variance exceeded flat-ground stability threshold.")]],
    WallPenetrationDetected[[= ZHLN::Reflect::Description("Character breached or penetrated inside a solid obstacle collider.")]],
    JumpTrajectoryFailed[[= ZHLN::Reflect::Description("Character jump trajectory or apex altitude did not match parabolic kinematics.")]],
    LandingTransitionFailed[[= ZHLN::Reflect::Description("Landing state transition failed upon touchdown.")]],
    StepClimbingFailed[[= ZHLN::Reflect::Description("Character failed to step up and climb over an obstacle ledge.")]],
    DynamicPushFailed[[= ZHLN::Reflect::Description("Character failed to push or impart impulse to a dynamic rigid body.")]],
};

namespace {

inline float GetDensePosition(const ZHLN::PhysicsContext& pc, uint32_t denseIndex, uint32_t axis) noexcept {
    auto        posView = pc.GetPositionBuffer();
    const auto* posData = static_cast<const JPH::Real*>(posView.buf);
    return static_cast<float>(posData[denseIndex * 4 + axis]);
}

} // namespace

// ============================================================================
// Test Suite Implementation
// ============================================================================

struct CharacterMovementTestSuite {
    CharacterMovementTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, 131072);

        JPH::RegisterDefaultAllocator();
        if (JPH::Factory::sInstance == nullptr) {
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
    }

    ~CharacterMovementTestSuite() {
        ZHLN::TaskSystem::Shutdown();
        JPH::UnregisterTypes();
        if (JPH::Factory::sInstance != nullptr) {
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }

    struct Tests {
        // ====================================================================
        // 1. Flat Ground Movement & Sub-Millimeter Jitter Verification
        // ====================================================================
        std::expected<void, ZHLN::Error> character_ground_movement_and_jitter() {
            ZHLN::PhysicsConfig  cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            // 1. Ground at Y = 0.0m (Dense index 0)
            auto groundShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 50.0f, 0.5f, 50.0f);
            pc.CreateRigidBody(groundShape, JPH::RVec3(0, -0.5, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);

            // 2. Character at (0, 0, 0) (Dense index 1)
            constexpr uint32_t kCharDenseIndex = 1;
            ZHLN::Entity       charHandle      = pc.CreateCharacter(JPH::RVec3(0.0, 0.0, 0.0));
            pc.OptimizeBroadphase();

            constexpr float dt = 1.0f / 60.0f;

            // Settle on ground for 10 frames
            for (int i = 0; i < 10; ++i) {
                pc.SetCharacterVelocity(charHandle, JPH::Vec3::sZero());
                pc.Step(dt);
            }
            ZHLN::Test::ExpectTrue(pc.IsCharacterOnGround(charHandle));

            constexpr uint32_t kFrames = 60; // 1.0 second simulation
            std::vector<float> yPositions;
            yPositions.reserve(kFrames);

            constexpr float kMoveSpeed = 6.0f;
            for (uint32_t i = 0; i < kFrames; ++i) {
                pc.SetCharacterVelocity(charHandle, JPH::Vec3(0.0f, 0.0f, kMoveSpeed));
                pc.Step(dt);

                yPositions.push_back(GetDensePosition(pc, kCharDenseIndex, 1));

                // Invariant: Character must advance monotonically along +Z
                ZHLN::Test::ExpectTrue(GetDensePosition(pc, kCharDenseIndex, 2) > 0.0f);
            }

            // A. Forward Displacement Verification (~6.0m in 1.0s)
            float totalZ = GetDensePosition(pc, kCharDenseIndex, 2);
            ZHLN::Test::ExpectTrue(totalZ > 5.0f && totalZ < 7.0f);

            // B. Jitter Analysis (Verify vertical position variance is under 5mm on flat terrain)
            float minY           = *std::min_element(yPositions.begin(), yPositions.end());
            float maxY           = *std::max_element(yPositions.begin(), yPositions.end());
            float heightVariance = maxY - minY;

            ZHLN::Test::ExpectTrue(heightVariance < 0.005f);
            if (heightVariance >= 0.005f) {
                return std::unexpected(CharacterMovementTestError::VerticalJitterDetected);
            }

            return {};
        }

        // ====================================================================
        // 2. Obstacle Wall Collision, Penetration & Slide Response
        // ====================================================================
        std::expected<void, ZHLN::Error> character_wall_collision_and_penetration() {
            ZHLN::PhysicsConfig  cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            // 1. Ground at Y = 0.0m (Dense index 0)
            auto groundShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 50.0f, 0.5f, 50.0f);
            pc.CreateRigidBody(groundShape, JPH::RVec3(0, -0.5, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);

            // 2. Obstacle Wall at Z = 4.0m with half-depth 0.5m -> Front face at Z = 3.5m (Dense index 1)
            auto wallShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 4.0f, 2.0f, 0.5f);
            pc.CreateRigidBody(wallShape, JPH::RVec3(0.0, 1.5, 4.0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);

            // 3. Spawn Character at Z = 0.0m with Bumper radius XZ = 0.5m (Dense index 2)
            constexpr uint32_t             kCharDenseIndex = 2;
            ZHLN::Physics::DualShapeConfig hull {.lifterRadius = 0.4f, .bumperRadiusXZ = 0.5f, .bumperRadiusY = 0.7f};
            ZHLN::Entity                   charHandle = pc.CreateCharacter(JPH::RVec3(0.0, 0.0, 0.0), hull);
            pc.OptimizeBroadphase();

            constexpr float dt = 1.0f / 60.0f;

            // Settle
            for (int i = 0; i < 10; ++i) {
                pc.SetCharacterVelocity(charHandle, JPH::Vec3::sZero());
                pc.Step(dt);
            }

            // Run for 90 frames (1.5s): without a wall, character would reach 12m
            for (uint32_t i = 0; i < 90; ++i) {
                pc.SetCharacterVelocity(charHandle, JPH::Vec3(0.0f, 0.0f, 8.0f));
                pc.Step(dt);
            }

            float finalZ = GetDensePosition(pc, kCharDenseIndex, 2);

            // Expected stop: Wall front (3.50m) - Bumper radius (0.50m) = Z ~ 3.00m (±0.15m padding)
            ZHLN::Test::ExpectTrue(finalZ <= 3.15f);
            ZHLN::Test::ExpectTrue(finalZ >= 2.85f);

            if (finalZ > 3.15f) {
                return std::unexpected(CharacterMovementTestError::WallPenetrationDetected);
            }

            return {};
        }

        // ====================================================================
        // 3. Jump Trajectory, Airborne State & Touchdown Lifecycle
        // ====================================================================
        std::expected<void, ZHLN::Error> character_jump_gravity_and_landing() {
            ZHLN::PhysicsConfig  cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            // 1. Ground at Y = 0.0m (Dense index 0)
            auto groundShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 50.0f, 0.5f, 50.0f);
            pc.CreateRigidBody(groundShape, JPH::RVec3(0, -0.5, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);

            // 2. Character at (0, 0, 0) (Dense index 1)
            constexpr uint32_t kCharDenseIndex = 1;
            ZHLN::Entity       charHandle      = pc.CreateCharacter(JPH::RVec3(0.0, 0.0, 0.0));
            pc.OptimizeBroadphase();

            constexpr float dt = 1.0f / 60.0f;

            // Settle on ground for 10 frames
            for (int i = 0; i < 10; ++i) {
                pc.SetCharacterVelocity(charHandle, JPH::Vec3::sZero());
                pc.Step(dt);
            }
            ZHLN::Test::ExpectTrue(pc.IsCharacterOnGround(charHandle));

            float startY = GetDensePosition(pc, kCharDenseIndex, 1);

            // 1. Initiate Jump (v0 = 18.0 m/s > 15 m/s floor-snapping escape velocity)
            float vertVel = 18.0f;
            pc.SetCharacterVelocity(charHandle, JPH::Vec3(0.0f, vertVel, 0.0f));
            pc.Step(dt);

            ZHLN::Test::ExpectFalse(pc.IsCharacterOnGround(charHandle));

            // 2. Track trajectory until touchdown
            float peakY       = startY;
            bool  wasAirborne = false;
            bool  landed      = false;

            for (int i = 0; i < 90; ++i) {
                bool onGround = pc.IsCharacterOnGround(charHandle);
                if (!onGround) {
                    wasAirborne = true;
                    vertVel -= 32.0f * dt; // Gravity: -32 m/s^2
                    peakY = std::max(peakY, GetDensePosition(pc, kCharDenseIndex, 1));
                } else if (wasAirborne) {
                    landed  = true;
                    vertVel = 0.0f;
                    break; // Touchdown achieved
                }
                pc.SetCharacterVelocity(charHandle, JPH::Vec3(0.0f, vertVel, 0.0f));
                pc.Step(dt);
            }

            // Expected Apex with v0 = 18.0 m/s and g = 32 m/s^2: h = 324 / 64 ≈ 5.0m
            ZHLN::Test::ExpectTrue(wasAirborne);
            ZHLN::Test::ExpectTrue(landed);
            ZHLN::Test::ExpectTrue(peakY >= startY + 3.0f && peakY <= startY + 6.0f);
            ZHLN::Test::ExpectTrue(pc.IsCharacterOnGround(charHandle));

            if (!landed || !pc.IsCharacterOnGround(charHandle)) {
                return std::unexpected(CharacterMovementTestError::LandingTransitionFailed);
            }

            return {};
        }

        // ====================================================================
        // 4. Ledge & Stair Step-Up Climbing Traversal
        // ====================================================================
        std::expected<void, ZHLN::Error> character_step_climbing_traversal() {
            ZHLN::PhysicsConfig  cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            // 1. Lower ground at Y = 0.0m (Dense index 0)
            auto groundShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 50.0f, 0.5f, 50.0f);
            pc.CreateRigidBody(groundShape, JPH::RVec3(0, -0.5, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);

            // 2. Step Curb: Height = 0.10m (half-height = 0.05m), Depth = 1.0m (half-depth = 0.5m) (Dense index 1)
            // Positioned at (0.0, 0.05, 2.0) -> spans Z in [1.5m, 2.5m], top surface at Y = 0.10m
            auto stepShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 2.0f, 0.05f, 0.5f);
            pc.CreateRigidBody(stepShape, JPH::RVec3(0.0, 0.05, 2.0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);

            // 3. Spawn Character at Z = 0.0m on ground (Dense index 2)
            constexpr uint32_t kCharDenseIndex = 2;
            ZHLN::Entity       charHandle      = pc.CreateCharacter(JPH::RVec3(0.0, 0.0, 0.0));
            pc.OptimizeBroadphase();

            constexpr float dt = 1.0f / 60.0f;
            for (int i = 0; i < 10; ++i) {
                pc.SetCharacterVelocity(charHandle, JPH::Vec3::sZero());
                pc.Step(dt);
            }

            // Walk across the step at 4 m/s for 80 frames (1.33s, total distance 5.3m)
            for (uint32_t i = 0; i < 80; ++i) {
                pc.SetCharacterVelocity(charHandle, JPH::Vec3(0.0f, 0.0f, 4.0f));
                pc.Step(dt);
            }

            float finalZ = GetDensePosition(pc, kCharDenseIndex, 2);

            // Invariant: Character climbed over and traversed past the step (Z > 3.0m)
            ZHLN::Test::ExpectTrue(finalZ > 3.0f);
            ZHLN::Test::ExpectTrue(pc.IsCharacterOnGround(charHandle));

            if (finalZ <= 3.0f) {
                return std::unexpected(CharacterMovementTestError::StepClimbingFailed);
            }

            return {};
        }

        // ====================================================================
        // 5. Dynamic Rigid Body Push Interaction
        // ====================================================================
        std::expected<void, ZHLN::Error> character_dynamic_rigidbody_push() {
            ZHLN::PhysicsConfig  cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            // 1. Ground at Y = 0.0m
            auto groundShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 50.0f, 0.5f, 50.0f);
            pc.CreateRigidBody(groundShape, JPH::RVec3(0, -0.5, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);

            // 2. Dynamic pushable box at Z = 2.0m (Bottom touches Y = 0.0m)
            auto         boxShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 0.25f, 0.25f, 0.25f);
            ZHLN::Entity pushBox =
                pc.CreateRigidBody(boxShape, JPH::RVec3(0.0, 0.25, 2.0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, ZHLN::Layers::MOVING);

            // 3. Spawn Character at Z = 0.0m
            ZHLN::Entity charHandle = pc.CreateCharacter(JPH::RVec3(0.0, 0.0, 0.0));
            pc.OptimizeBroadphase();

            constexpr float dt = 1.0f / 60.0f;
            for (int i = 0; i < 10; ++i) {
                pc.SetCharacterVelocity(charHandle, JPH::Vec3::sZero());
                pc.Step(dt);
            }

            // Walk into the box for 60 frames
            for (uint32_t i = 0; i < 60; ++i) {
                pc.SetCharacterVelocity(charHandle, JPH::Vec3(0.0f, 0.0f, 4.0f));
                pc.Step(dt);
            }

            // Invariant: Query the dynamic box position using public Raycast API
            auto rayHit = pc.Raycast(JPH::RVec3(0.0, 0.25, 0.0), JPH::Vec3(0.0f, 0.0f, 1.0f), 10.0f);
            ZHLN::Test::ExpectTrue(rayHit.hasHit);
            ZHLN::Test::ExpectTrue(rayHit.handle == pushBox);
            ZHLN::Test::ExpectTrue(rayHit.position.GetZ() > 2.6f);

            if (!rayHit.hasHit || rayHit.position.GetZ() <= 2.6f) {
                return std::unexpected(CharacterMovementTestError::DynamicPushFailed);
            }

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<CharacterMovementTestSuite>();
}
