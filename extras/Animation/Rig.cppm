// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <array>
#include <optional>
#include <string_view>
export module ZHLN.Rig;

export namespace ZHLN::Rig {

/**
 * @brief Standardized type-safe humanoid joint indices.
 */
enum class Joint : int32_t {
    Hips = 0,
    Spine,
    Chest,
    Neck,
    Head,
    HeadEnd,

    ClavicleL,
    UpperArmL,
    ForearmL,
    HandL,
    HandEndL,

    ClavicleR,
    UpperArmR,
    ForearmR,
    HandR,
    HandEndR,

    ThighL,
    ShinL,
    FootL,
    ToeL,

    ThighR,
    ShinR,
    FootR,
    ToeR,

    Count
};

inline constexpr uint32_t JointCount = static_cast<uint32_t>(Joint::Count);

/**
 * @brief Categorized groups of joints representing structural limbs.
 */
enum class Limb : uint8_t { Root, Spine, Head, LeftArm, RightArm, LeftLeg, RightLeg };

/**
 * @brief Metadata for a single joint within the standardized rig.
 * Uses raw floats to ensure compile-time literal compatibility.
 */
struct JointDesc {
    Joint                id;
    std::string_view     name;
    int32_t              parentIndex; // -1 for root (Hips)
    Limb                 limbGroup;
    std::array<float, 3> bindPosition;
    std::array<float, 4> bindRotation;
};

/**
 * @brief Mapping for bilateral symmetry.
 */
struct SymmetryPair {
    Joint left;
    Joint right;
};

/**
 * @brief Hit capsule description for locational damage calculations.
 */
struct HitCapsule {
    Joint    a, b;
    float    r, mult;
    uint32_t zone; // 0=Head, 1=Torso, 2=Limb
};

// ============================================================================
// STANDARD RIG DEFINITION (T-Pose Base Data)
// ============================================================================

inline constexpr std::array<JointDesc, JointCount> StandardHierarchy = {
    {{Joint::Hips, "hips", -1, Limb::Root, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::Spine, "spine", static_cast<int32_t>(Joint::Hips), Limb::Spine, {0.0f, 0.14f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::Chest, "chest", static_cast<int32_t>(Joint::Spine), Limb::Spine, {0.0f, 0.18f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::Neck, "neck", static_cast<int32_t>(Joint::Chest), Limb::Spine, {0.0f, 0.20f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::Head, "head", static_cast<int32_t>(Joint::Neck), Limb::Head, {0.0f, 0.09f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::HeadEnd, "headEnd", static_cast<int32_t>(Joint::Head), Limb::Head, {0.0f, 0.23f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},

     // Left Arm
     {Joint::ClavicleL, "clavL", static_cast<int32_t>(Joint::Chest), Limb::LeftArm, {0.055f, 0.135f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::UpperArmL, "armL", static_cast<int32_t>(Joint::ClavicleL), Limb::LeftArm, {0.135f, -0.010f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::ForearmL, "foreL", static_cast<int32_t>(Joint::UpperArmL), Limb::LeftArm, {0.015f, -0.270f, 0.005f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::HandL, "handL", static_cast<int32_t>(Joint::ForearmL), Limb::LeftArm, {0.010f, -0.250f, 0.010f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::HandEndL, "handEndL", static_cast<int32_t>(Joint::HandL), Limb::LeftArm, {0.000f, -0.090f, 0.020f}, {0.0f, 0.0f, 0.0f, 1.0f}},

     // Right Arm
     {Joint::ClavicleR, "clavR", static_cast<int32_t>(Joint::Chest), Limb::RightArm, {-0.055f, 0.135f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::UpperArmR, "armR", static_cast<int32_t>(Joint::ClavicleR), Limb::RightArm, {-0.135f, -0.010f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::ForearmR, "foreR", static_cast<int32_t>(Joint::UpperArmR), Limb::RightArm, {-0.015f, -0.270f, 0.005f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::HandR, "handR", static_cast<int32_t>(Joint::ForearmR), Limb::RightArm, {-0.010f, -0.250f, 0.010f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::HandEndR, "handEndR", static_cast<int32_t>(Joint::HandR), Limb::RightArm, {0.000f, -0.090f, 0.020f}, {0.0f, 0.0f, 0.0f, 1.0f}},

     // Left Leg
     {Joint::ThighL, "thighL", static_cast<int32_t>(Joint::Hips), Limb::LeftLeg, {0.100f, -0.050f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::ShinL, "shinL", static_cast<int32_t>(Joint::ThighL), Limb::LeftLeg, {0.005f, -0.430f, 0.005f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::FootL, "footL", static_cast<int32_t>(Joint::ShinL), Limb::LeftLeg, {0.000f, -0.420f, -0.020f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::ToeL, "toeL", static_cast<int32_t>(Joint::FootL), Limb::LeftLeg, {0.000f, -0.070f, 0.160f}, {0.0f, 0.0f, 0.0f, 1.0f}},

     // Right Leg
     {Joint::ThighR, "thighR", static_cast<int32_t>(Joint::Hips), Limb::RightLeg, {-0.100f, -0.050f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::ShinR, "shinR", static_cast<int32_t>(Joint::ThighR), Limb::RightLeg, {-0.005f, -0.430f, 0.005f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::FootR, "footR", static_cast<int32_t>(Joint::ShinR), Limb::RightLeg, {0.000f, -0.420f, -0.020f}, {0.0f, 0.0f, 0.0f, 1.0f}},
     {Joint::ToeR, "toeR", static_cast<int32_t>(Joint::FootR), Limb::RightLeg, {0.000f, -0.070f, 0.160f}, {0.0f, 0.0f, 0.0f, 1.0f}}}
};

inline constexpr std::array<SymmetryPair, 9> SymmetryMap = {
    {{Joint::ClavicleL, Joint::ClavicleR},
     {Joint::UpperArmL, Joint::UpperArmR},
     {Joint::ForearmL, Joint::ForearmR},
     {Joint::HandL, Joint::HandR},
     {Joint::HandEndL, Joint::HandEndR},
     {Joint::ThighL, Joint::ThighR},
     {Joint::ShinL, Joint::ShinR},
     {Joint::FootL, Joint::FootR},
     {Joint::ToeL, Joint::ToeR}}
};

inline constexpr std::array<HitCapsule, 11> HIT_CAPSULES = {
    {{Joint::Head, Joint::HeadEnd, 0.135f, 4.5f, 0},
     {Joint::Chest, Joint::Neck, 0.215f, 1.15f, 1},
     {Joint::Hips, Joint::Chest, 0.2f, 1.0f, 1},
     {Joint::UpperArmL, Joint::ForearmL, 0.095f, 0.65f, 2},
     {Joint::ForearmL, Joint::HandL, 0.085f, 0.6f, 2},
     {Joint::UpperArmR, Joint::ForearmR, 0.095f, 0.65f, 2},
     {Joint::ForearmR, Joint::HandR, 0.085f, 0.6f, 2},
     {Joint::ThighL, Joint::ShinL, 0.125f, 0.7f, 2},
     {Joint::ShinL, Joint::FootL, 0.1f, 0.6f, 2},
     {Joint::ThighR, Joint::ShinR, 0.125f, 0.7f, 2},
     {Joint::ShinR, Joint::FootR, 0.1f, 0.6f, 2}}
};

// ============================================================================
// COMPILE-TIME & RUN-TIME UTILITIES
// ============================================================================

[[nodiscard]] constexpr int32_t GetParentIndex(Joint joint) noexcept {
    return StandardHierarchy[static_cast<size_t>(joint)].parentIndex;
}

[[nodiscard]] constexpr std::optional<Joint> GetMirroredJoint(Joint joint) noexcept {
    for (const auto& pair: SymmetryMap) {
        if (pair.left == joint)
            return pair.right;
        if (pair.right == joint)
            return pair.left;
    }
    return std::nullopt;
}

/**
 * @brief Returns the default bind pose local position as a Jolt Vec3.
 */
[[nodiscard]] inline JPH::Vec3 GetBindPosition(Joint joint) noexcept {
    const auto& pos = StandardHierarchy[static_cast<size_t>(joint)].bindPosition;
    return {pos[0], pos[1], pos[2]};
}

/**
 * @brief Returns the default bind pose local rotation as a Jolt Quat.
 */
[[nodiscard]] inline JPH::Quat GetBindRotation(Joint joint) noexcept {
    const auto& rot = StandardHierarchy[static_cast<size_t>(joint)].bindRotation;
    return {rot[0], rot[1], rot[2], rot[3]};
}

[[nodiscard]] constexpr std::optional<Joint> FindJointByName(std::string_view name) noexcept {
    for (const auto& desc: StandardHierarchy) {
        if (desc.name == name) {
            return desc.id;
        }
    }
    return std::nullopt;
}

} // namespace ZHLN::Rig
