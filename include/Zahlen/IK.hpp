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
};

struct TwoBoneIKSolverOutput {
    JPH::Vec3 midPosition;
    JPH::Vec3 upperDirection;
    JPH::Vec3 lowerDirection;
    bool      valid = false;
};

/**
 * @brief Evaluates an analytic 2-Bone IK chain (e.g. Upper Arm -> Forearm -> Hand).
 * Uses the Law of Cosines to solve middle joint placement in O(1) time.
 */
inline TwoBoneIKSolverOutput SolveTwoBoneIK(const TwoBoneIKSolverInput& input) noexcept {
    TwoBoneIKSolverOutput out;

    JPH::Vec3 axis = input.targetPosition - input.upperPosition;
    float     d    = axis.Length();

    if (d < 1e-5f) {
        return out;
    }

    float l1 = input.upperLength;
    float l2 = input.lowerLength;

    // Enforce triangle inequality bounds to prevent imaginary numbers in acos
    float maxD = (l1 + l2) * 0.9999f;
    float minD = std::abs(l1 - l2) + 1e-4f;
    d          = std::clamp(d, minD, maxD);

    JPH::Vec3 normAxis = axis / axis.Length();

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
    JPH::Vec3 lowerVec = input.targetPosition - out.midPosition;

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
