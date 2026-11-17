// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Animation/IK.hpp
//
// Analytic two-bone IK: a declarative animation modifier backed by
// AnimationContext. TwoBoneIKComponent is pure trivially-copyable state;
// TwoBoneIKSystem forwards chains into AnimationContext prior to Step().
#pragma once

#include <Zahlen/Animation/IKControls.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/ECS/SystemAccess.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ZHLN {
class Engine;

namespace Animation {
class AnimationContext;
}

namespace IK {

struct ZHLN_API TwoBoneIKSystem {
    static void Update(
        ECS::Query<const Components::AnimatorComponent, const TwoBoneIKComponent, const Components::TransformComponent> query,
        ECS::ResMut<Animation::AnimationContext>                                                                        anim
    );
};

// Core owns evaluation and cleanup. Describe is a marker; Install is a
// compatibility no-op and never appends a second evaluator or application module.
[[nodiscard]] ZHLN_API auto Describe() noexcept -> Module;
ZHLN_API void               Install(Engine& engine);

struct TwoBoneIKSolverInput {
    FVec3 upperPosition;
    FVec3 targetPosition;
    FVec3 poleVector; // Direction hint for middle joint (elbow/knee)
    float upperLength;
    float lowerLength;
    float maxExtension = 0.9999f; // < 1 keeps the middle joint from locking straight.
};

struct TwoBoneIKSolverOutput {
    FVec3 midPosition;
    FVec3 endPosition;
    FVec3 upperDirection;
    FVec3 lowerDirection;
    float requestedDistance = 0.0f;
    float solvedDistance    = 0.0f;
    bool  reachClamped      = false;
    bool  valid             = false;
};

/**
 * @brief Evaluates an analytic 2-Bone IK chain (e.g. Upper Arm -> Forearm -> Hand).
 * Uses the Law of Cosines to solve middle joint placement in O(1) time.
 */
inline TwoBoneIKSolverOutput SolveTwoBoneIK(const TwoBoneIKSolverInput& input) noexcept {
    TwoBoneIKSolverOutput out;

    const FVec3 axis              = input.targetPosition - input.upperPosition;
    const float requestedDistance = axis.Length();

    const float l1 = input.upperLength;
    const float l2 = input.lowerLength;
    if (requestedDistance < 1.0e-5f || l1 <= 1.0e-5f || l2 <= 1.0e-5f) {
        return out;
    }

    // Enforce triangle inequality and a configurable extension limit. Crucially,
    // solve and publish a constrained end position instead of aiming the lower
    // segment at the original unreachable target (which visually stretches it).
    const float minD      = std::abs(l1 - l2) + 1.0e-4f;
    const float extension = std::clamp(input.maxExtension, 0.50f, 0.9999f);
    const float maxD      = std::max(minD, (l1 + l2) * extension);
    const float d         = std::clamp(requestedDistance, minD, maxD);
    const FVec3 normAxis  = axis / requestedDistance;
    out.endPosition       = input.upperPosition + normAxis * d;
    out.requestedDistance = requestedDistance;
    out.solvedDistance    = d;
    out.reachClamped      = std::abs(requestedDistance - d) > 1.0e-5f;

    // Law of Cosines for upper joint angle
    float cosUpper   = std::clamp((l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), -1.0f, 1.0f);
    float angleUpper = std::acos(cosUpper);

    // Project pole vector onto plane orthogonal to target direction axis
    FVec3 perp = input.poleVector - normAxis * Dot(input.poleVector, normAxis);
    if (perp.LengthSquared() < 1e-6f) {
        perp = (std::abs(normAxis.x) > std::abs(normAxis.y) ? FVec3 {normAxis.z, 0.0f, -normAxis.x} : FVec3 {0.0f, normAxis.z, -normAxis.y}).Normalized();
    } else {
        perp = perp.Normalized();
    }

    // Compute solved middle joint position
    out.midPosition = input.upperPosition + (normAxis * std::cos(angleUpper) * l1) + (perp * std::sin(angleUpper) * l1);

    FVec3 upperVec = out.midPosition - input.upperPosition;
    FVec3 lowerVec = out.endPosition - out.midPosition;

    if (upperVec.LengthSquared() > 1e-6f && lowerVec.LengthSquared() > 1e-6f) {
        out.upperDirection = upperVec.Normalized();
        out.lowerDirection = lowerVec.Normalized();
        out.valid          = true;
    }

    return out;
}

/**
 * @brief Orients a node matrix so its local forward vector aligns with worldTargetDir.
 */
inline auto AlignNodeToDirection(const Mat4x4& currentWorldMat, FVec3 localBindDir, FVec3 worldTargetDir) noexcept -> Mat4x4 {
    const auto currentWorldDir = (currentWorldMat * FVec4 {localBindDir.x, localBindDir.y, localBindDir.z, 0.0f}).XYZ().Normalized();
    if (Dot(currentWorldDir, worldTargetDir) > 0.9999f) {
        return currentWorldMat;
    }
    const auto lengthProduct = std::sqrt(currentWorldDir.LengthSquared() * worldTargetDir.LengthSquared());
    const auto w             = lengthProduct + Dot(currentWorldDir, worldTargetDir);
    auto       axis          = Cross(currentWorldDir, worldTargetDir);
    auto       adjust        = Quat {axis.x, axis.y, axis.z, w};
    if (w == 0.0f) {
        if (lengthProduct == 0.0f)
            return currentWorldMat;
        axis   = (std::abs(currentWorldDir.x) > std::abs(currentWorldDir.y) ? FVec3 {currentWorldDir.z, 0.0f, -currentWorldDir.x} :
                                                                              FVec3 {0.0f, currentWorldDir.z, -currentWorldDir.y})
                     .Normalized();
        adjust = {axis.x, axis.y, axis.z, 0.0f};
    } else {
        adjust = adjust.Normalized();
    }
    const auto parts = DecomposeTransform(currentWorldMat);
    return ComposeTransform(parts.translation, (adjust * parts.rotation).Normalized(), FVec3::One());
}

} // namespace IK
} // namespace ZHLN
