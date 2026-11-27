// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <Animation/PrefabRig.hpp>
#include <Zahlen/COM/IDebugDraw.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <CharacterController/CharacterComponents.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string_view>

export module ZHLN.Locomotion;

export namespace ZHLN::Locomotion {

struct CharacterSetup {
    float                    targetHeight            = 1.80f;
    float                    colliderRadiusScale     = 0.24f;
    float                    colliderMaxRadius       = 0.42f;
    float                    colliderBottomMargin    = 0.0f;
    float                    colliderMinRadius       = 0.16f;
    float                    colliderMinHeight       = 0.45f;
    float                    headClearance           = 0.03f;
    float                    walkSpeed               = 3.4f;
    float                    sprintSpeed             = 6.1f;
    float                    jumpVelocity            = 5.2f;
    float                    cameraDistance          = 4.2f;
    float                    cameraHeightOffset      = 0.90f;
    float                    cameraShoulderOffset    = 0.0f;
    float                    cameraLookSensitivity   = 0.12f;
    float                    cameraPositionSmoothing = 14.0f;
    bool                     cameraAlignYawToTarget  = true;
    Physics::DualShape colliderShape {};
};

struct CharacterSpawnResult {
    Entity                   entity        = Entity::Null();
    FVec3                    scale         = FVec3::One();
    FVec3                    boundsSize    = FVec3::Zero();
    float                    height        = 0.0f;
    float                    groundOffset  = 0.0f;
    Physics::DualShape colliderShape {};
    bool                     valid         = false;
};

struct PrefabBounds {
    FVec3 min   = FVec3::Zero();
    FVec3 max   = FVec3::Zero();
    bool  valid = false;

    [[nodiscard]] auto GetSize() const noexcept -> FVec3 { return valid ? max - min : FVec3::Zero(); }
    [[nodiscard]] auto GetCenter() const noexcept -> FVec3 { return valid ? (min + max) * 0.5f : FVec3::Zero(); }
};

[[nodiscard]] auto GetNodeModelTransform(const Animation::PrefabRig& prefab, int32_t nodeIndex) noexcept -> Mat4x4 {
    if (nodeIndex < 0 || static_cast<size_t>(nodeIndex) >= prefab.nodes.size()) return Mat4x4::Identity();
    auto transform = prefab.nodes[static_cast<size_t>(nodeIndex)].localTransform;
    for (int32_t parent = prefab.nodes[static_cast<size_t>(nodeIndex)].parentIndex, guard = 0;
         parent >= 0 && static_cast<size_t>(parent) < prefab.nodes.size() && guard < 256; ++guard) {
        transform = prefab.nodes[static_cast<size_t>(parent)].localTransform * transform;
        parent    = prefab.nodes[static_cast<size_t>(parent)].parentIndex;
    }
    return transform;
}

[[nodiscard]] auto MeasurePrefabBounds(const Animation::PrefabRig& prefab, FVec3 scale = FVec3::One()) noexcept -> PrefabBounds {
    PrefabBounds bounds {
        .min   = FVec3::Splat(std::numeric_limits<float>::max()),
        .max   = FVec3::Splat(std::numeric_limits<float>::lowest()),
        .valid = false,
    };
    const auto scaleTransform = ComposeTransform(FVec3::Zero(), Quat::Identity(), scale);
    auto includePoint = [&](FVec3 point) {
        bounds.min.x = std::min(bounds.min.x, point.x);
        bounds.min.y = std::min(bounds.min.y, point.y);
        bounds.min.z = std::min(bounds.min.z, point.z);
        bounds.max.x = std::max(bounds.max.x, point.x);
        bounds.max.y = std::max(bounds.max.y, point.y);
        bounds.max.z = std::max(bounds.max.z, point.z);
        bounds.valid = true;
    };
    for (const auto& part : prefab.parts) {
        if (part.nodeIndex < 0 || static_cast<size_t>(part.nodeIndex) >= prefab.nodes.size()) continue;
        const auto transform = scaleTransform * GetNodeModelTransform(prefab, part.nodeIndex) * part.localTransform;
        for (int corner = 0; corner < 8; ++corner) {
            const auto localCorner = FVec4 {
                (corner & 1) ? part.localMax.x : part.localMin.x,
                (corner & 2) ? part.localMax.y : part.localMin.y,
                (corner & 4) ? part.localMax.z : part.localMin.z,
                1.0f,
            };
            includePoint((transform * localCorner).XYZ());
        }
    }
    return bounds;
}

[[nodiscard]] auto FitDualShapeToBounds(const PrefabBounds& bounds, const CharacterSetup& setup = {}) noexcept -> Physics::DualShape {
    if (!bounds.valid) return setup.colliderShape;
    const FVec3 size             = bounds.GetSize();
    const float horizontalExtent = std::max(size.x, size.z) * 0.5f;
    const float height           = std::max(size.y + setup.headClearance, setup.colliderMinHeight);
    float lifterRadius = std::clamp(
        std::min(horizontalExtent * 0.88f, height * setup.colliderRadiusScale), setup.colliderMinRadius, setup.colliderMaxRadius);
    lifterRadius = std::min(lifterRadius, height * 0.34f);
    const float bumperRadiusXZ = std::clamp(
        std::max(horizontalExtent * 0.96f, lifterRadius * 1.18f), lifterRadius + 0.02f, setup.colliderMaxRadius * 1.25f);
    const float ratio         = std::clamp(lifterRadius / bumperRadiusXZ, 0.0f, 0.98f);
    const float blendFactor   = 1.0f + std::sqrt(std::max(0.0f, 1.0f - (ratio * ratio)));
    const float desiredTop    = std::max(height - setup.colliderBottomMargin, lifterRadius * 2.1f);
    const float bumperRadiusY = std::max((desiredTop - lifterRadius) / blendFactor, lifterRadius * 0.65f);
    return Physics::DualShape {.lifterRadius = lifterRadius, .bumperRadiusXZ = bumperRadiusXZ, .bumperRadiusY = bumperRadiusY};
}

void DrawDualShapeDebug(COM::IDebugDraw& debug, FVec3 footPosition, const Physics::DualShape& config) {
    constexpr int   segments    = 28;
    constexpr FVec4 lifterColor = {0.15f, 0.90f, 1.00f, 1.0f};
    constexpr FVec4 bumperColor = {1.00f, 0.72f, 0.15f, 1.0f};
    auto drawEllipse = [&](FVec3 center, FVec3 axisA, FVec3 axisB, FVec4 color) {
        for (int i = 0; i < segments; ++i) {
            const float a0 = (2.0f * std::numbers::pi_v<float> * static_cast<float>(i)) / static_cast<float>(segments);
            const float a1 = (2.0f * std::numbers::pi_v<float> * static_cast<float>(i + 1)) / static_cast<float>(segments);
            debug.AddLine(center + axisA * std::cos(a0) + axisB * std::sin(a0),
                          center + axisA * std::cos(a1) + axisB * std::sin(a1), color);
        }
    };
    const FVec3 lifterCenter = footPosition + FVec3 {0.0f, config.GetLifterOffsetY(), 0.0f};
    drawEllipse(lifterCenter, {config.lifterRadius, 0.0f, 0.0f}, {0.0f, config.lifterRadius, 0.0f}, lifterColor);
    drawEllipse(lifterCenter, {0.0f, config.lifterRadius, 0.0f}, {0.0f, 0.0f, config.lifterRadius}, lifterColor);
    drawEllipse(lifterCenter, {config.lifterRadius, 0.0f, 0.0f}, {0.0f, 0.0f, config.lifterRadius}, lifterColor);
    const FVec3 bumperCenter = footPosition + FVec3 {0.0f, config.GetBumperOffsetY(), 0.0f};
    drawEllipse(bumperCenter, {config.bumperRadiusXZ, 0.0f, 0.0f}, {0.0f, config.bumperRadiusY, 0.0f}, bumperColor);
    drawEllipse(bumperCenter, {0.0f, config.bumperRadiusY, 0.0f}, {0.0f, 0.0f, config.bumperRadiusXZ}, bumperColor);
    drawEllipse(bumperCenter, {config.bumperRadiusXZ, 0.0f, 0.0f}, {0.0f, 0.0f, config.bumperRadiusXZ}, bumperColor);
}

[[nodiscard]] auto SpawnCharacter(
    COM::IEngine&         engine,
    AssetID               prefabHash,
    std::string_view      modelVirtualPath,
    FVec3                 spawnFootPosition,
    DegF                  initialYaw,
    const CharacterSetup& setup = {}) -> CharacterSpawnResult;

[[nodiscard]] auto SpawnCharacter(
    COM::IEngine&         engine,
    std::string_view      modelVirtualPath,
    FVec3                 spawnFootPosition,
    DegF                  initialYaw,
    const CharacterSetup& setup = {}) -> CharacterSpawnResult;

void DestroyCharacter(COM::IEngine& engine, Entity characterEntity);
void SwitchToFollowCamera(COM::IEngine& engine, Entity characterEntity, const CharacterSetup& setup = {});
void RecenterFollowCamera(COM::IEngine& engine, Entity characterEntity);

} // namespace ZHLN::Locomotion
