// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Physics/Jolt.hpp>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Animation/IK.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

export module ZHLN.Animator;

export namespace ZHLN::Animation {

struct ParametricCurve {
    std::vector<float> keys;

    [[nodiscard]] float Evaluate(float phase) const noexcept {
        if (keys.empty())
            return 0.0f;
        size_t n = keys.size();
        float  x = std::fmod(std::fmod(phase, 1.0f) + 1.0f, 1.0f) * n;
        size_t i = static_cast<size_t>(std::floor(x));
        float  f = x - static_cast<float>(i);
        f        = f * f * (3.0f - 2.0f * f);
        return keys[i % n] + (keys[(i + 1) % n] - keys[i % n]) * f;
    }
};

JPH::Quat AimBone(FVec3 bindDirection, JPH::QuatArg parentWorldQuat, FVec3 targetWorldDir) noexcept {
    FVec3 localDir = parentWorldQuat.Inversed() * targetWorldDir.Normalized();
    return JPH::Quat::sFromTo(bindDirection, localDir);
}

void SolveTwoBoneIK(
    FVec3 jointWorldPos,
    FVec3 parentBindPos,
    FVec3 childBindPos,
    JPH::QuatArg grandparentWorldQuat,
    FVec3 targetWorld,
    FVec3 poleWorld,
    JPH::Quat&   outParentQuat,
    JPH::Quat&   outChildQuat
) noexcept {
    auto ikOutput = ZHLN::IK::SolveTwoBoneIK(
        {.upperPosition  = jointWorldPos,
         .targetPosition = targetWorld,
         .poleVector     = poleWorld,
         .upperLength    = parentBindPos.Length(),
         .lowerLength    = childBindPos.Length()}
    );

    if (!ikOutput.valid)
        return;

    outParentQuat             = AimBone(parentBindPos, grandparentWorldQuat, ikOutput.upperDirection);
    JPH::Quat parentWorldQuat = grandparentWorldQuat * outParentQuat;
    outChildQuat              = AimBone(childBindPos, parentWorldQuat, ikOutput.lowerDirection);
}

} // namespace ZHLN::Animation
