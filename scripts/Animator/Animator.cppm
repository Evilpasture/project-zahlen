module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <algorithm>
#include <cmath>
#include <vector>
export module ZHLN.Animator;

export namespace ZHLN::Animation {

/**
 * @brief Evaluates Hermite spline curves based on keyframe data.
 */
struct ParametricCurve {
    std::vector<float> keys;

    [[nodiscard]] float Evaluate(float phase) const noexcept {
        if (keys.empty())
            return 0.0f;

        size_t n = keys.size();
        float  x = std::fmod(std::fmod(phase, 1.0f) + 1.0f, 1.0f) * n;
        size_t i = static_cast<size_t>(std::floor(x));
        float  f = x - static_cast<float>(i);

        // Smooth Hermite interpolation (S-curve)
        f = f * f * (3.0f - 2.0f * f);

        return keys[i % n] + (keys[(i + 1) % n] - keys[i % n]) * f;
    }
};

/**
 * @brief Orients a local bone quaternion to point toward a world direction.
 */
inline JPH::Quat AimBone(JPH::Vec3Arg bindDirection, JPH::QuatArg parentWorldQuat, JPH::Vec3Arg targetWorldDir) noexcept {
    JPH::Vec3 localDir = parentWorldQuat.Inversed() * targetWorldDir.Normalized();
    return JPH::Quat::sFromTo(bindDirection, localDir);
}

/**
 * @brief Analytical 2-Joint Inverse Kinematics (e.g., Shoulder -> Elbow -> Wrist).
 */
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
    float l1 = parentBindPos.Length();
    float l2 = childBindPos.Length();

    JPH::Vec3 axis = targetWorld - jointWorldPos;
    float     d    = axis.Length();
    if (d < 1e-4f)
        return;

    float maxD = (l1 + l2) * 0.998f;
    if (d > maxD)
        d = maxD;
    float minD = std::abs(l1 - l2) + 0.02f;
    if (d < minD)
        d = minD;

    axis = axis.Normalized();

    float cosA  = std::clamp((l1 * l1 + d * d - l2 * l2) / (2.0f * l1 * d), -1.0f, 1.0f);
    float angle = std::acos(cosA);

    JPH::Vec3 perp = poleWorld - jointWorldPos;
    perp           = perp - axis * perp.Dot(axis);
    if (perp.LengthSq() < 1e-6f) {
        perp = JPH::Vec3(-axis.GetY(), axis.GetX(), 0.0f);
    }
    perp = perp.Normalized();

    JPH::Vec3 elbowPos = jointWorldPos + axis * (std::cos(angle) * l1) + perp * (std::sin(angle) * l1);
    JPH::Vec3 endPos   = jointWorldPos + axis * d;

    // Solve upper bone
    JPH::Vec3 d1  = (elbowPos - jointWorldPos).Normalized();
    outParentQuat = AimBone(parentBindPos, grandparentWorldQuat, d1);

    // Solve lower bone
    JPH::Quat parentWorldQuat = grandparentWorldQuat * outParentQuat;
    JPH::Vec3 d2              = (endPos - elbowPos).Normalized();
    outChildQuat              = AimBone(childBindPos, parentWorldQuat, d2);
}

} // namespace ZHLN::Animation
