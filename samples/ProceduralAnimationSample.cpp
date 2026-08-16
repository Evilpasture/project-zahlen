// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

// Jolt Physics
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace {

// ============================================================================
// NAMED CONSTANTS & TUNING PARAMETERS
// ============================================================================

// Wireframe Density & Geometry
inline constexpr int   kSphereLatRings  = 10;
inline constexpr int   kSphereLongRings = 10;
inline constexpr int   kRingSegments    = 36;
inline constexpr float kTwoPi           = 2.0f * std::numbers::pi_v<float>;

// Accurate Normalized Wireframe Colors
inline const JPH::Vec4 kColorBumper {0.10f, 1.00f, 0.20f, 1.0f};        // Vibrant Green
inline const JPH::Vec4 kColorLifter {1.00f, 1.00f, 1.00f, 1.0f};        // Pure White
inline const JPH::Vec4 kColorVelocityDebug {1.00f, 0.90f, 0.10f, 1.0f}; // Yellow
inline const JPH::Vec4 kColorNormalDebug {0.10f, 0.80f, 1.00f, 1.0f};   // Cyan

// Locomotion Kinematics
inline constexpr float kDefaultMaxRunSpeed      = 14.5f;
inline constexpr float kDefaultMaxSprintSpeed   = 22.0f;
inline constexpr float kDefaultGroundAccel      = 68.0f;
inline constexpr float kDefaultGroundFriction   = 34.0f;
inline constexpr float kDefaultPivotBraking     = 72.0f;
inline constexpr float kDefaultAirAccel         = 32.0f;
inline constexpr float kDefaultAirDrag          = 1.2f;
inline constexpr float kDefaultGravity          = -34.0f;
inline constexpr float kDefaultJumpImpulse      = 14.8f;
inline constexpr float kDefaultTerminalVelocity = -48.0f;
inline constexpr float kDefaultTurnRate         = 18.0f;

// Dual-Shape Rig Dimensions (Coaxial, Fixed Relative Offsets)
inline constexpr float kMaxStepHeight     = 0.75f;
inline constexpr float kMaxStepDownHeight = 0.35f;

// Lower White Lifter Sphere (Slightly increased from 0.55m -> 0.58m)
inline constexpr float kLifterRadius  = 0.58f;
inline constexpr float kLifterOffsetY = kLifterRadius;

// Upper Green Bumper Oval-Sphere
inline constexpr float kBumperRadiusXZ = 0.70f;
inline constexpr float kBumperRadiusY  = 0.85f;

// Exact analytical offset: Y_B = Y_L + R_y * sqrt(1 - (0.58 / 0.70)^2)
// sqrt(1 - 0.68653) = 0.55988
inline constexpr float kBumperOffsetY = kLifterOffsetY + (kBumperRadiusY * 0.55988f); // 1.0559m

inline constexpr double kInitialPlayerSpawnX = 0.0;
inline constexpr double kInitialPlayerSpawnZ = 0.0;
inline constexpr float  kPlayerInitPosY      = 4.0f;

// Raycast & Probing Tolerances
inline constexpr float kEpsilon               = 0.001f;
inline constexpr float kInputDeadzone         = 0.01f;
inline constexpr float kVelocityThresholdLow  = 0.05f;
inline constexpr float kVelocityThresholdMed  = 0.10f;
inline constexpr float kGroundedFallPadding   = 0.35f;
inline constexpr float kAirborneFallPadding   = 0.45f;
inline constexpr float kBumperProbePadding    = 0.08f;
inline constexpr float kPivotThresholdSpeed   = 2.00f;
inline constexpr float kPivotDotThreshold     = -0.25f;
inline constexpr float kWallSlopeThreshold    = 0.70f;
inline constexpr float kDegreesRightAngle     = 90.0f;
inline constexpr float kVelocityDebugScale    = 0.25f;
inline constexpr float kNormalDebugLength     = 1.20f;
inline constexpr float kGroundedVelocityLimit = 0.50f;

// Audio Feedback
inline constexpr float kJumpAudioFrequency = 520.0f;
inline constexpr float kJumpAudioDuration  = 0.08f;
inline constexpr float kJumpAudioVolume    = 0.25f;

// Calibrated Lighting
inline constexpr float kAmbientExposure = 10.0f;
inline constexpr float kSunIntensity    = 28.0f;
inline constexpr float kSunDirectionX   = 0.45f;
inline constexpr float kSunDirectionY   = 1.00f;
inline constexpr float kSunDirectionZ   = 0.30f;
inline const JPH::Vec3 kSunPosition {25.0f, 60.0f, 25.0f};
inline const JPH::Vec3 kSunColor {1.00f, 0.96f, 0.90f};
inline const JPH::Vec4 kSkyZenith {0.25f, 0.55f, 0.95f, 1.0f};
inline const JPH::Vec4 kSkyHorizon {0.70f, 0.85f, 1.00f, 1.0f};
inline const JPH::Vec4 kSkyGround {0.20f, 0.28f, 0.20f, 1.0f};

// World Dimensions & Materials
inline constexpr int   kTerrainSamples   = 128;
inline constexpr float kTerrainWorldSize = 220.0f;
inline constexpr float kTerrainMaxHeight = 12.0f;
inline constexpr float kTerrainRoughness = 0.80f;
inline constexpr float kTerrainMetallic  = 0.02f;

inline constexpr float  kPlatformHalfExtentsXZ = 10.0f;
inline constexpr float  kPlatformHalfHeight    = 0.50f;
inline constexpr double kPlatformPosY          = 0.50;
inline constexpr float  kPlatformRoughness     = 0.50f;
inline constexpr float  kPlatformMetallic      = 0.10f;
inline const JPH::Vec4  kPlatformColor {0.32f, 0.34f, 0.38f, 1.0f};

inline constexpr int   kStepObstacleCount = 6;
inline constexpr float kStepBaseHeight    = 0.20f;
inline constexpr float kStepHeightStep    = 0.10f;
inline constexpr float kStepStartX        = 12.0f;
inline constexpr float kStepSpacingX      = 2.50f;
inline constexpr float kStepPosZ          = -4.0f;
inline constexpr float kStepHalfWidthXZ   = 1.00f;
inline constexpr float kStepRoughness     = 0.45f;
inline constexpr float kStepMetallic      = 0.15f;
inline const JPH::Vec4 kStepColor {0.65f, 0.40f, 0.20f, 1.0f};

inline constexpr float kPillarLightTriggerHeight = 6.00f;
inline constexpr float kPillarLightOffsetY       = 0.80f;
inline constexpr float kPillarLightRadius        = 0.35f;
inline constexpr float kPillarLightRange         = 10.0f;
inline constexpr float kPillarLightIntensity     = 10.0f;
inline const JPH::Vec3 kPillarLightColor {1.00f, 0.60f, 0.20f};
inline constexpr float kPillarRoughness = 0.40f;
inline constexpr float kPillarMetallic  = 0.15f;

// Engine Configuration Constants
inline constexpr uint32_t kMaxPhysicsBodies         = 2048;
inline constexpr uint32_t kMaxPhysicsPairs          = 4096;
inline constexpr uint32_t kMaxPhysicsConstraints    = 4096;
inline constexpr uint32_t kPhysicsTempAllocatorSize = 32 * 1024 * 1024;
inline constexpr uint32_t kDefaultWindowWidth       = 1280;
inline constexpr uint32_t kDefaultWindowHeight      = 720;

// Camera & Interaction Parameters
inline constexpr float  kMaxDeltaTimeCap       = 0.05f;
inline constexpr float  kMouseLookSensitivity  = 0.15f;
inline constexpr float  kMinCameraPitchLimit   = -85.0f;
inline constexpr float  kMaxCameraPitchLimit   = 85.0f;
inline constexpr float  kCameraFollowSharpness = 32.0f;
inline constexpr float  kCameraFollowDistance  = 5.80f;
inline constexpr float  kCameraTargetOffsetY   = 1.20f;
inline constexpr float  kInitialCameraYaw      = 90.0f;
inline constexpr float  kInitialCameraPitch    = -12.0f;
inline constexpr float  kInitialCameraFOV      = 52.0f;
inline constexpr double kInitialCameraInitY    = 6.50;
inline constexpr double kInitialCameraInitZ    = -12.0;

// ============================================================================
// 1. WIREFRAME RENDERING HELPERS
// ============================================================================

struct EllipsoidDesc {
    float radiusXZ = 0.0f;
    float radiusY  = 0.0f;
    int   latRings = kSphereLatRings;
    int   lonRings = kSphereLongRings;
    int   segments = kRingSegments;
};

/**
 * @brief Draws a 3D wireframe ellipsoid (oval sphere) with latitude rings and longitudinal meridians.
 */
auto DrawWireframeEllipsoid(ZHLN::RenderContext& rc, const JPH::Vec3& center, EllipsoidDesc desc, const JPH::Vec4& color) noexcept -> void {
    // 1. Latitudinal Parallel Rings
    for (int lat = 1; lat < desc.latRings; ++lat) {
        const auto phi        = -std::numbers::pi_v<float> * 0.5f + (static_cast<float>(lat) / static_cast<float>(desc.latRings)) * std::numbers::pi_v<float>;
        const auto ringY      = center.GetY() + (std::sin(phi) * desc.radiusY);
        const auto ringRadius = desc.radiusXZ * std::cos(phi);

        for (int i = 0; i < desc.segments; ++i) {
            const auto t0 = (static_cast<float>(i) / static_cast<float>(desc.segments)) * kTwoPi;
            const auto t1 = (static_cast<float>(i + 1) / static_cast<float>(desc.segments)) * kTwoPi;

            const JPH::Vec3 p0(center.GetX() + (std::cos(t0) * ringRadius), ringY, center.GetZ() + (std::sin(t0) * ringRadius));
            const JPH::Vec3 p1(center.GetX() + (std::cos(t1) * ringRadius), ringY, center.GetZ() + (std::sin(t1) * ringRadius));

            rc.DrawLine(p0, p1, color);
        }
    }

    // 2. Longitudinal Meridian Ellipses
    for (int lon = 0; lon < desc.lonRings; ++lon) {
        const auto lonAngle = (static_cast<float>(lon) / static_cast<float>(desc.lonRings)) * std::numbers::pi_v<float>;
        const auto cosLon   = std::cos(lonAngle);
        const auto sinLon   = std::sin(lonAngle);

        for (int i = 0; i < desc.segments; ++i) {
            const auto t0 = (static_cast<float>(i) / static_cast<float>(desc.segments)) * kTwoPi;
            const auto t1 = (static_cast<float>(i + 1) / static_cast<float>(desc.segments)) * kTwoPi;

            const auto r0 = desc.radiusXZ * std::cos(t0);
            const auto y0 = center.GetY() + (desc.radiusY * std::sin(t0));
            const auto r1 = desc.radiusXZ * std::cos(t1);
            const auto y1 = center.GetY() + (desc.radiusY * std::sin(t1));

            const JPH::Vec3 p0(center.GetX() + (r0 * cosLon), y0, center.GetZ() + (r0 * sinLon));
            const JPH::Vec3 p1(center.GetX() + (r1 * cosLon), y1, center.GetZ() + (r1 * sinLon));

            rc.DrawLine(p0, p1, color);
        }
    }
}

/**
 * @brief Draws a 3D wireframe sphere.
 */
auto DrawWireframeSphere(ZHLN::RenderContext& rc, const JPH::Vec3& center, float radius, const JPH::Vec4& color) noexcept -> void {
    DrawWireframeEllipsoid(rc, center, {.radiusXZ = radius, .radiusY = radius}, color);
}

// ============================================================================
// 2. LOCOMOTION CONTROLLER
// ============================================================================

struct LocomotionParams {
    float maxRunSpeed      = kDefaultMaxRunSpeed;
    float maxSprintSpeed   = kDefaultMaxSprintSpeed;
    float groundAccel      = kDefaultGroundAccel;
    float groundFriction   = kDefaultGroundFriction;
    float pivotBraking     = kDefaultPivotBraking;
    float airAccel         = kDefaultAirAccel;
    float airDrag          = kDefaultAirDrag;
    float gravity          = kDefaultGravity;
    float jumpImpulse      = kDefaultJumpImpulse;
    float terminalVelocity = kDefaultTerminalVelocity;
    float turnRate         = kDefaultTurnRate;

    float maxStepHeight = kMaxStepHeight;

    // Dual-Shape Rig Dimensions
    float bumperRadiusXZ = kBumperRadiusXZ;
    float bumperRadiusY  = kBumperRadiusY;
    float bumperOffsetY  = kBumperOffsetY;

    float lifterRadius  = kLifterRadius;
    float lifterOffsetY = kLifterOffsetY;
};

class PhysicsLocomotionController {
  public:
    JPH::Vec3        position     = JPH::Vec3(static_cast<float>(kInitialPlayerSpawnX), kPlayerInitPosY, static_cast<float>(kInitialPlayerSpawnZ));
    JPH::Vec3        velocity     = JPH::Vec3::sZero();
    JPH::Quat        orientation  = JPH::Quat::sIdentity();
    JPH::Vec3        groundNormal = JPH::Vec3::sAxisY();
    bool             isGrounded   = false;
    LocomotionParams params {};

    auto Update(ZHLN::Engine& engine, const ZHLN::Camera& cam, float dt) -> void {
        auto& reg = engine.GetRegistry();
        auto& pc  = engine.GetPhysicsContext();
        auto& rc  = engine.GetRenderContext();

        // 1. Camera-Relative Input Vector
        float inputX      = 0.0f;
        float inputZ      = 0.0f;
        bool  wantsSprint = false;
        bool  wantsJump   = false;

        for (ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::InputStateComponent>()) {
            ZHLN::ECS::Patch<ZHLN::Components::InputStateComponent>(reg, e, [&](const auto& state) -> void {
                if (state.IsKeyDown(static_cast<uint8_t>(ZHLN::KeyCode::W))) {
                    inputZ += 1.0f;
                }
                if (state.IsKeyDown(static_cast<uint8_t>(ZHLN::KeyCode::S))) {
                    inputZ -= 1.0f;
                }
                if (state.IsKeyDown(static_cast<uint8_t>(ZHLN::KeyCode::A))) {
                    inputX -= 1.0f;
                }
                if (state.IsKeyDown(static_cast<uint8_t>(ZHLN::KeyCode::D))) {
                    inputX += 1.0f;
                }

                wantsSprint = state.IsKeyDown(static_cast<uint8_t>(ZHLN::KeyCode::LShift));
                wantsJump   = state.IsKeyDown(static_cast<uint8_t>(ZHLN::KeyCode::Space));
            });
        }

        const auto yawRad = JPH::DegreesToRadians(cam.yaw);
        JPH::Vec3  camFwd(std::cos(yawRad), 0.0f, std::sin(yawRad));
        camFwd = camFwd.Normalized();
        const JPH::Vec3 camRight(-camFwd.GetZ(), 0.0f, camFwd.GetX());

        JPH::Vec3 wishDir  = (camFwd * inputZ) + (camRight * inputX);
        auto      inputLen = wishDir.Length();
        if (inputLen > kEpsilon) {
            wishDir /= inputLen;
            inputLen = std::min(inputLen, 1.0f);
        }

        // 2. Physical 3D Lifter SphereCast (With Step-Down Ledge Protection)
        JPH::ShapeRefC lifterShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Sphere, params.lifterRadius);

        const float stepUpAllowance = params.maxStepHeight;
        const float maxFallDistance = isGrounded ? (stepUpAllowance + kGroundedFallPadding) : (std::max(0.0f, -velocity.GetY() * dt) + kAirborneFallPadding);
        const float castDistance    = stepUpAllowance + maxFallDistance;

        const JPH::RVec3 lifterCastStart(
            static_cast<JPH::Real>(position.GetX()), static_cast<JPH::Real>(position.GetY() + params.lifterRadius + stepUpAllowance),
            static_cast<JPH::Real>(position.GetZ())
        );
        const auto groundCast = pc.Shapecast(lifterShape, lifterCastStart, JPH::Quat::sIdentity(), -JPH::Vec3::sAxisY(), castDistance);

        if (groundCast.hasHit && groundCast.contactNormal.GetY() >= kWallSlopeThreshold && velocity.GetY() <= kGroundedVelocityLimit) {
            const float groundCenterY = static_cast<float>(lifterCastStart.GetY()) - (groundCast.fraction * castDistance);
            const float targetPosY    = groundCenterY - params.lifterRadius;
            const float deltaY        = targetPosY - position.GetY();

            // Only snap if stepping up or small step-down; otherwise fall naturally over ledges
            if (deltaY >= -kMaxStepDownHeight && deltaY <= stepUpAllowance) {
                isGrounded   = true;
                groundNormal = groundCast.contactNormal;
                position.SetY(targetPosY);
                if (velocity.GetY() < 0.0f) {
                    velocity.SetY(0.0f);
                }
            } else {
                isGrounded   = false;
                groundNormal = JPH::Vec3::sAxisY();
            }
        } else {
            isGrounded   = false;
            groundNormal = JPH::Vec3::sAxisY();
        }

        // 3. Acceleration, Friction, and Pivot Braking
        float targetSpeed = (wantsSprint ? params.maxSprintSpeed : params.maxRunSpeed) * inputLen;

        JPH::Vec3  flatVel(velocity.GetX(), 0.0f, velocity.GetZ());
        const auto currentFlatSpeed = flatVel.Length();

        if (isGrounded) {
            if (inputLen > kInputDeadzone) {
                if (currentFlatSpeed > kPivotThresholdSpeed && flatVel.Normalized().Dot(wishDir) < kPivotDotThreshold) {
                    flatVel -= flatVel * (params.pivotBraking * dt);
                } else {
                    flatVel += wishDir * (params.groundAccel * dt);
                    if (flatVel.Length() > targetSpeed) {
                        flatVel = flatVel.Normalized() * targetSpeed;
                    }
                }
            } else {
                const auto drop     = params.groundFriction * dt;
                const auto newSpeed = std::max(0.0f, currentFlatSpeed - drop);
                flatVel             = (currentFlatSpeed > kEpsilon) ? (flatVel * (newSpeed / currentFlatSpeed)) : JPH::Vec3::sZero();
            }

            if (wantsJump) {
                velocity.SetY(params.jumpImpulse);
                isGrounded = false;
                engine.GetAudioContext().PlayProceduralBeep(kJumpAudioFrequency, kJumpAudioDuration, kJumpAudioVolume);
            }
        } else {
            if (inputLen > kInputDeadzone) {
                flatVel += wishDir * (params.airAccel * dt);
                if (flatVel.Length() > targetSpeed) {
                    flatVel = flatVel.Normalized() * targetSpeed;
                }
            }
            flatVel *= std::exp(-params.airDrag * dt);
            velocity.SetY(std::max(params.terminalVelocity, velocity.GetY() + (params.gravity * dt)));
        }

        velocity.SetX(flatVel.GetX());
        velocity.SetZ(flatVel.GetZ());

        // 4. Dual Horizontal Sweeps (Prevents both Lifter & Bumper from penetrating walls/ledges)
        if (flatVel.LengthSq() > kVelocityThresholdLow) {
            JPH::Vec3 moveDir  = flatVel.Normalized();
            float     moveDist = flatVel.Length() * dt;

            // A. Lifter Sweep
            const JPH::RVec3 lifterSweepStart(
                static_cast<JPH::Real>(position.GetX()), static_cast<JPH::Real>(position.GetY() + params.lifterOffsetY), static_cast<JPH::Real>(position.GetZ())
            );
            auto lifterWallHit = pc.Shapecast(lifterShape, lifterSweepStart, JPH::Quat::sIdentity(), moveDir, moveDist + kBumperProbePadding);
            if (lifterWallHit.hasHit && lifterWallHit.contactNormal.GetY() < kWallSlopeThreshold) {
                JPH::Vec3 wallNorm = JPH::Vec3(lifterWallHit.contactNormal.GetX(), 0.0f, lifterWallHit.contactNormal.GetZ()).Normalized();
                float     intoWall = flatVel.Dot(wallNorm);
                if (intoWall < 0.0f) {
                    flatVel -= wallNorm * intoWall;
                    velocity.SetX(flatVel.GetX());
                    velocity.SetZ(flatVel.GetZ());
                }
            }

            // B. Bumper Sweep
            JPH::ShapeRefC   bumperShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Sphere, params.bumperRadiusXZ);
            const JPH::RVec3 bumperSweepStart(
                static_cast<JPH::Real>(position.GetX()), static_cast<JPH::Real>(position.GetY() + params.bumperOffsetY), static_cast<JPH::Real>(position.GetZ())
            );
            auto bumperWallHit = pc.Shapecast(bumperShape, bumperSweepStart, JPH::Quat::sIdentity(), moveDir, moveDist + kBumperProbePadding);
            if (bumperWallHit.hasHit && bumperWallHit.contactNormal.GetY() < kWallSlopeThreshold) {
                JPH::Vec3 wallNorm = JPH::Vec3(bumperWallHit.contactNormal.GetX(), 0.0f, bumperWallHit.contactNormal.GetZ()).Normalized();
                float     intoWall = flatVel.Dot(wallNorm);
                if (intoWall < 0.0f) {
                    flatVel -= wallNorm * intoWall;
                    velocity.SetX(flatVel.GetX());
                    velocity.SetZ(flatVel.GetZ());
                }
            }
        }

        // 5. Integrate Position & Align Rotation
        position += velocity * dt;

        if (flatVel.LengthSq() > kVelocityThresholdMed) {
            const float     targetYaw = std::atan2(-flatVel.GetZ(), flatVel.GetX()) + JPH::DegreesToRadians(kDegreesRightAngle);
            const JPH::Quat targetRot = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), targetYaw);
            orientation               = orientation.SLERP(targetRot, std::clamp(params.turnRate * dt, 0.0f, 1.0f)).Normalized();
        }

        // 6. Render Coaxial Dual-Shape Rig (Mathematically exact equator intersection)
        const JPH::Vec3 finalBumperCenter = position + JPH::Vec3(0.0f, params.bumperOffsetY, 0.0f);
        const JPH::Vec3 finalLifterCenter = position + JPH::Vec3(0.0f, params.lifterOffsetY, 0.0f);

        DrawWireframeEllipsoid(rc, finalBumperCenter, {.radiusXZ = params.bumperRadiusXZ, .radiusY = params.bumperRadiusY}, kColorBumper);

        DrawWireframeSphere(rc, finalLifterCenter, params.lifterRadius, kColorLifter);

        rc.DrawLine(finalBumperCenter, finalBumperCenter + velocity * kVelocityDebugScale, kColorVelocityDebug);

        if (isGrounded) {
            rc.DrawLine(position, position + groundNormal * kNormalDebugLength, kColorNormalDebug);
        }
    }
};

// ============================================================================
// 3. ARENA SETUP
// ============================================================================

auto BuildProceduralArena(ZHLN::Engine& engine) -> void {
    auto& reg = engine.GetRegistry();

    // 1. Calibrated Daylight PBR Atmosphere
    for (ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>()) {
        ZHLN::ECS::Patch<ZHLN::Components::PostProcessSettingsComponent>(reg, e, [](auto& pp) -> void {
            pp.ambientExposure = kAmbientExposure;
            pp.skyZenith       = kSkyZenith;
            pp.skyHorizon      = kSkyHorizon;
            pp.skyGround       = kSkyGround;
        });
    }

    ZHLN::Log("[ProceduralAnimationSample] Building Heightmap Terrain...");

    // 2. Procedural Terrain
    ZHLN::CreativeWorksFactory::CreateTerrain(
        engine, kTerrainSamples, kTerrainWorldSize, kTerrainMaxHeight, ZHLN::CreativeWorksFactory::TerrainType::Default,
        ZHLN::CreativeWorksFactory::SpawnParams {
            .position        = {kInitialPlayerSpawnX, kInitialPlayerSpawnX, kInitialPlayerSpawnX},
            .createPhysics   = true,
            .isStaticPhysics = true,
            .roughness       = kTerrainRoughness,
            .metallic        = kTerrainMetallic
        }
    );

    // 3. Center Platform
    ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(kPlatformHalfExtentsXZ, kPlatformHalfHeight, kPlatformHalfExtentsXZ),
        ZHLN::CreativeWorksFactory::SpawnParams {
            .position        = {kInitialPlayerSpawnX, kPlatformPosY, kInitialPlayerSpawnX},
            .createPhysics   = true,
            .isStaticPhysics = true,
            .roughness       = kPlatformRoughness,
            .metallic        = kPlatformMetallic,
            .color           = kPlatformColor
        }
    );

    // 4. Stepping Stones
    for (int i = 0; i < kStepObstacleCount; ++i) {
        const auto stepHeight = kStepBaseHeight + (static_cast<float>(i) * kStepHeightStep);
        const auto posX       = static_cast<double>(kStepStartX + (static_cast<float>(i) * kStepSpacingX));
        const auto posY       = static_cast<double>(stepHeight * 0.5f);
        const auto posZ       = static_cast<double>(kStepPosZ);

        ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(kStepHalfWidthXZ, stepHeight * 0.5f, kStepHalfWidthXZ),
            ZHLN::CreativeWorksFactory::SpawnParams {
                .position        = {posX, posY, posZ},
                .createPhysics   = true,
                .isStaticPhysics = true,
                .roughness       = kStepRoughness,
                .metallic        = kStepMetallic,
                .color           = kStepColor
            }
        );
    }

    // 5. Static Obstacle Pillars
    struct PillarDesc {
        float     x      = 0.0f;
        float     z      = 0.0f;
        float     width  = 0.0f;
        float     height = 0.0f;
        JPH::Vec4 color  = JPH::Vec4::sZero();
    };

    const std::array<PillarDesc, 8> pillars = {
        {{.x = -14.0f, .z = -8.0f, .width = 1.5f, .height = 5.0f, .color = JPH::Vec4(0.35f, 0.45f, 0.60f, 1.0f)},
         {.x = -18.0f, .z = 4.0f, .width = 2.0f, .height = 7.5f, .color = JPH::Vec4(0.30f, 0.40f, 0.55f, 1.0f)},
         {.x = -8.0f, .z = 16.0f, .width = 1.2f, .height = 4.0f, .color = JPH::Vec4(0.40f, 0.50f, 0.65f, 1.0f)},
         {.x = 8.0f, .z = 18.0f, .width = 1.8f, .height = 6.0f, .color = JPH::Vec4(0.35f, 0.45f, 0.60f, 1.0f)},
         {.x = 18.0f, .z = 12.0f, .width = 2.2f, .height = 9.0f, .color = JPH::Vec4(0.25f, 0.35f, 0.50f, 1.0f)},
         {.x = -12.0f, .z = -18.0f, .width = 1.4f, .height = 4.5f, .color = JPH::Vec4(0.38f, 0.48f, 0.62f, 1.0f)},
         {.x = 14.0f, .z = -16.0f, .width = 2.0f, .height = 8.0f, .color = JPH::Vec4(0.28f, 0.38f, 0.52f, 1.0f)},
         {.x = 24.0f, .z = 0.0f, .width = 1.5f, .height = 5.0f, .color = JPH::Vec4(0.35f, 0.45f, 0.60f, 1.0f)}}
    };

    for (const auto& p: pillars) {
        const auto pillarX = static_cast<double>(p.x);
        const auto pillarY = static_cast<double>(p.height * 0.5f);
        const auto pillarZ = static_cast<double>(p.z);

        ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(p.width * 0.5f, p.height * 0.5f, p.width * 0.5f),
            ZHLN::CreativeWorksFactory::SpawnParams {
                .position        = {pillarX, pillarY, pillarZ},
                .createPhysics   = true,
                .isStaticPhysics = true,
                .roughness       = kPillarRoughness,
                .metallic        = kPillarMetallic,
                .color           = p.color
            }
        );

        if (p.height >= kPillarLightTriggerHeight) {
            const JPH::Vec3 lightPos(p.x, p.height + kPillarLightOffsetY, p.z);
            reg.Create(
                ZHLN::Components::NameComponent {.name = ZHLN::String64("PillarLight")}, ZHLN::Components::TransformComponent {.position = lightPos},
                ZHLN::Components::LightComponent {
                    .type      = ZHLN::LightType::Point,
                    .color     = kPillarLightColor,
                    .intensity = kPillarLightIntensity,
                    .radius    = kPillarLightRadius,
                    .range     = kPillarLightRange
                }
            );
        }
    }

    // 6. Directional Sunlight
    reg.Create(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("SunLight")}, ZHLN::Components::TransformComponent {.position = kSunPosition},
        ZHLN::Components::LightComponent {
            .type      = ZHLN::LightType::Sun,
            .color     = kSunColor,
            .intensity = kSunIntensity,
            .direction = JPH::Vec3(kSunDirectionX, kSunDirectionY, kSunDirectionZ).Normalized()
        }
    );
}

} // namespace

// ============================================================================
// 4. MAIN ENTRY POINT
// ============================================================================

auto main(int argc, char* argv[]) -> int {
    // 1. Parse Command Line
    auto optionsRes = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& options = optionsRes.value();

    if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(options.logLevel);
    ZHLN::SetupSignalHandler();
    ZHLN::TaskSystem::Init();
    ZHLN::DefaultPreset::SetDisabled(true);

    // 2. Initialize Engine
    ZHLN::EngineConfig config {
        .physics =
            {.maxBodies             = kMaxPhysicsBodies,
             .maxBodyPairs          = kMaxPhysicsPairs,
             .maxContactConstraints = kMaxPhysicsConstraints,
             .tempAllocatorSize     = kPhysicsTempAllocatorSize},
        .render = {
            .appName        = "Zahlen :: Procedural Locomotion Sample",
            .width          = kDefaultWindowWidth,
            .height         = kDefaultWindowHeight,
            .vsync          = options.vsync,
            .fullscreen     = options.fullscreen,
            .validationMode = options.validationMode,
            .headless       = options.headless
        }
    };

    auto engineRes = ZHLN::Engine::Create(config);
    if (!engineRes) {
        ZHLN::Log("FATAL: Failed to initialize Engine: {}", engineRes.error().Message());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    engine->GetWindow().Focus();
    engine->InitializeDefaultScene();

    BuildProceduralArena(*engine);

    // 3. Instantiate Controller
    PhysicsLocomotionController controller {};
    controller.position = JPH::Vec3(static_cast<float>(kInitialPlayerSpawnX), kPlayerInitPosY, static_cast<float>(kInitialPlayerSpawnZ));

    ZHLN::Clock clock;
    auto&       cam = engine->GetCamera();
    cam.position    = JPH::Vec3(0.0f, static_cast<float>(kInitialCameraInitY), static_cast<float>(kInitialCameraInitZ));
    cam.yaw         = kInitialCameraYaw;
    cam.pitch       = kInitialCameraPitch;
    cam.fov         = kInitialCameraFOV;

    // Fast initial frame anchor
    const auto      initialYawRad   = JPH::DegreesToRadians(cam.yaw);
    const auto      initialPitchRad = JPH::DegreesToRadians(cam.pitch);
    const JPH::Vec3 initialCamForward(
        std::cos(initialYawRad) * std::cos(initialPitchRad), std::sin(initialPitchRad), std::sin(initialYawRad) * std::cos(initialPitchRad)
    );
    cam.position = (controller.position + JPH::Vec3(0.0f, kCameraTargetOffsetY, 0.0f)) - (initialCamForward.Normalized() * kCameraFollowDistance);

    ZHLN::Log("[ProceduralAnimationSample] Ready. Controls: WASD (Move), LSHIFT (Sprint), SPACE (Jump), Right-Click Drag (Camera).");

    // 4. Main Simulation Loop
    while (engine->IsRunning()) {
        const auto dt = std::min(clock.GetDeltaTime(), kMaxDeltaTimeCap);
        engine->ProcessEvents();

        for (ZHLN::Entity e: engine->GetRegistry().GetEntitiesWith<ZHLN::Components::InputStateComponent>()) {
            ZHLN::ECS::Patch<ZHLN::Components::InputStateComponent>(engine->GetRegistry(), e, [&](auto& st) -> void {
                if (st.needsResize) {
                    engine->GetRenderContext().SetResolution(st.newSize);
                    st.needsResize = false;
                }
                if (st.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton))) {
                    cam.yaw += st.GetMouseDeltaX() * kMouseLookSensitivity;
                    cam.pitch = std::clamp(cam.pitch - (st.GetMouseDeltaY() * kMouseLookSensitivity), kMinCameraPitchLimit, kMaxCameraPitchLimit);
                }
            });
        }

        // Advance Kinematics & Render Fixed Coaxial Dual-Shape Rig
        controller.Update(*engine, cam, dt);

        // Snappy Camera Follow
        const auto      yawRad   = JPH::DegreesToRadians(cam.yaw);
        const auto      pitchRad = JPH::DegreesToRadians(cam.pitch);
        const JPH::Vec3 camForward(std::cos(yawRad) * std::cos(pitchRad), std::sin(pitchRad), std::sin(yawRad) * std::cos(pitchRad));

        const JPH::Vec3 targetAnchor = controller.position + JPH::Vec3(0.0f, kCameraTargetOffsetY, 0.0f);
        const JPH::Vec3 idealCamPos  = targetAnchor - (camForward.Normalized() * kCameraFollowDistance);
        const float     followFactor = 1.0f - std::exp(-kCameraFollowSharpness * dt);

        cam.position = cam.position + ((idealCamPos - cam.position) * followFactor);

        const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetWindow().Close();
            break;
        }
    }

    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}
