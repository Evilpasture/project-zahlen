// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/IK.hpp>
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

inline JPH::Quat AimBone(JPH::Vec3Arg bindDirection, JPH::QuatArg parentWorldQuat, JPH::Vec3Arg targetWorldDir) noexcept {
    JPH::Vec3 localDir = parentWorldQuat.Inversed() * targetWorldDir.Normalized();
    return JPH::Quat::sFromTo(bindDirection, localDir);
}

inline void SolveTwoBoneIK(
    JPH::Vec3Arg jointWorldPos,
    JPH::Vec3Arg parentBindPos,
    JPH::Vec3Arg childBindPos,
    JPH::QuatArg grandparentWorldQuat,
    JPH::Vec3Arg targetWorld,
    JPH::Vec3Arg poleWorld,
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
