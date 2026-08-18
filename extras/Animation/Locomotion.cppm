// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

export module ZHLN.Locomotion;

export namespace ZHLN::Locomotion {

inline constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

struct CharacterBoundsEstimate {
    JPH::Vec3 min   = JPH::Vec3::sZero();
    JPH::Vec3 max   = JPH::Vec3::sZero();
    bool      valid = false;

    [[nodiscard]] auto Size() const noexcept -> JPH::Vec3 {
        return valid ? max - min : JPH::Vec3::sZero();
    }
};

struct DualShapeFitOptions {
    float horizontalPadding   = 1.08f;
    float verticalPadding     = 1.06f;
    float lifterToBumperRatio = 0.78f;
    float lifterToHeightRatio = 0.24f;
    float minimumBumperRadius = 0.10f;
};

namespace Detail {

[[nodiscard]] inline auto GetNodeModelTransform(const ModelPrefab& prefab, int32_t nodeIndex) noexcept -> JPH::Mat44 {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
        return JPH::Mat44::sIdentity();
    }

    JPH::Mat44 transform = prefab.nodes[static_cast<size_t>(nodeIndex)].localTransform;
    int32_t    parent    = prefab.nodes[static_cast<size_t>(nodeIndex)].parentIndex;
    for (size_t depth = 0; depth < prefab.nodes.size() && parent >= 0 && parent < static_cast<int32_t>(prefab.nodes.size()); ++depth) {
        transform = prefab.nodes[static_cast<size_t>(parent)].localTransform * transform;
        parent    = prefab.nodes[static_cast<size_t>(parent)].parentIndex;
    }
    return transform;
}

} // namespace Detail

/** Estimates the imported visual envelope from every transformed mesh AABB. */
[[nodiscard]] inline auto
    EstimateCharacterBounds(const ModelPrefab& prefab, JPH::Vec3Arg modelScale = JPH::Vec3::sReplicate(1.0f)) noexcept -> CharacterBoundsEstimate {
    CharacterBoundsEstimate estimate;
    const JPH::Mat44        scaleTransform = JPH::Mat44::sScale(modelScale);

    auto expand = [&](JPH::Vec3Arg point) {
        if (!std::isfinite(point.GetX()) || !std::isfinite(point.GetY()) || !std::isfinite(point.GetZ())) {
            return;
        }
        if (!estimate.valid) {
            estimate.min   = point;
            estimate.max   = point;
            estimate.valid = true;
            return;
        }
        estimate.min =
            JPH::Vec3(std::min(estimate.min.GetX(), point.GetX()), std::min(estimate.min.GetY(), point.GetY()), std::min(estimate.min.GetZ(), point.GetZ()));
        estimate.max =
            JPH::Vec3(std::max(estimate.max.GetX(), point.GetX()), std::max(estimate.max.GetY(), point.GetY()), std::max(estimate.max.GetZ(), point.GetZ()));
    };

    for (const ModelPart& part: prefab.parts) {
        if (part.nodeIndex < 0 || part.nodeIndex >= static_cast<int32_t>(prefab.nodes.size())) {
            continue;
        }
        const JPH::Mat44 transform = scaleTransform * Detail::GetNodeModelTransform(prefab, part.nodeIndex) * part.localTransform;
        const JPH::Vec3  localMin(part.localMin[0], part.localMin[1], part.localMin[2]);
        const JPH::Vec3  localMax(part.localMax[0], part.localMax[1], part.localMax[2]);
        for (uint32_t corner = 0; corner < 8; ++corner) {
            const JPH::Vec3 localPoint(
                (corner & 1u) != 0u ? localMax.GetX() : localMin.GetX(), (corner & 2u) != 0u ? localMax.GetY() : localMin.GetY(),
                (corner & 4u) != 0u ? localMax.GetZ() : localMin.GetZ()
            );
            expand(transform.Multiply3x3(localPoint) + transform.GetTranslation());
        }
    }
    return estimate;
}

/** Fits the Overgrowth-style lifter and bumper to a visual envelope. */
[[nodiscard]] inline auto
    FitDualShapeToBounds(const CharacterBoundsEstimate& bounds, const Physics::DualShapeConfig& fallback = {}, const DualShapeFitOptions& options = {}) noexcept
    -> Physics::DualShapeConfig {
    if (!bounds.valid) {
        return fallback;
    }

    const JPH::Vec3 size = bounds.Size();
    if (size.GetX() <= 0.0f || size.GetY() <= 0.0f || size.GetZ() <= 0.0f) {
        return fallback;
    }

    const float maxAbsX          = std::max(std::abs(bounds.min.GetX()), std::abs(bounds.max.GetX()));
    const float maxAbsZ          = std::max(std::abs(bounds.min.GetZ()), std::abs(bounds.max.GetZ()));
    const float horizontalExtent = std::max(maxAbsX, maxAbsZ);
    const float targetTop        = std::max(bounds.max.GetY(), size.GetY()) * std::max(options.verticalPadding, 1.0f);
    if (!std::isfinite(horizontalExtent) || !std::isfinite(targetTop) || horizontalExtent <= 0.0f || targetTop <= 0.0f) {
        return fallback;
    }

    Physics::DualShapeConfig result;
    result.bumperRadiusXZ     = std::max(horizontalExtent * std::max(options.horizontalPadding, 1.0f), std::max(options.minimumBumperRadius, 0.01f));
    const float maximumLifter = result.bumperRadiusXZ * 0.95f;
    result.lifterRadius       = std::clamp(
        std::min(
            result.bumperRadiusXZ * std::clamp(options.lifterToBumperRatio, 0.1f, 0.95f), targetTop * std::clamp(options.lifterToHeightRatio, 0.05f, 0.45f)
        ),
        std::max(options.minimumBumperRadius * 0.5f, 0.01f), maximumLifter
    );

    const float cutRatio = std::clamp(result.lifterRadius / result.bumperRadiusXZ, 0.0f, 0.999f);
    const float cutScale = std::sqrt(std::max(0.0f, 1.0f - cutRatio * cutRatio));
    result.bumperRadiusY = std::max(result.bumperRadiusXZ, (targetTop - result.lifterRadius) / std::max(1.0f + cutScale, 0.001f));
    return result;
}

[[nodiscard]] inline auto EstimateDualShapeConfig(
    const ModelPrefab&              prefab,
    JPH::Vec3Arg                    modelScale = JPH::Vec3::sReplicate(1.0f),
    const Physics::DualShapeConfig& fallback   = {},
    const DualShapeFitOptions&      options    = {}
) noexcept -> Physics::DualShapeConfig {
    return FitDualShapeToBounds(EstimateCharacterBounds(prefab, modelScale), fallback, options);
}

struct EllipsoidDesc {
    float radiusXZ = 0.50f;
    float radiusY  = 0.70f;
    int   latRings = 10;
    int   lonRings = 10;
    int   segments = 36;
};

inline auto DrawWireframeEllipsoid(RenderContext& rc, const JPH::Vec3& center, const EllipsoidDesc& desc, const JPH::Vec4& color) noexcept -> void {
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

inline auto DrawWireframeSphere(
    RenderContext&   rc,
    const JPH::Vec3& center,
    float            radius,
    const JPH::Vec4& color,
    int              latRings = 10,
    int              lonRings = 10,
    int              segments = 36
) noexcept -> void {
    DrawWireframeEllipsoid(rc, center, {.radiusXZ = radius, .radiusY = radius, .latRings = latRings, .lonRings = lonRings, .segments = segments}, color);
}

struct DebugPalette {
    JPH::Vec4 colorBumper        = {0.10f, 1.00f, 0.20f, 1.0f}; // Vibrant Green
    JPH::Vec4 colorLifter        = {1.00f, 1.00f, 1.00f, 1.0f}; // Pure White
    JPH::Vec4 colorVelocityDebug = {1.00f, 0.90f, 0.10f, 1.0f}; // Yellow
};

/**
 * @brief Spawns a CharacterVirtual player entity using the exact Dual-Shape compound hull.
 */
inline auto SpawnCharacter(
    Engine&                         engine,
    const JPH::Vec3&                spawnPosition = {0.0f, 2.0f, 0.0f},
    const Physics::DualShapeConfig& config        = {},
    float                           speed         = 14.5f,
    float                           jumpForce     = 14.8f
) -> Entity {
    auto& reg = engine.GetRegistry();
    auto& pc  = engine.GetPhysicsContext();

    const Entity player = reg.Create();
    reg.Add(player, Components::PlayerTagComponent {});
    reg.Add(player, Components::NameComponent {.name = String64("Player_VirtualCharacter")});
    reg.Add(player, Components::TransformComponent {.position = spawnPosition});
    reg.Add(
        player,
        Components::WorldTransformComponent {
            .world = Math::CreateTransform(spawnPosition, JPH::Quat::sIdentity()), .previous = Math::CreateTransform(spawnPosition, JPH::Quat::sIdentity())
        }
    );
    reg.Add(player, Components::InputComponent {});
    reg.Add(player, Components::MovementComponent {.speed = speed, .jumpForce = jumpForce});

    // Create the Jolt CharacterVirtual with native Dual-Shape compound shape
    const Entity charPhys = pc.CreateCharacter(JPH::RVec3(spawnPosition), config);
    reg.Add(player, Components::PhysicsComponent {.physicsHandle = charPhys});
    reg.Add(
        player, Components::PhysicsStateComponent {
                    .currPosition = spawnPosition, .prevPosition = spawnPosition, .currRotation = JPH::Quat::sIdentity(), .prevRotation = JPH::Quat::sIdentity()
                }
    );

    // Configure third-person follow camera and strip FreeCam
    for (Entity camEnt: reg.GetEntitiesWith<Components::MainCameraTagComponent>()) {
        reg.Remove<Components::FreeCamTagComponent>(camEnt);

        reg.Add(
            camEnt, Components::TargetCameraComponent {
                        .target            = player,
                        .distance          = 5.50f,
                        .targetDistance    = 5.50f,
                        .yaw               = 90.0f,
                        .pitch             = -14.0f,
                        .targetOffset      = JPH::Vec3(0.0f, 0.80f, 0.0f),
                        .stiffness         = 24.0f,
                        .vignetteIntensity = 1.10f,
                        .vignettePower     = 1.50f,
                        .fov               = 52.0f,
                        .targetFov         = 52.0f
                    }
        );
        reg.Add(camEnt, Components::InputComponent {});
    }

    return player;
}

/**
 * @brief Renders the visual dual-shape rig 1:1 anchored to the smoothed character transform.
 */
inline auto
    RenderDebugRig(Engine& engine, Entity playerEntity, const Physics::DualShapeConfig& config = {}, const DebugPalette& palette = {}) noexcept -> void {
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();

    if (!reg.IsAlive(playerEntity)) {
        return;
    }

    const auto* trans = reg.Get<Components::TransformComponent>(playerEntity);
    if (trans == nullptr) {
        return;
    }

    const JPH::Vec3 pos               = trans->position;
    const JPH::Vec3 finalLifterCenter = pos + JPH::Vec3(0.0f, config.GetLifterOffsetY(), 0.0f);
    const JPH::Vec3 finalBumperCenter = pos + JPH::Vec3(0.0f, config.GetBumperOffsetY(), 0.0f);

    // 1. Draw Upper Bumper Oval (Green)
    DrawWireframeEllipsoid(rc, finalBumperCenter, {.radiusXZ = config.bumperRadiusXZ, .radiusY = config.bumperRadiusY}, palette.colorBumper);

    // 2. Draw Lower Lifter Sphere (White) - Touching ground at Y=0.0m
    DrawWireframeSphere(rc, finalLifterCenter, config.lifterRadius, palette.colorLifter);

    // 3. Draw Velocity Vector
    const auto* move = reg.Get<Components::MovementComponent>(playerEntity);
    if (move != nullptr) {
        const float     speed = move->speed * (move->isSprinting ? std::max(move->sprintMultiplier, 1.0f) : 1.0f);
        const JPH::Vec3 vel(move->inputX * speed, move->currentYVel, move->inputZ * speed);
        if (vel.LengthSq() > 0.01f) {
            rc.DrawLine(finalBumperCenter, finalBumperCenter + (vel * 0.25f), palette.colorVelocityDebug);
        }
    }
}

} // namespace ZHLN::Locomotion
