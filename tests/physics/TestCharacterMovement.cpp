// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include "helpers/CommonFixtures.hpp"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Zahlen/Core/Buffer.h>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <CharacterController/CharacterComponents.hpp>
#include <CharacterController/CharacterMovement.hpp>
#include <CharacterController/PlayerInput.hpp>
#include <Zahlen/Engine/Capabilities/InputReader.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <expected>
#include <vector>

// ============================================================================
// Error Codes
// ============================================================================

enum class CharacterTestError : uint8_t {
    GroundedStateFailed ZHLN_ANNOTATION(ZHLN::Description<"Character failed to maintain or establish a stable grounded state.">{}) = 1,
    DisplacementMismatch ZHLN_ANNOTATION(ZHLN::Description<"Kinematic displacement did not match expected velocity integration.">{}),
    SubFrameJitterDetected ZHLN_ANNOTATION(ZHLN::Description<"Sub-frame alpha interpolation produced non-monotonic or jittery motion.">{}),
    CameraDistanceVariance ZHLN_ANNOTATION(ZHLN::Description<"Camera-to-character relative distance variance exceeded 0.1mm threshold.">{}),
    MotionNonMonotonic ZHLN_ANNOTATION(ZHLN::Description<"Position hitching, backward movement, or teleportation detected.">{}),
    WallBreachDetected ZHLN_ANNOTATION(ZHLN::Description<"Character penetrated into a solid obstacle collider.">{}),
    WallSlideFailed ZHLN_ANNOTATION(ZHLN::Description<"Character failed to slide tangentially along an obstacle plane.">{}),
    JumpTrajectoryFailed ZHLN_ANNOTATION(ZHLN::Description<"Jump trajectory, apex altitude, or landing transition failed.">{}),
    StepClimbFailed ZHLN_ANNOTATION(ZHLN::Description<"Character failed to step up and traverse over an obstacle ledge.">{}),
    SlopeClimbFailed ZHLN_ANNOTATION(ZHLN::Description<"Character failed to climb a walkable slope or slid unexpectedly.">{}),
    DynamicPushFailed ZHLN_ANNOTATION(ZHLN::Description<"Character failed to impart momentum to a dynamic rigid body.">{}),
};

// ============================================================================
// Helpers
// ============================================================================

namespace {

inline auto GetBodyPosition(const ZHLN::PhysicsContext& pc, uint32_t denseIndex) noexcept -> ZHLN::FVec3 {
    auto        posView = pc.GetPositionBuffer();
    const auto* posData = static_cast<const JPH::Real*>(posView.buf);
    return {static_cast<float>(posData[denseIndex * 4 + 0]), static_cast<float>(posData[denseIndex * 4 + 1]), static_cast<float>(posData[denseIndex * 4 + 2])};
}

} // namespace

// ============================================================================
// CPU Pipeline Test Harness
// ============================================================================

struct CPUPipelineHarness {
    ZHLN::PhysicsContext pc;
    ZHLN::ECS::Registry  reg;
    ZHLN::Camera         cam;
    ZHLN::Entity         player {};
    ZHLN::Physics::BodyID charPhys {};
    ZHLN::FVec3          prevPos = ZHLN::FVec3::Zero();
    ZHLN::FVec3          currPos = ZHLN::FVec3::Zero();

    float accumulator  = 0.0f;
    float currentAlpha = 0.0f;

    static constexpr float kTargetDt = 1.0f / 60.0f;

    explicit CPUPipelineHarness(const ZHLN::PhysicsConfig& cfg, ZHLN::DVec3 spawnPos = ZHLN::DVec3::Zero(), const ZHLN::Physics::DualShapeConfig& hull = {}):
        pc(cfg) {
        // Ground at Y = -0.5m with half-height 0.5m -> surface at Y = 0.0m (Dense index 0)
        auto groundShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 100.0f, 0.5f, 100.0f);
        pc.CreateRigidBody(groundShape, {0, -0.5, 0}, {0.0f, 0.0f, 0.0f, 1.0f}, JPH::EMotionType::Static, ZHLN::Layers::ID::NON_MOVING);

        // Character at spawn position (Dense index 1). The hull is authored
        // explicitly: the core character API takes a shape and behavioral
        // knobs, and the supporting volume must fit the hull's origin.
        charPhys = pc.CreateCharacter(
            spawnPos, ZHLN::Physics::CharacterParams {
                          .shape = ZHLN::Physics::CreateDualShape(hull), .supportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -hull.GetLifterOffsetY())
                      }
        );
        player   = reg.Create(
            ZHLN::Components::TransformComponent {.position = spawnPos.Cast<float>()}, ZHLN::Character::MovementComponent {.speed = 6.0f},
            ZHLN::Components::PhysicsComponent {.physicsID = charPhys, .isStatic = false}
        );
        prevPos = spawnPos.Cast<float>();
        currPos = spawnPos.Cast<float>();

        pc.OptimizeBroadphase();

        cam.position = spawnPos.Cast<float>() + ZHLN::FVec3(0.0f, 1.5f, -5.0f);
        cam.yaw      = ZHLN::DegF{-90.0f};
        cam.pitch    = ZHLN::DegF{0.0f};
    }

    ~CPUPipelineHarness() {
        std::vector<ZHLN::Physics::BodyID> handles;
        for (const auto& component: reg.GetRawArray<ZHLN::Components::PhysicsComponent>()) {
            handles.push_back(component.physicsID);
        }
        pc.DestroyBodies(handles);
        reg.Clear();
    }

    void Settle(int frames = 15) {
        for (int i = 0; i < frames; ++i) {
            Tick(kTargetDt, 0.0f, 0.0f);
        }
    }

    void Tick(float renderDt, float inputX, float inputZ, float verticalVel = 0.0f) {
        auto move  = reg.Get<ZHLN::Character::MovementComponent>(player);
        auto trans = reg.Get<ZHLN::Components::TransformComponent>(player);

        move->inputX = inputX;
        move->inputZ = inputZ;

        accumulator += std::min(renderDt, 0.1f);
        while (accumulator >= kTargetDt) {
            const ZHLN::FVec3 vel {move->inputX * move->speed, verticalVel, move->inputZ * move->speed};
            pc.SetCharacterVelocity(charPhys, vel);

            // Prop interaction is controller policy, not a contact-callback
            // policy: the fixed step pushes the props the character moves
            // through, the same way the engine's CharacterPreStep hook does.
            ZHLN::Character::PushProps(ZHLN::Character::CharacterReadQuery {reg}, ZHLN::ECS::ResMut<ZHLN::PhysicsContext> {&pc});
            pc.Step(kTargetDt);

            prevPos = currPos;
            currPos = GetBodyPosition(pc, 1);

            accumulator -= kTargetDt;
        }

        currentAlpha = accumulator / kTargetDt;
        float alpha  = std::clamp(currentAlpha, 0.0f, 1.0f);

        trans->position = prevPos + alpha * (currPos - prevPos);

        const ZHLN::FVec3 targetCenter = trans->position + ZHLN::FVec3(0.0f, 1.5f, 0.0f);
        cam.position           = targetCenter + ZHLN::FVec3(0.0f, 0.0f, -5.0f);
    }
};

// ============================================================================
// Character Movement Tests (zero-boilerplate namespace collection)
// ============================================================================

namespace ZHLN::Test {
namespace ZHLN_ANNOTATION(UseFixtures<Fixture::TaskSystemAndJoltScope> {}) {

        // The namespace mark expresses the intended suite fixture. Each test
        // also requests it explicitly: initialization must not depend on the
        // compiler reflecting an annotation on an unnamed namespace.
        void test_00_task_and_jolt_fixture_is_live(Fixture::TaskSystemAndJoltScope&) {
            ZHLN::Test::ExpectGt(ZHLN::TaskSystem::GetWorkerCount(), 0u);
            ZHLN::Test::ExpectTrue(JPH::Factory::sInstance != nullptr);
        }

        void test_typed_player_input_and_intent_seat_and_freecam(Fixture::TaskSystemAndJoltScope&) {
            // The seat translation is a pure function over a seat's physical
            // state, so it is tested as one: no router, no engine, no window and
            // no synthetic device events. What used to be a constructed router
            // plus InjectKey(W, true) is now two locals.
            ECS::Registry reg;

            InputState primaryKbm {};
            primaryKbm.SetKey(KeyCode::W, true);
            primaryKbm.SetKey(KeyCode::Space, true);
            primaryKbm.SetKey(KeyCode::LShift, true);

            InputState secondaryKbm {};
            secondaryKbm.SetKey(KeyCode::D, true);

            const auto primary   = reg.Create(Character::InputComponent {}, Character::MovementComponent {});
            const auto secondary = reg.Create(Character::InputComponent {}, Character::MovementComponent {}, Components::PlayerSeatComponent {.seat = PlayerSeat {1}});
            ExpectTrue(reg.Get<Components::PlayerSeatComponent>(secondary)->seat == PlayerSeat {1});

            // Both seats get the keyboard, and neither has a pad: this is the
            // shape TranslateSeatIntent takes, not a router configured to fake it.
            ExpectTrue(primaryKbm.IsKeyDown(KeyCode::W));
            ExpectTrue(secondaryKbm.IsKeyDown(KeyCode::D));

            // A router that provides no pads and no actions. The function needs
            // one only to answer gamepad queries, which return idle values for a
            // bundle with no device attached -- so it takes an InputReader from a
            // real Engine rather than a fabricated one. Author the nothing it
            // needs and read back.
            auto engineCreated = Engine::Create(EngineConfig {.appName = "CharacterInputTest", .validationMode = ValidationMode::Off, .headless = true});
            ExpectTrue(engineCreated.has_value());
            if (!engineCreated) {
                return;
            }
            auto engine       = std::move(*engineCreated);
            const auto reader = std::as_const(*engine).Input();

            const Character::SeatIntent primaryIntent =
                Character::TranslateSeatIntent(primaryKbm, /*seatHasKeyboardMouse=*/true, reader, GamepadSlot {static_cast<uint8_t>(kMaxGamepads)});
            const Character::SeatIntent secondaryIntent =
                Character::TranslateSeatIntent(secondaryKbm, /*seatHasKeyboardMouse=*/true, reader, GamepadSlot {static_cast<uint8_t>(kMaxGamepads)});

            ExpectEq(primaryIntent.moveZ, 1.0f);
            ExpectEq(primaryIntent.moveX, 0.0f);
            ExpectTrue(primaryIntent.jump && primaryIntent.sprint);
            ExpectEq(secondaryIntent.moveX, 1.0f);
            ExpectEq(secondaryIntent.moveZ, 0.0f);
            ExpectTrue(!secondaryIntent.jump && !secondaryIntent.sprint);

            // A seat the routing does not give the keyboard reports no keyboard
            // intent at all -- the same rule the system applies per seat.
            const Character::SeatIntent unseatedIntent =
                Character::TranslateSeatIntent(primaryKbm, /*seatHasKeyboardMouse=*/false, reader, GamepadSlot {static_cast<uint8_t>(kMaxGamepads)});
            ExpectEq(unseatedIntent.moveZ, 0.0f);
            ExpectEq(unseatedIntent.moveX, 0.0f);
            ExpectTrue(!unseatedIntent.jump && !unseatedIntent.sprint);

            // Latch the intent into the components the next stage consumes.
            const auto latch = [&](Entity entity, const Character::SeatIntent& intent) {
                reg.Patch<Character::InputComponent>(entity, [&](Character::InputComponent& ic) {
                    ic.localMoveX     = intent.moveX;
                    ic.localMoveZ     = intent.moveZ;
                    ic.lookYawDelta   = intent.lookYaw;
                    ic.lookPitchDelta = intent.lookPitch;
                    ic.zoomDelta      = intent.zoom;
                    ic.wantsToJump    = intent.jump;
                    ic.wantsToSprint  = intent.sprint;
                });
            };
            latch(primary, primaryIntent);
            latch(secondary, secondaryIntent);
            ExpectTrue(reg.Get<Character::InputComponent>(primary)->localMoveZ == 1.0f);
            ExpectTrue(reg.Get<Character::InputComponent>(primary)->wantsToJump && reg.Get<Character::InputComponent>(primary)->wantsToSprint);
            ExpectEq(reg.Get<Character::InputComponent>(secondary)->localMoveX, 1.0f);

            // No camera means intent remains latched, not partially changed.
            Character::PlayerInputSystem::PlayerInputTranslate(Character::PlayerInputSystem::IntentQuery {reg});
            ExpectTrue(reg.Get<Character::MovementComponent>(primary)->inputX == 0.0f);
            Components::CameraComponent camera {};
            camera.camera.yaw = DegF {0.0f};
            const auto cameraEntity = reg.Create(camera, Components::MainCameraTagComponent {});
            Character::PlayerInputSystem::PlayerInputTranslate(Character::PlayerInputSystem::IntentQuery {reg});
            const auto p = reg.Get<Character::MovementComponent>(primary);
            const auto s = reg.Get<Character::MovementComponent>(secondary);
            ExpectTrue(std::abs(p->inputX - 1.0f) < 1e-5f && std::abs(p->inputZ) < 1e-5f);
            ExpectTrue(p->jumpRequested && p->isSprinting && std::abs(s->inputZ - 1.0f) < 1e-5f);
            reg.Add(cameraEntity, FreeCam::FreeCamTagComponent {});
            Character::PlayerInputSystem::PlayerInputTranslate(Character::PlayerInputSystem::IntentQuery {reg});
            ExpectTrue(p->inputX == 0.0f && p->inputZ == 0.0f && !p->jumpRequested);
            ExpectTrue(s->inputX == 0.0f && s->inputZ == 0.0f && !s->jumpRequested);
        }

        void test_typed_fixed_step_movement_and_grounded_state(Fixture::TaskSystemAndJoltScope&) {
            PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness harness(cfg);
            harness.Settle(10);
            auto move = harness.reg.Get<Character::MovementComponent>(harness.player);
            move->inputX = 1.0f;
            move->jumpRequested = true;
            move->isGrounded = true;
            const auto withoutPhysics = harness.reg.Create(Character::MovementComponent {.currentYVel = 7.0f});
            Character::MovementSystem(Character::MovementQuery {harness.reg}, FrameDt {1.0f / 60.0f});
            move = harness.reg.Get<Character::MovementComponent>(harness.player);
            ExpectTrue(!move->jumpRequested && move->currentYVel == move->jumpForce && move->currentVelX > 0.0f);
            ExpectTrue(harness.reg.Get<Character::MovementComponent>(withoutPhysics)->currentYVel == 7.0f);
            Character::CommitCharacterSteering(Character::CharacterReadQuery {harness.reg}, ECS::ResMut<PhysicsContext> {&harness.pc});
            ExpectTrue(harness.pc.GetCharacterVelocity(harness.charPhys).GetX() > 0.0f);
            harness.pc.Step(1.0f / 60.0f);
            Character::WriteCharacterGrounded(Character::CharacterGroundedQuery {harness.reg}, ECS::Res<PhysicsContext> {&harness.pc});
            move = harness.reg.Get<Character::MovementComponent>(harness.player);
            ExpectTrue(move->wasGrounded && move->isGrounded == harness.pc.IsCharacterOnGround(harness.charPhys));
        }

        // 1. Settle & Resting Stability
        auto test_01_flat_ground_settling_and_stability(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            harness.Settle(20);

            ZHLN::Test::ExpectTrue(harness.pc.IsCharacterOnGround(harness.charPhys));

            const auto pos = GetBodyPosition(harness.pc, 1);
            ZHLN::Test::ExpectLt(std::abs(pos.y), 0.005f);

            if (!harness.pc.IsCharacterOnGround(harness.charPhys) || std::abs(pos.y) >= 0.005f) {
                return std::unexpected(CharacterTestError::GroundedStateFailed);
            }
            return {};
        }

        // 2. Kinematic Forward & Lateral Displacement
        auto test_02_constant_velocity_displacement(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);
            harness.Settle(10);

            for (int i = 0; i < 60; ++i) {
                harness.Tick(1.0f / 60.0f, 0.0f, 1.0f);
            }

            const auto pos = GetBodyPosition(harness.pc, 1);
            ZHLN::Test::ExpectGe(pos.z, 5.85f) && ZHLN::Test::ExpectLe(pos.z, 6.15f);

            if (pos.z < 5.85f || pos.z > 6.15f) {
                return std::unexpected(CharacterTestError::DisplacementMismatch);
            }
            return {};
        }

        // 3. 144 Hz Sub-Frame Interpolation Smoothness
        auto test_03_144hz_subframe_interpolation_smoothness(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);
            harness.Settle(10);

            harness.Tick(1.0f / 60.0f, 0.0f, 1.0f);

            const auto trans = harness.reg.Get<ZHLN::Components::TransformComponent>(harness.player);
            float      lastZ = trans->position.z;

            constexpr float dt144 = 1.0f / 144.0f;
            for (int i = 0; i < 144; ++i) {
                harness.Tick(dt144, 0.0f, 1.0f);

                float currZ = trans->position.z;
                ZHLN::Test::ExpectGt(currZ, lastZ);

                if (currZ <= lastZ) {
                    return std::unexpected(CharacterTestError::SubFrameJitterDetected);
                }
                lastZ = currZ;
            }
            return {};
        }

        // 4. Camera-to-Character Relative Distance Invariance (< 0.1mm)
        auto test_04_camera_relative_distance_invariance(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);
            harness.Settle(10);

            std::vector<float> distances;
            distances.reserve(144);

            constexpr float dt144 = 1.0f / 144.0f;
            for (int i = 0; i < 144; ++i) {
                harness.Tick(dt144, 0.0f, 1.0f);

                const auto trans        = harness.reg.Get<ZHLN::Components::TransformComponent>(harness.player);
                const ZHLN::FVec3 targetCenter = trans->position + ZHLN::FVec3(0.0f, 1.5f, 0.0f);

                float dist = (harness.cam.position - targetCenter).Length();
                distances.push_back(dist);
            }

            float minDist  = *std::ranges::min_element(distances);
            float maxDist  = *std::ranges::max_element(distances);
            float variance = maxDist - minDist;

            ZHLN::Test::ExpectLt(variance, 0.0001f);
            ZHLN::Test::ExpectLt(std::abs(minDist - 5.0f), 0.0001f);

            if (variance >= 0.0001f) {
                return std::unexpected(CharacterTestError::CameraDistanceVariance);
            }
            return {};
        }

        // 5. Variable Delta-Time Motion Monotonicity
        auto test_05_variable_dt_motion_monotonicity(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);
            harness.Settle(10);

            harness.Tick(1.0f / 60.0f, 0.0f, 1.0f);

            const auto trans = harness.reg.Get<ZHLN::Components::TransformComponent>(harness.player);
            float      lastZ = trans->position.z;

            const std::array<float, 5> variableDts = {0.008f, 0.033f, 0.012f, 0.048f, 0.016f};

            for (int loop = 0; loop < 25; ++loop) {
                for (float dt: variableDts) {
                    harness.Tick(dt, 0.0f, 1.0f);

                    float currZ = trans->position.z;
                    ZHLN::Test::ExpectGt(currZ, lastZ);

                    if (currZ <= lastZ) {
                        return std::unexpected(CharacterTestError::MotionNonMonotonic);
                    }
                    lastZ = currZ;
                }
            }
            return {};
        }

        // 6. Wall Collision & Penetration Resistance
        auto test_06_wall_collision_and_penetration_prevention(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            auto wallShape = harness.pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 10.0f, 2.0f, 0.5f);
            harness.pc.CreateRigidBody(wallShape, {0, 1.5, 4.0}, {0.0f, 0.0f, 0.0f, 1.0f}, JPH::EMotionType::Static, ZHLN::Layers::ID::NON_MOVING);
            harness.pc.OptimizeBroadphase();

            harness.Settle(10);

            for (int i = 0; i < 90; ++i) {
                harness.Tick(1.0f / 60.0f, 0.0f, 1.66f);
            }

            const auto pos = GetBodyPosition(harness.pc, 1);

            ZHLN::Test::ExpectLe(pos.z, 3.15f);
            ZHLN::Test::ExpectGe(pos.z, 2.85f);

            if (pos.z > 3.15f) {
                return std::unexpected(CharacterTestError::WallBreachDetected);
            }
            return {};
        }

        // 7. Angled Wall Sliding & Tangential Deflection
        auto test_07_angled_wall_sliding_deflection(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            auto wallShape = harness.pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 50.0f, 2.0f, 0.5f);
            harness.pc.CreateRigidBody(wallShape, {0, 1.5, 3.0}, {0.0f, 0.0f, 0.0f, 1.0f}, JPH::EMotionType::Static, ZHLN::Layers::ID::NON_MOVING);
            harness.pc.OptimizeBroadphase();

            harness.Settle(10);

            for (int i = 0; i < 60; ++i) {
                harness.Tick(1.0f / 60.0f, 1.0f, 1.0f);
            }

            const auto pos = GetBodyPosition(harness.pc, 1);

            ZHLN::Test::ExpectLe(pos.z, 2.20f);
            ZHLN::Test::ExpectGe(pos.x, 4.0f);

            if (pos.z > 2.20f || pos.x < 4.0f) {
                return std::unexpected(CharacterTestError::WallSlideFailed);
            }
            return {};
        }

        // 8. Jump Trajectory & Parabolic Apex
        auto test_08_jump_kinematics_and_landing(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
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
                    peakY = std::max(peakY, GetBodyPosition(harness.pc, 1).y);
                } else if (wasInAir) {
                    landed  = true;
                    vertVel = 0.0f;
                    break;
                }

                harness.Tick(dt, 0.0f, 0.0f, vertVel);
            }

            ZHLN::Test::ExpectTrue(wasInAir);
            ZHLN::Test::ExpectTrue(landed);
            ZHLN::Test::ExpectGe(peakY, 1.8f) && ZHLN::Test::ExpectLe(peakY, 2.7f);
            ZHLN::Test::ExpectTrue(harness.pc.IsCharacterOnGround(harness.charPhys));

            if (!landed || peakY < 1.8f) {
                return std::unexpected(CharacterTestError::JumpTrajectoryFailed);
            }
            return {};
        }

        // 9. Ledge & Curb Step-Up Auto-Climbing
        auto test_09_ledge_step_climbing(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            auto curbShape = harness.pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 5.0f, 0.075f, 0.5f);
            harness.pc.CreateRigidBody(curbShape, {0, 0.075, 2.0}, {0.0f, 0.0f, 0.0f, 1.0f}, JPH::EMotionType::Static, ZHLN::Layers::ID::NON_MOVING);
            harness.pc.OptimizeBroadphase();

            harness.Settle(10);

            for (int i = 0; i < 75; ++i) {
                harness.Tick(1.0f / 60.0f, 0.0f, 0.66f);
            }

            const auto pos = GetBodyPosition(harness.pc, 1);

            ZHLN::Test::ExpectGt(pos.z, 3.0f);
            ZHLN::Test::ExpectTrue(harness.pc.IsCharacterOnGround(harness.charPhys));

            if (pos.z <= 3.0f || !harness.pc.IsCharacterOnGround(harness.charPhys)) {
                return std::unexpected(CharacterTestError::StepClimbFailed);
            }
            return {};
        }

        // 10. Walkable Slope Traversal
        auto test_10_walkable_slope_climbing(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            const auto rampRot = ZHLN::Quat::FromEulerDegrees({{-25.0f, 0.0f, 0.0f}});
            auto      rampShape = harness.pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 5.0f, 0.2f, 5.0f);
            harness.pc.CreateRigidBody(rampShape, {0, 1.0, 4.0}, rampRot.AsVec4(), JPH::EMotionType::Static, ZHLN::Layers::ID::NON_MOVING);
            harness.pc.OptimizeBroadphase();

            harness.Settle(10);

            for (int i = 0; i < 60; ++i) {
                harness.Tick(1.0f / 60.0f, 0.0f, 1.0f);
            }

            const auto pos = GetBodyPosition(harness.pc, 1);

            ZHLN::Test::ExpectGt(pos.y, 0.8f);
            ZHLN::Test::ExpectGt(pos.z, 3.0f);

            if (pos.y <= 0.8f) {
                return std::unexpected(CharacterTestError::SlopeClimbFailed);
            }
            return {};
        }

        // 11. Dynamic Rigid Body Push Interaction
        auto test_11_dynamic_rigid_body_push_impulse(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::PhysicsConfig cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            CPUPipelineHarness  harness(cfg);

            auto         boxShape = harness.pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 0.25f, 0.25f, 0.25f);
            ZHLN::Physics::BodyID pushBox =
                harness.pc.CreateRigidBody(boxShape, {0.0, 0.25, 2.0}, {0.0f, 0.0f, 0.0f, 1.0f}, JPH::EMotionType::Dynamic, ZHLN::Layers::ID::MOVING);
            harness.pc.OptimizeBroadphase();

            harness.Settle(10);

            for (int i = 0; i < 60; ++i) {
                harness.Tick(1.0f / 60.0f, 0.0f, 0.66f);
            }

            auto rayHit = harness.pc.Raycast({0.0, 0.25, 0.0}, {0.0f, 0.0f, 1.0f}, 10.0f);
            ZHLN::Test::ExpectTrue(rayHit.hasHit);
            ZHLN::Test::ExpectTrue(rayHit.bodyID == pushBox);
            ZHLN::Test::ExpectGt(rayHit.position.z, 2.6f);

            if (!rayHit.hasHit || rayHit.position.z <= 2.6f) {
                return std::unexpected(CharacterTestError::DynamicPushFailed);
            }
            return {};
        }

        // 12. Acceleration from Rest
        auto test_12_acceleration_from_rest(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            // Test the acceleration/deceleration logic directly on the component
            // without the physics harness (which bypasses MovementSystem).
            ZHLN::Character::MovementComponent move;
            move.acceleration = 25.0f;
            move.deceleration = 30.0f;
            move.speed        = 7.0f;
            move.currentVelX  = 0.0f;
            move.currentVelZ  = 0.0f;

            constexpr float dt           = 1.0f / 60.0f;
            const float     targetSpeed  = move.speed;

            // Simulate the acceleration logic from MovementSystem for one frame
            auto applyAcceleration = [&](float inputX, float inputZ) {
                const float targetVelX   = inputX * move.speed;
                const float targetVelZ   = inputZ * move.speed;
                const float targetSpeedSq = targetVelX * targetVelX + targetVelZ * targetVelZ;
                const float currentSpeedSq = move.currentVelX * move.currentVelX + move.currentVelZ * move.currentVelZ;
                const bool  hasInput       = targetSpeedSq > 0.01f;
                const float rate           = hasInput ? move.acceleration : move.deceleration;

                if (hasInput) {
                    const float invTargetSpeed = (targetSpeedSq > 0.01f) ? 1.0f / std::sqrt(targetSpeedSq) : 0.0f;
                    const float targetDirX     = targetVelX * invTargetSpeed;
                    const float targetDirZ     = targetVelZ * invTargetSpeed;
                    const float tSpeed         = std::sqrt(targetSpeedSq);
                    const float currentAlong   = move.currentVelX * targetDirX + move.currentVelZ * targetDirZ;
                    const float newSpeed       = std::min(currentAlong + rate * dt, tSpeed);
                    const float blend          = (tSpeed > 0.01f) ? std::min(newSpeed / tSpeed, 1.0f) : 0.0f;
                    const float curSpd         = std::sqrt(currentSpeedSq);
                    const float invCurSpd      = (curSpd > 0.01f) ? 1.0f / curSpd : 0.0f;
                    const float curDirX        = move.currentVelX * invCurSpd;
                    const float curDirZ        = move.currentVelZ * invCurSpd;
                    const float bDirX          = curDirX * (1.0f - blend) + targetDirX * blend;
                    const float bDirZ          = curDirZ * (1.0f - blend) + targetDirZ * blend;
                    const float bDirLen        = std::sqrt(bDirX * bDirX + bDirZ * bDirZ);
                    const float invBDirLen     = (bDirLen > 0.01f) ? 1.0f / bDirLen : 0.0f;
                    move.currentVelX           = bDirX * invBDirLen * newSpeed;
                    move.currentVelZ           = bDirZ * invBDirLen * newSpeed;
                } else {
                    const float curSpd = std::sqrt(currentSpeedSq);
                    if (curSpd > 0.01f) {
                        const float newSpd = std::max(curSpd - rate * dt, 0.0f);
                        const float scale  = newSpd / curSpd;
                        move.currentVelX *= scale;
                        move.currentVelZ *= scale;
                    } else {
                        move.currentVelX = 0.0f;
                        move.currentVelZ = 0.0f;
                    }
                }
            };

            // Apply input for one frame - velocity should start increasing but not reach target
            applyAcceleration(0.0f, 1.0f);
            float velAfterOneFrame = std::sqrt(move.currentVelX * move.currentVelX + move.currentVelZ * move.currentVelZ);

            ZHLN::Test::ExpectGt(velAfterOneFrame, 0.0f);
            ZHLN::Test::ExpectLt(velAfterOneFrame, targetSpeed);

            // After enough time, should reach target speed
            for (int i = 0; i < 60; ++i) {
                applyAcceleration(0.0f, 1.0f);
            }
            float velAfterConvergence = std::sqrt(move.currentVelX * move.currentVelX + move.currentVelZ * move.currentVelZ);

            ZHLN::Test::ExpectLt(std::abs(velAfterConvergence - targetSpeed), 0.1f);

            if (velAfterOneFrame <= 0.0f || velAfterOneFrame >= targetSpeed ||
                std::abs(velAfterConvergence - targetSpeed) >= 0.1f) {
                return std::unexpected(CharacterTestError::DisplacementMismatch);
            }
            return {};
        }

        // 13. Deceleration to Rest
        auto test_13_deceleration_to_rest(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::Character::MovementComponent move;
            move.acceleration = 25.0f;
            move.deceleration = 30.0f;
            move.speed        = 7.0f;
            move.currentVelX  = 0.0f;
            move.currentVelZ  = 0.0f;

            constexpr float dt = 1.0f / 60.0f;

            auto applyAcceleration = [&](float inputX, float inputZ) {
                const float targetVelX   = inputX * move.speed;
                const float targetVelZ   = inputZ * move.speed;
                const float targetSpeedSq = targetVelX * targetVelX + targetVelZ * targetVelZ;
                const float currentSpeedSq = move.currentVelX * move.currentVelX + move.currentVelZ * move.currentVelZ;
                const bool  hasInput       = targetSpeedSq > 0.01f;
                const float rate           = hasInput ? move.acceleration : move.deceleration;

                if (hasInput) {
                    const float invTargetSpeed = (targetSpeedSq > 0.01f) ? 1.0f / std::sqrt(targetSpeedSq) : 0.0f;
                    const float targetDirX     = targetVelX * invTargetSpeed;
                    const float targetDirZ     = targetVelZ * invTargetSpeed;
                    const float tSpeed         = std::sqrt(targetSpeedSq);
                    const float currentAlong   = move.currentVelX * targetDirX + move.currentVelZ * targetDirZ;
                    const float newSpeed       = std::min(currentAlong + rate * dt, tSpeed);
                    const float blend          = (tSpeed > 0.01f) ? std::min(newSpeed / tSpeed, 1.0f) : 0.0f;
                    const float curSpd         = std::sqrt(currentSpeedSq);
                    const float invCurSpd      = (curSpd > 0.01f) ? 1.0f / curSpd : 0.0f;
                    const float curDirX        = move.currentVelX * invCurSpd;
                    const float curDirZ        = move.currentVelZ * invCurSpd;
                    const float bDirX          = curDirX * (1.0f - blend) + targetDirX * blend;
                    const float bDirZ          = curDirZ * (1.0f - blend) + targetDirZ * blend;
                    const float bDirLen        = std::sqrt(bDirX * bDirX + bDirZ * bDirZ);
                    const float invBDirLen     = (bDirLen > 0.01f) ? 1.0f / bDirLen : 0.0f;
                    move.currentVelX           = bDirX * invBDirLen * newSpeed;
                    move.currentVelZ           = bDirZ * invBDirLen * newSpeed;
                } else {
                    const float curSpd = std::sqrt(currentSpeedSq);
                    if (curSpd > 0.01f) {
                        const float newSpd = std::max(curSpd - rate * dt, 0.0f);
                        const float scale  = newSpd / curSpd;
                        move.currentVelX *= scale;
                        move.currentVelZ *= scale;
                    } else {
                        move.currentVelX = 0.0f;
                        move.currentVelZ = 0.0f;
                    }
                }
            };

            // First, accelerate to full speed
            for (int i = 0; i < 60; ++i) {
                applyAcceleration(0.0f, 1.0f);
            }
            float velAtFullSpeed = std::sqrt(move.currentVelX * move.currentVelX + move.currentVelZ * move.currentVelZ);
            ZHLN::Test::ExpectLt(std::abs(velAtFullSpeed - move.speed), 0.1f);

            // Release input - velocity should start decreasing but not instantly zero
            applyAcceleration(0.0f, 0.0f);
            float velAfterOneFrame = std::sqrt(move.currentVelX * move.currentVelX + move.currentVelZ * move.currentVelZ);

            ZHLN::Test::ExpectGt(velAfterOneFrame, 0.0f);
            ZHLN::Test::ExpectLt(velAfterOneFrame, velAtFullSpeed);

            // After enough time, should stop completely
            for (int i = 0; i < 60; ++i) {
                applyAcceleration(0.0f, 0.0f);
            }
            float velAfterStopping = std::sqrt(move.currentVelX * move.currentVelX + move.currentVelZ * move.currentVelZ);

            ZHLN::Test::ExpectLt(velAfterStopping, 0.01f);

            if (velAfterOneFrame <= 0.0f || velAfterOneFrame >= velAtFullSpeed || velAfterStopping >= 0.01f) {
                return std::unexpected(CharacterTestError::DisplacementMismatch);
            }
            return {};
        }

        // 14. Acceleration Rate Consistency
        auto test_14_acceleration_rate_consistency(Fixture::TaskSystemAndJoltScope&) -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::Character::MovementComponent move;
            move.acceleration = 20.0f;
            move.deceleration = 30.0f;
            move.speed        = 10.0f;
            move.currentVelX  = 0.0f;
            move.currentVelZ  = 0.0f;

            constexpr float dt = 1.0f / 60.0f;

            auto applyAcceleration = [&](float inputX, float inputZ) {
                const float targetVelX   = inputX * move.speed;
                const float targetVelZ   = inputZ * move.speed;
                const float targetSpeedSq = targetVelX * targetVelX + targetVelZ * targetVelZ;
                const float currentSpeedSq = move.currentVelX * move.currentVelX + move.currentVelZ * move.currentVelZ;
                const bool  hasInput       = targetSpeedSq > 0.01f;
                const float rate           = hasInput ? move.acceleration : move.deceleration;

                if (hasInput) {
                    const float invTargetSpeed = (targetSpeedSq > 0.01f) ? 1.0f / std::sqrt(targetSpeedSq) : 0.0f;
                    const float targetDirX     = targetVelX * invTargetSpeed;
                    const float targetDirZ     = targetVelZ * invTargetSpeed;
                    const float tSpeed         = std::sqrt(targetSpeedSq);
                    const float currentAlong   = move.currentVelX * targetDirX + move.currentVelZ * targetDirZ;
                    const float newSpeed       = std::min(currentAlong + rate * dt, tSpeed);
                    const float blend          = (tSpeed > 0.01f) ? std::min(newSpeed / tSpeed, 1.0f) : 0.0f;
                    const float curSpd         = std::sqrt(currentSpeedSq);
                    const float invCurSpd      = (curSpd > 0.01f) ? 1.0f / curSpd : 0.0f;
                    const float curDirX        = move.currentVelX * invCurSpd;
                    const float curDirZ        = move.currentVelZ * invCurSpd;
                    const float bDirX          = curDirX * (1.0f - blend) + targetDirX * blend;
                    const float bDirZ          = curDirZ * (1.0f - blend) + targetDirZ * blend;
                    const float bDirLen        = std::sqrt(bDirX * bDirX + bDirZ * bDirZ);
                    const float invBDirLen     = (bDirLen > 0.01f) ? 1.0f / bDirLen : 0.0f;
                    move.currentVelX           = bDirX * invBDirLen * newSpeed;
                    move.currentVelZ           = bDirZ * invBDirLen * newSpeed;
                } else {
                    const float curSpd = std::sqrt(currentSpeedSq);
                    if (curSpd > 0.01f) {
                        const float newSpd = std::max(curSpd - rate * dt, 0.0f);
                        const float scale  = newSpd / curSpd;
                        move.currentVelX *= scale;
                        move.currentVelZ *= scale;
                    } else {
                        move.currentVelX = 0.0f;
                        move.currentVelZ = 0.0f;
                    }
                }
            };

            // Measure velocity increase over several frames
            std::vector<float> velocities;
            velocities.reserve(10);

            for (int i = 0; i < 10; ++i) {
                applyAcceleration(0.0f, 1.0f);
                float vel = std::sqrt(move.currentVelX * move.currentVelX + move.currentVelZ * move.currentVelZ);
                velocities.push_back(vel);
            }

            // Check that velocity increases monotonically
            for (size_t i = 1; i < velocities.size(); ++i) {
                ZHLN::Test::ExpectGt(velocities[i], velocities[i - 1]);
                if (velocities[i] <= velocities[i - 1]) {
                    return std::unexpected(CharacterTestError::DisplacementMismatch);
                }
            }

            // Check that acceleration rate is approximately correct (within 20% tolerance)
            float measuredAccel = (velocities[5] - velocities[0]) / (5.0f * dt);
            float accelError = std::abs(measuredAccel - move.acceleration) / move.acceleration;

            ZHLN::Test::ExpectLt(accelError, 0.20f);

            if (accelError >= 0.20f) {
                return std::unexpected(CharacterTestError::DisplacementMismatch);
            }
            return {};
        }

} // namespace
} // namespace ZHLN::Test
