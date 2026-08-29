// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Zahlen/Buffer.h>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <expected>
#include <vector>

// ============================================================================
// Error Codes
// ============================================================================

enum class CharacterTestError : uint8_t {
    GroundedStateFailed[[= ZHLN::Description<"Character failed to maintain or establish a stable grounded state.">{}]] = 1,
    DisplacementMismatch[[= ZHLN::Description<"Kinematic displacement did not match expected velocity integration.">{}]],
    SubFrameJitterDetected[[= ZHLN::Description<"Sub-frame alpha interpolation produced non-monotonic or jittery motion.">{}]],
    CameraDistanceVariance[[= ZHLN::Description<"Camera-to-character relative distance variance exceeded 0.1mm threshold.">{}]],
    MotionNonMonotonic[[= ZHLN::Description<"Position hitching, backward movement, or teleportation detected.">{}]],
    WallBreachDetected[[= ZHLN::Description<"Character penetrated into a solid obstacle collider.">{}]],
    WallSlideFailed[[= ZHLN::Description<"Character failed to slide tangentially along an obstacle plane.">{}]],
    JumpTrajectoryFailed[[= ZHLN::Description<"Jump trajectory, apex altitude, or landing transition failed.">{}]],
    StepClimbFailed[[= ZHLN::Description<"Character failed to step up and traverse over an obstacle ledge.">{}]],
    SlopeClimbFailed[[= ZHLN::Description<"Character failed to climb a walkable slope or slid unexpectedly.">{}]],
    DynamicPushFailed[[= ZHLN::Description<"Character failed to impart momentum to a dynamic rigid body.">{}]],
};

// ============================================================================
// Helpers
// ============================================================================

namespace {

inline auto GetBodyPosition(const ZHLN::PhysicsContext& pc, uint32_t denseIndex) noexcept -> JPH::Vec3 {
    auto        posView = pc.GetPositionBuffer();
    const auto* posData = static_cast<const JPH::Real*>(posView.buf);
    return {static_cast<float>(posData[denseIndex * 4 + 0]), static_cast<float>(posData[denseIndex * 4 + 1]), static_cast<float>(posData[denseIndex * 4 + 2])};
}

} // namespace

// ============================================================================
// CPU Pipeline Test Harness
// ============================================================================

struct CPUPipelineHarness {
    ZHLN::ECS::Registry  reg;
    ZHLN::PhysicsContext pc;
    ZHLN::Camera         cam;
    ZHLN::Entity         player {};
    ZHLN::Entity         charPhys {};

    float accumulator  = 0.0f;
    float currentAlpha = 0.0f;

    static constexpr float kTargetDt = 1.0f / 60.0f;

    explicit CPUPipelineHarness(const ZHLN::PhysicsConfig& cfg, JPH::RVec3Arg spawnPos = JPH::RVec3(0, 0, 0), const ZHLN::Physics::DualShapeConfig& hull = {}):
        pc(cfg) {
        // Ground at Y = -0.5m with half-height 0.5m -> surface at Y = 0.0m (Dense index 0)
        auto groundShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 100.0f, 0.5f, 100.0f);
        pc.CreateRigidBody(groundShape, JPH::RVec3(0, -0.5, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);

        // Character at spawn position (Dense index 1)
        charPhys = pc.CreateCharacter(spawnPos, hull);
        player   = reg.Create(
            ZHLN::Components::TransformComponent {.position = JPH::Vec3(spawnPos)}, ZHLN::Components::MovementComponent {.speed = 6.0f},
            ZHLN::Components::PhysicsComponent {charPhys},
            ZHLN::Components::PhysicsStateComponent {.currPosition = JPH::Vec3(spawnPos), .prevPosition = JPH::Vec3(spawnPos)}
        );

        pc.OptimizeBroadphase();

        cam.position = JPH::Vec3(spawnPos) + JPH::Vec3(0.0f, 1.5f, -5.0f);
        cam.yaw      = -90.0f;
        cam.pitch    = 0.0f;
    }

    void Settle(int frames = 15) {
        for (int i = 0; i < frames; ++i) {
            Tick(kTargetDt, 0.0f, 0.0f);
        }
    }

    void Tick(float renderDt, float inputX, float inputZ, float verticalVel = 0.0f) {
        auto* move  = reg.Get<ZHLN::Components::MovementComponent>(player);
        auto* state = reg.Get<ZHLN::Components::PhysicsStateComponent>(player);
        auto* trans = reg.Get<ZHLN::Components::TransformComponent>(player);

        move->inputX = inputX;
        move->inputZ = inputZ;

        accumulator += std::min(renderDt, 0.1f);
        while (accumulator >= kTargetDt) {
            JPH::Vec3 vel(move->inputX * move->speed, verticalVel, move->inputZ * move->speed);
            pc.SetCharacterVelocity(charPhys, vel);

            pc.Step(kTargetDt);

            state->prevPosition = state->currPosition;
            state->currPosition = GetBodyPosition(pc, 1);

            accumulator -= kTargetDt;
        }

        currentAlpha = accumulator / kTargetDt;
        float alpha  = std::clamp(currentAlpha, 0.0f, 1.0f);

        trans->position = state->prevPosition + alpha * (state->currPosition - state->prevPosition);

        JPH::Vec3 targetCenter = trans->position + JPH::Vec3(0.0f, 1.5f, 0.0f);
        cam.position           = targetCenter + JPH::Vec3(0.0f, 0.0f, -5.0f);
    }
};

// ============================================================================
// Test Suite
// ============================================================================

struct CharacterMovementTestSuite {
    CharacterMovementTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);

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
        // 1. Settle & Resting Stability
        auto test_01_flat_ground_settling_and_stability() -> std::expected<void, ZHLN::Error> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            harness.Settle(20);

            ZHLN::Test::ExpectTrue(harness.pc.IsCharacterOnGround(harness.charPhys));

            JPH::Vec3 pos = GetBodyPosition(harness.pc, 1);
            ZHLN::Test::ExpectTrue(std::abs(pos.GetY()) < 0.005f);

            if (!harness.pc.IsCharacterOnGround(harness.charPhys) || std::abs(pos.GetY()) >= 0.005f) {
                return std::unexpected(CharacterTestError::GroundedStateFailed);
            }
            return {};
        }

        // 2. Kinematic Forward & Lateral Displacement
        auto test_02_constant_velocity_displacement() -> std::expected<void, ZHLN::Error> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);
            harness.Settle(10);

            for (int i = 0; i < 60; ++i) {
                harness.Tick(1.0f / 60.0f, 0.0f, 1.0f);
            }

            JPH::Vec3 pos = GetBodyPosition(harness.pc, 1);
            ZHLN::Test::ExpectTrue(pos.GetZ() >= 5.85f && pos.GetZ() <= 6.15f);

            if (pos.GetZ() < 5.85f || pos.GetZ() > 6.15f) {
                return std::unexpected(CharacterTestError::DisplacementMismatch);
            }
            return {};
        }

        // 3. 144 Hz Sub-Frame Interpolation Smoothness
        auto test_03_144hz_subframe_interpolation_smoothness() -> std::expected<void, ZHLN::Error> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);
            harness.Settle(10);

            harness.Tick(1.0f / 60.0f, 0.0f, 1.0f);

            const auto* trans = harness.reg.Get<ZHLN::Components::TransformComponent>(harness.player);
            float       lastZ = trans->position.GetZ();

            constexpr float dt144 = 1.0f / 144.0f;
            for (int i = 0; i < 144; ++i) {
                harness.Tick(dt144, 0.0f, 1.0f);

                float currZ = trans->position.GetZ();
                ZHLN::Test::ExpectTrue(currZ > lastZ);

                if (currZ <= lastZ) {
                    return std::unexpected(CharacterTestError::SubFrameJitterDetected);
                }
                lastZ = currZ;
            }
            return {};
        }

        // 4. Camera-to-Character Relative Distance Invariance (< 0.1mm)
        auto test_04_camera_relative_distance_invariance() -> std::expected<void, ZHLN::Error> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);
            harness.Settle(10);

            std::vector<float> distances;
            distances.reserve(144);

            constexpr float dt144 = 1.0f / 144.0f;
            for (int i = 0; i < 144; ++i) {
                harness.Tick(dt144, 0.0f, 1.0f);

                const auto* trans        = harness.reg.Get<ZHLN::Components::TransformComponent>(harness.player);
                JPH::Vec3   targetCenter = trans->position + JPH::Vec3(0.0f, 1.5f, 0.0f);

                float dist = (harness.cam.position - targetCenter).Length();
                distances.push_back(dist);
            }

            float minDist  = *std::ranges::min_element(distances);
            float maxDist  = *std::ranges::max_element(distances);
            float variance = maxDist - minDist;

            ZHLN::Test::ExpectTrue(variance < 0.0001f);
            ZHLN::Test::ExpectTrue(std::abs(minDist - 5.0f) < 0.0001f);

            if (variance >= 0.0001f) {
                return std::unexpected(CharacterTestError::CameraDistanceVariance);
            }
            return {};
        }

        // 5. Variable Delta-Time Motion Monotonicity
        auto test_05_variable_dt_motion_monotonicity() -> std::expected<void, ZHLN::Error> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);
            harness.Settle(10);

            harness.Tick(1.0f / 60.0f, 0.0f, 1.0f);

            const auto* trans = harness.reg.Get<ZHLN::Components::TransformComponent>(harness.player);
            float       lastZ = trans->position.GetZ();

            const std::array<float, 5> variableDts = {0.008f, 0.033f, 0.012f, 0.048f, 0.016f};

            for (int loop = 0; loop < 25; ++loop) {
                for (float dt: variableDts) {
                    harness.Tick(dt, 0.0f, 1.0f);

                    float currZ = trans->position.GetZ();
                    ZHLN::Test::ExpectTrue(currZ > lastZ);

                    if (currZ <= lastZ) {
                        return std::unexpected(CharacterTestError::MotionNonMonotonic);
                    }
                    lastZ = currZ;
                }
            }
            return {};
        }

        // 6. Wall Collision & Penetration Resistance
        auto test_06_wall_collision_and_penetration_prevention() -> std::expected<void, ZHLN::Error> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            auto wallShape = harness.pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 10.0f, 2.0f, 0.5f);
            harness.pc.CreateRigidBody(wallShape, JPH::RVec3(0, 1.5, 4.0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);
            harness.pc.OptimizeBroadphase();

            harness.Settle(10);

            for (int i = 0; i < 90; ++i) {
                harness.Tick(1.0f / 60.0f, 0.0f, 1.66f);
            }

            JPH::Vec3 pos = GetBodyPosition(harness.pc, 1);

            ZHLN::Test::ExpectTrue(pos.GetZ() <= 3.15f);
            ZHLN::Test::ExpectTrue(pos.GetZ() >= 2.85f);

            if (pos.GetZ() > 3.15f) {
                return std::unexpected(CharacterTestError::WallBreachDetected);
            }
            return {};
        }

        // 7. Angled Wall Sliding & Tangential Deflection
        auto test_07_angled_wall_sliding_deflection() -> std::expected<void, ZHLN::Error> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            auto wallShape = harness.pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 50.0f, 2.0f, 0.5f);
            harness.pc.CreateRigidBody(wallShape, JPH::RVec3(0, 1.5, 3.0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);
            harness.pc.OptimizeBroadphase();

            harness.Settle(10);

            for (int i = 0; i < 60; ++i) {
                harness.Tick(1.0f / 60.0f, 1.0f, 1.0f);
            }

            JPH::Vec3 pos = GetBodyPosition(harness.pc, 1);

            ZHLN::Test::ExpectTrue(pos.GetZ() <= 2.20f);
            ZHLN::Test::ExpectTrue(pos.GetX() >= 4.0f);

            if (pos.GetZ() > 2.20f || pos.GetX() < 4.0f) {
                return std::unexpected(CharacterTestError::WallSlideFailed);
            }
            return {};
        }

        // 8. Jump Trajectory & Parabolic Apex
        auto test_08_jump_kinematics_and_landing() -> std::expected<void, ZHLN::Error> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);
            harness.Settle(10);

            float vertVel  = 12.0f;
            float peakY    = 0.0f;
            bool  wasInAir = false;
            bool  landed   = false;

            constexpr float dt = 1.0f / 60.0f;

            for (int i = 0; i < 90; ++i) {
                bool onGround = harness.pc.IsCharacterOnGround(harness.charPhys);

                if (!onGround) {
                    wasInAir = true;
                    vertVel -= 32.0f * dt;
                    peakY = std::max(peakY, GetBodyPosition(harness.pc, 1).GetY());
                } else if (wasInAir) {
                    landed  = true;
                    vertVel = 0.0f;
                    break;
                }

                harness.Tick(dt, 0.0f, 0.0f, vertVel);
            }

            ZHLN::Test::ExpectTrue(wasInAir);
            ZHLN::Test::ExpectTrue(landed);
            ZHLN::Test::ExpectTrue(peakY >= 1.8f && peakY <= 2.7f);
            ZHLN::Test::ExpectTrue(harness.pc.IsCharacterOnGround(harness.charPhys));

            if (!landed || peakY < 1.8f) {
                return std::unexpected(CharacterTestError::JumpTrajectoryFailed);
            }
            return {};
        }

        // 9. Ledge & Curb Step-Up Auto-Climbing
        auto test_09_ledge_step_climbing() -> std::expected<void, ZHLN::Error> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            auto curbShape = harness.pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 5.0f, 0.075f, 0.5f);
            harness.pc.CreateRigidBody(curbShape, JPH::RVec3(0, 0.075, 2.0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);
            harness.pc.OptimizeBroadphase();

            harness.Settle(10);

            for (int i = 0; i < 75; ++i) {
                harness.Tick(1.0f / 60.0f, 0.0f, 0.66f);
            }

            JPH::Vec3 pos = GetBodyPosition(harness.pc, 1);

            ZHLN::Test::ExpectTrue(pos.GetZ() > 3.0f);
            ZHLN::Test::ExpectTrue(harness.pc.IsCharacterOnGround(harness.charPhys));

            if (pos.GetZ() <= 3.0f || !harness.pc.IsCharacterOnGround(harness.charPhys)) {
                return std::unexpected(CharacterTestError::StepClimbFailed);
            }
            return {};
        }

        // 10. Walkable Slope Traversal
        auto test_10_walkable_slope_climbing() -> std::expected<void, ZHLN::Error> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            JPH::Quat rampRot   = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::DegreesToRadians(-25.0f));
            auto      rampShape = harness.pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 5.0f, 0.2f, 5.0f);
            harness.pc.CreateRigidBody(rampShape, JPH::RVec3(0, 1.0, 4.0), rampRot, JPH::EMotionType::Static, ZHLN::Layers::NON_MOVING);
            harness.pc.OptimizeBroadphase();

            harness.Settle(10);

            for (int i = 0; i < 60; ++i) {
                harness.Tick(1.0f / 60.0f, 0.0f, 1.0f);
            }

            JPH::Vec3 pos = GetBodyPosition(harness.pc, 1);

            ZHLN::Test::ExpectTrue(pos.GetY() > 0.8f);
            ZHLN::Test::ExpectTrue(pos.GetZ() > 3.0f);

            if (pos.GetY() <= 0.8f) {
                return std::unexpected(CharacterTestError::SlopeClimbFailed);
            }
            return {};
        }

        // 11. Dynamic Rigid Body Push Interaction
        auto test_11_dynamic_rigid_body_push_impulse() -> std::expected<void, ZHLN::Error> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            auto         boxShape = harness.pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 0.25f, 0.25f, 0.25f);
            ZHLN::Entity pushBox =
                harness.pc.CreateRigidBody(boxShape, JPH::RVec3(0.0, 0.25, 2.0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, ZHLN::Layers::MOVING);
            harness.pc.OptimizeBroadphase();

            harness.Settle(10);

            for (int i = 0; i < 60; ++i) {
                harness.Tick(1.0f / 60.0f, 0.0f, 0.66f);
            }

            auto rayHit = harness.pc.Raycast(JPH::RVec3(0.0, 0.25, 0.0), JPH::Vec3(0.0f, 0.0f, 1.0f), 10.0f);
            ZHLN::Test::ExpectTrue(rayHit.hasHit);
            ZHLN::Test::ExpectTrue(rayHit.handle == pushBox);
            ZHLN::Test::ExpectTrue(rayHit.position.GetZ() > 2.6f);

            if (!rayHit.hasHit || rayHit.position.GetZ() <= 2.6f) {
                return std::unexpected(CharacterTestError::DynamicPushFailed);
            }
            return {};
        }
    };
};

auto main() -> int {
    return ZHLN::Test::Runner::Run<CharacterMovementTestSuite>();
}
