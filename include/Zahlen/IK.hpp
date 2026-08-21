// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <algorithm>
#include <cmath>

namespace ZHLN::IK {

struct TwoBoneIKSolverInput {
    JPH::Vec3 upperPosition;
    JPH::Vec3 targetPosition;
    JPH::Vec3 poleVector; // Direction hint for middle joint (elbow/knee)
    float     upperLength;
    float     lowerLength;
    float     maxExtension = 0.9999f; // < 1 keeps the middle joint from locking straight.
};

struct TwoBoneIKSolverOutput {
    JPH::Vec3 midPosition;
    JPH::Vec3 endPosition;
    JPH::Vec3 upperDirection;
    JPH::Vec3 lowerDirection;
    float     requestedDistance = 0.0f;
    float     solvedDistance    = 0.0f;
    bool      reachClamped      = false;
    bool      valid             = false;
};

/**
 * @brief Evaluates an analytic 2-Bone IK chain (e.g. Upper Arm -> Forearm -> Hand).
 * Uses the Law of Cosines to solve middle joint placement in O(1) time.
 */
inline TwoBoneIKSolverOutput SolveTwoBoneIK(const TwoBoneIKSolverInput& input) noexcept {
    TwoBoneIKSolverOutput out;

    const JPH::Vec3 axis              = input.targetPosition - input.upperPosition;
    const float     requestedDistance = axis.Length();

    const float l1 = input.upperLength;
    const float l2 = input.lowerLength;
    if (requestedDistance < 1.0e-5f || l1 <= 1.0e-5f || l2 <= 1.0e-5f) {
        return out;
    }

    // Enforce triangle inequality and a configurable extension limit. Crucially,
    // solve and publish a constrained end position instead of aiming the lower
    // segment at the original unreachable target (which visually stretches it).
    const float     minD      = std::abs(l1 - l2) + 1.0e-4f;
    const float     extension = std::clamp(input.maxExtension, 0.50f, 0.9999f);
    const float     maxD      = std::max(minD, (l1 + l2) * extension);
    const float     d         = std::clamp(requestedDistance, minD, maxD);
    const JPH::Vec3 normAxis  = axis / requestedDistance;
    out.endPosition           = input.upperPosition + normAxis * d;
    out.requestedDistance     = requestedDistance;
    out.solvedDistance        = d;
    out.reachClamped          = std::abs(requestedDistance - d) > 1.0e-5f;

    // Law of Cosines for upper joint angle
    float cosUpper   = std::clamp((l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), -1.0f, 1.0f);
    float angleUpper = std::acos(cosUpper);

    // Project pole vector onto plane orthogonal to target direction axis
    JPH::Vec3 perp = input.poleVector - normAxis * input.poleVector.Dot(normAxis);
    if (perp.LengthSq() < 1e-6f) {
        perp = normAxis.GetNormalizedPerpendicular();
    } else {
        perp = perp.Normalized();
    }

    // Compute solved middle joint position
    out.midPosition = input.upperPosition + (normAxis * std::cos(angleUpper) * l1) + (perp * std::sin(angleUpper) * l1);

    JPH::Vec3 upperVec = out.midPosition - input.upperPosition;
    JPH::Vec3 lowerVec = out.endPosition - out.midPosition;

    if (upperVec.LengthSq() > 1e-6f && lowerVec.LengthSq() > 1e-6f) {
        out.upperDirection = upperVec.Normalized();
        out.lowerDirection = lowerVec.Normalized();
        out.valid          = true;
    }

    return out;
}

/**
 * @brief Orients a node matrix so its local forward vector aligns with worldTargetDir.
 */
inline JPH::Mat44 AlignNodeToDirection(const JPH::Mat44& currentWorldMat, JPH::Vec3Arg localBindDir, JPH::Vec3Arg worldTargetDir) noexcept {
    JPH::Vec3 currentWorldDir = currentWorldMat.Multiply3x3(localBindDir).Normalized();

    if (currentWorldDir.Dot(worldTargetDir) > 0.9999f) {
        return currentWorldMat;
    }

    JPH::Quat rotationAdjust = JPH::Quat::sFromTo(currentWorldDir, worldTargetDir);

    JPH::Vec3 translation = currentWorldMat.GetTranslation();
    JPH::Quat currentRot  = currentWorldMat.GetQuaternion().Normalized();

    JPH::Quat newRot = (rotationAdjust * currentRot).Normalized();
    return JPH::Mat44::sRotationTranslation(newRot, translation);
}

} // namespace ZHLN::IK
