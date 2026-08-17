// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProceduralAnimationSystem.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/ProceduralAnimation.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <numbers>
#include <span>
#include <string_view>
#include <utility>

namespace ZHLN {
namespace {

inline constexpr std::array<std::string_view, kCoreBoneCount> kCoreBoneLabels = {
    "Root",     "Hips",  "Spine",  "SupSpine", "Chest", "Neck", "Head",   "UpperArmL", "ForearmL", "HandL", "UpperArmR",
    "ForearmR", "HandR", "ThighL", "ShinL",    "FootL", "ToeL", "ThighR", "ShinR",     "FootR",    "ToeR",
};

struct CanonicalName {
    std::array<char, 96> data {};
    size_t               size = 0;

    [[nodiscard]] std::string_view View() const noexcept {
        return {data.data(), size};
    }
};

[[nodiscard]] CanonicalName Canonicalize(std::string_view name) noexcept {
    CanonicalName result;
    for (char value: name) {
        if (result.size == result.data.size()) {
            break;
        }
        char c = value;
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            result.data[result.size++] = c;
        }
    }
    return result;
}

[[nodiscard]] std::string_view StripKnownRigPrefix(std::string_view value) noexcept {
    if (value.size() > 3 && (value.starts_with("def") || value.starts_with("org"))) {
        value.remove_prefix(3);
    }
    return value;
}

[[nodiscard]] bool MatchesAny(std::string_view value, std::span<const std::string_view> aliases, bool allowSuffix) noexcept {
    for (std::string_view alias: aliases) {
        if (value == alias || (allowSuffix && value.size() > alias.size() && value.ends_with(alias))) {
            return true;
        }
    }
    return false;
}

template <size_t N>
[[nodiscard]] bool MatchesAny(std::string_view value, const std::array<std::string_view, N>& aliases, bool allowSuffix) noexcept {
    return MatchesAny(value, std::span<const std::string_view>(aliases), allowSuffix);
}

[[nodiscard]] bool MatchesBone(std::string_view name, CharacterBone bone, bool allowSuffix) noexcept {
    using namespace std::literals;
    switch (bone) {
        case CharacterBone::Root: {
            constexpr std::array aliases = {"root"sv, "armature"sv, "rig"sv, "characterrig"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::Hips: {
            constexpr std::array aliases = {"hips"sv, "hip"sv, "pelvis"sv, "mixamorighips"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::Spine: {
            constexpr std::array aliases = {"spine"sv, "spine0"sv, "spine01"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::SupSpine: {
            constexpr std::array aliases = {"supspine"sv, "upperspine"sv, "spine1"sv, "spine02"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::Chest: {
            constexpr std::array aliases = {"chest"sv, "upperchest"sv, "spine2"sv, "spine03"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::Neck: {
            constexpr std::array aliases = {"neck"sv, "neck1"sv, "mixamorigneck"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::Head: {
            constexpr std::array aliases = {"head"sv, "mixamorighead"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::UpperArmL: {
            constexpr std::array aliases = {"upperarml"sv, "leftupperarm"sv, "arml"sv, "mixamorigleftarm"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::ForearmL: {
            constexpr std::array aliases = {"forearml"sv, "leftforearm"sv, "lowerarml"sv, "mixamorigleftforearm"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::HandL: {
            constexpr std::array aliases = {"handl"sv, "lefthand"sv, "mixamoriglefthand"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::UpperArmR: {
            constexpr std::array aliases = {"upperarmr"sv, "rightupperarm"sv, "armr"sv, "mixamorigrightarm"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::ForearmR: {
            constexpr std::array aliases = {"forearmr"sv, "rightforearm"sv, "lowerarmr"sv, "mixamorigrightforearm"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::HandR: {
            constexpr std::array aliases = {"handr"sv, "righthand"sv, "mixamorigrighthand"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::ThighL: {
            constexpr std::array aliases = {"thighl"sv, "leftupleg"sv, "upperlegl"sv, "mixamorigleftupleg"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::ShinL: {
            constexpr std::array aliases = {"shinl"sv, "leftleg"sv, "lowerlegl"sv, "calfl"sv, "mixamorigleftleg"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::FootL: {
            constexpr std::array aliases = {"footl"sv, "leftfoot"sv, "anklel"sv, "mixamorigleftfoot"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::ToeL: {
            constexpr std::array aliases = {"toel"sv, "lefttoe"sv, "lefttoebase"sv, "mixamoriglefttoebase"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::ThighR: {
            constexpr std::array aliases = {"thighr"sv, "rightupleg"sv, "upperlegr"sv, "mixamorigrightupleg"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::ShinR: {
            constexpr std::array aliases = {"shinr"sv, "rightleg"sv, "lowerlegr"sv, "calfr"sv, "mixamorigrightleg"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::FootR: {
            constexpr std::array aliases = {"footr"sv, "rightfoot"sv, "ankler"sv, "mixamorigrightfoot"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        case CharacterBone::ToeR: {
            constexpr std::array aliases = {"toer"sv, "righttoe"sv, "righttoebase"sv, "mixamorigrighttoebase"sv};
            return MatchesAny(name, aliases, allowSuffix);
        }
        default:
            return false;
    }
}

[[nodiscard]] bool IsHairNode(std::string_view name) noexcept {
    return name.find("hair") != std::string_view::npos || name.find("braid") != std::string_view::npos || name.find("ponytail") != std::string_view::npos ||
           name.find("strand") != std::string_view::npos;
}

struct HairAddress {
    int32_t strand = -1;
    int32_t link   = -1;

    [[nodiscard]] bool IsValid() const noexcept {
        return strand >= 0 && strand < static_cast<int32_t>(HairStrandsComponent::kStrandCount) && link >= 0 &&
               link < static_cast<int32_t>(HairStrandsComponent::kLinksPerStrand);
    }
};

[[nodiscard]] HairAddress ParseHairAddress(std::string_view name) noexcept {
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };

    size_t hairEnd = std::string_view::npos;
    for (size_t i = 0; i + 4 <= name.size(); ++i) {
        if (lower(name[i]) == 'h' && lower(name[i + 1]) == 'a' && lower(name[i + 2]) == 'i' && lower(name[i + 3]) == 'r') {
            hairEnd = i + 4;
            break;
        }
    }
    if (hairEnd == std::string_view::npos) {
        return {};
    }

    std::array<uint32_t, 2> numbers {};
    size_t                  numberCount = 0;
    for (size_t i = hairEnd; i < name.size() && numberCount < numbers.size();) {
        if (name[i] < '0' || name[i] > '9') {
            ++i;
            continue;
        }
        uint32_t value = 0;
        while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
            value = value * 10u + static_cast<uint32_t>(name[i] - '0');
            ++i;
        }
        numbers[numberCount++] = value;
    }

    if (numberCount != 2 || numbers[0] == 0 || numbers[1] == 0) {
        return {};
    }
    HairAddress result {.strand = static_cast<int32_t>(numbers[0] - 1), .link = static_cast<int32_t>(numbers[1] - 1)};
    return result.IsValid() ? result : HairAddress {};
}

void FillIdentity(std::span<JPH::Mat44> matrices) noexcept {
    std::ranges::fill(matrices, JPH::Mat44::sIdentity());
}

void ResetRigBoneMap(RigBoneMap& map) noexcept {
    map.nodeIndices.fill(-1);
    FillIdentity(map.inverseBindMatrices);
    map.parentIndices.fill(-1);
    FillIdentity(map.bindLocalTransforms);
    FillIdentity(map.localTransforms);
    FillIdentity(map.modelTransforms);
    map.poseTranslations.fill(JPH::Vec3::sZero());
    map.poseTranslationVelocities.fill(JPH::Vec3::sZero());
    map.poseRotations.fill(JPH::Quat::sIdentity());
    map.poseAngularVelocities.fill(JPH::Vec3::sZero());
    map.poseScales.fill(JPH::Vec3::sReplicate(1.0f));
    map.poseScaleVelocities.fill(JPH::Vec3::sZero());
    map.springPoseTrack       = -1;
    map.springPoseInitialized = false;
    map.sourcePrefab          = nullptr;
    map.nodeCount             = 0;
    map.jointOffset           = 0;
    map.jointCount            = 0;
    map.skeletonIndex         = -1;
    map.poseVersion           = 0;
    map.initialized           = false;
    map.poseValid             = false;
}

[[nodiscard]] JPH::Vec3 ExtractScale(const JPH::Mat44& matrix) noexcept {
    return JPH::Vec3(matrix.GetColumn3(0).Length(), matrix.GetColumn3(1).Length(), matrix.GetColumn3(2).Length());
}

[[nodiscard]] JPH::Quat ExtractRotation(const JPH::Mat44& matrix) noexcept {
    const JPH::Vec3 scale = ExtractScale(matrix);
    const JPH::Vec3 x     = scale.GetX() > 1.0e-6f ? matrix.GetColumn3(0) / scale.GetX() : JPH::Vec3::sAxisX();
    const JPH::Vec3 y     = scale.GetY() > 1.0e-6f ? matrix.GetColumn3(1) / scale.GetY() : JPH::Vec3::sAxisY();
    const JPH::Vec3 z     = scale.GetZ() > 1.0e-6f ? matrix.GetColumn3(2) / scale.GetZ() : JPH::Vec3::sAxisZ();
    return JPH::Mat44(JPH::Vec4(x, 0.0f), JPH::Vec4(y, 0.0f), JPH::Vec4(z, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)).GetQuaternion().Normalized();
}

[[nodiscard]] float SmootherStep(float value) noexcept {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

void SpringVector(JPH::Vec3& value, JPH::Vec3& velocity, JPH::Vec3Arg target, float dt, float stiffness, float dampingFactor) noexcept {
    const float     safeDt             = std::clamp(dt, 0.0f, 0.05f);
    const float     safeStiffness      = std::max(stiffness, 0.0f);
    const float     dampingCoefficient = 2.0f * std::sqrt(safeStiffness) * std::max(dampingFactor, 0.0f);
    const float     f                  = 1.0f + safeDt * dampingCoefficient;
    const float     hoo                = safeDt * safeStiffness;
    const float     hhoo               = safeDt * hoo;
    const float     invDet             = 1.0f / (f + hhoo);
    const JPH::Vec3 oldValue           = value;
    value                              = (value * f + velocity * safeDt + JPH::Vec3(target) * hhoo) * invDet;
    velocity                           = (velocity + (JPH::Vec3(target) - oldValue) * hoo) * invDet;
}

void SpringRotation(JPH::Quat& value, JPH::Vec3& angularVelocity, JPH::QuatArg target, float dt, float stiffness, float dampingFactor) noexcept {
    const float safeDt             = std::clamp(dt, 0.0f, 0.05f);
    const float safeStiffness      = std::max(stiffness, 0.0f);
    const float dampingCoefficient = 2.0f * std::sqrt(safeStiffness) * std::max(dampingFactor, 0.0f);
    JPH::Quat   error              = (target * value.Inversed()).Normalized();
    JPH::Vec3   axis;
    float       angle = 0.0f;
    error.GetAxisAngle(axis, angle);
    const JPH::Vec3 errorVector = axis * angle;
    const float     denominator = 1.0f + dampingCoefficient * safeDt + safeStiffness * safeDt * safeDt;
    angularVelocity             = (angularVelocity + errorVector * (safeStiffness * safeDt)) / denominator;

    const JPH::Vec3 rotationStep = angularVelocity * safeDt;
    const float     stepAngle    = rotationStep.Length();
    if (stepAngle > 1.0e-7f) {
        value = (JPH::Quat::sRotation(rotationStep / stepAngle, std::min(stepAngle, std::numbers::pi_v<float>)) * value).Normalized();
    }
}

[[nodiscard]] JPH::Vec3 BicubicVector(JPH::Vec3Arg p0, JPH::Vec3Arg p1, JPH::Vec3Arg p2, JPH::Vec3Arg p3, float phase, float tension) noexcept {
    const float     tangentScale = 0.5f * (1.0f - std::clamp(tension, -1.0f, 1.0f));
    const JPH::Vec3 tangent1     = (p2 - p0) * tangentScale;
    const JPH::Vec3 tangent2     = (p3 - p1) * tangentScale;
    const float     t2           = phase * phase;
    const float     t3           = t2 * phase;
    const float     h00          = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float     h10          = t3 - 2.0f * t2 + phase;
    const float     h01          = -2.0f * t3 + 3.0f * t2;
    const float     h11          = t3 - t2;
    return p1 * h00 + tangent1 * h10 + p2 * h01 + tangent2 * h11;
}

[[nodiscard]] float QuaternionDot(JPH::QuatArg a, JPH::QuatArg b) noexcept {
    return a.GetX() * b.GetX() + a.GetY() * b.GetY() + a.GetZ() * b.GetZ() + a.GetW() * b.GetW();
}

[[nodiscard]] JPH::Quat AlignQuaternionHemisphere(JPH::QuatArg value, JPH::QuatArg reference) noexcept {
    return QuaternionDot(value, reference) < 0.0f ? JPH::Quat(-value.GetX(), -value.GetY(), -value.GetZ(), -value.GetW()) : JPH::Quat(value);
}

[[nodiscard]] JPH::Quat BicubicRotation(JPH::QuatArg q0, JPH::QuatArg q1, JPH::QuatArg q2, JPH::QuatArg q3, float phase, float tension) noexcept {
    const JPH::Quat p1  = q1.Normalized();
    const JPH::Quat p0  = AlignQuaternionHemisphere(q0.Normalized(), p1);
    const JPH::Quat p2  = AlignQuaternionHemisphere(q2.Normalized(), p1);
    const JPH::Quat p3  = AlignQuaternionHemisphere(q3.Normalized(), p1);
    const JPH::Vec3 xyz = BicubicVector(p0.GetXYZ(), p1.GetXYZ(), p2.GetXYZ(), p3.GetXYZ(), phase, tension);

    const float tangentScale = 0.5f * (1.0f - std::clamp(tension, -1.0f, 1.0f));
    const float tangent1     = (p2.GetW() - p0.GetW()) * tangentScale;
    const float tangent2     = (p3.GetW() - p1.GetW()) * tangentScale;
    const float t2           = phase * phase;
    const float t3           = t2 * phase;
    const float w            = p1.GetW() * (2.0f * t3 - 3.0f * t2 + 1.0f) + tangent1 * (t3 - 2.0f * t2 + phase) + p2.GetW() * (-2.0f * t3 + 3.0f * t2) +
                               tangent2 * (t3 - t2);
    return JPH::Quat(xyz.GetX(), xyz.GetY(), xyz.GetZ(), w).Normalized();
}

void SamplePoseChannel(
    const AnimationChannel& channel,
    float                   time,
    PoseInterpolationMode   mode,
    float                   bicubicTension,
    JPH::Vec3&              translation,
    JPH::Quat&              rotation,
    JPH::Vec3&              scale
) noexcept {
    if (channel.keyTimes.empty() || channel.keyValues.empty()) {
        return;
    }

    const auto   upper    = std::ranges::upper_bound(channel.keyTimes, time);
    const size_t index1   = upper == channel.keyTimes.end() ? channel.keyTimes.size() - 1 : static_cast<size_t>(std::distance(channel.keyTimes.begin(), upper));
    const size_t index0   = index1 > 0 ? index1 - 1 : 0;
    const float  time0    = channel.keyTimes[index0];
    const float  time1    = channel.keyTimes[index1];
    float        rawPhase = time1 > time0 ? std::clamp((time - time0) / (time1 - time0), 0.0f, 1.0f) : 0.0f;
    const bool   useBicubic = mode == PoseInterpolationMode::Bicubic && channel.interpolation != InterpolationType::Step;
    const float  phase      = channel.interpolation == InterpolationType::Step ? 0.0f : (useBicubic ? rawPhase : SmootherStep(rawPhase));

    const size_t components   = channel.path == AnimationPathType::Rotation ? 4u : 3u;
    const size_t valuesPerKey = channel.keyValues.size() / channel.keyTimes.size();
    if (valuesPerKey < components) {
        return;
    }
    const size_t valueOffset = channel.interpolation == InterpolationType::CubicSpline && valuesPerKey >= components * 3 ? components : 0u;
    const size_t indexPrev   = index0 > 0 ? index0 - 1 : index0;
    const size_t indexNext   = std::min(index1 + 1, channel.keyTimes.size() - 1);
    const auto   valueAt     = [&](size_t key) { return channel.keyValues.data() + key * valuesPerKey + valueOffset; };
    const float* valuePrev   = valueAt(indexPrev);
    const float* value0      = valueAt(index0);
    const float* value1      = valueAt(index1);
    const float* valueNext   = valueAt(indexNext);

    if (channel.path == AnimationPathType::Translation || channel.path == AnimationPathType::Scale) {
        const JPH::Vec3 p0(valuePrev[0], valuePrev[1], valuePrev[2]);
        const JPH::Vec3 p1(value0[0], value0[1], value0[2]);
        const JPH::Vec3 p2(value1[0], value1[1], value1[2]);
        const JPH::Vec3 p3(valueNext[0], valueNext[1], valueNext[2]);
        const JPH::Vec3 result = useBicubic ? BicubicVector(p0, p1, p2, p3, phase, bicubicTension) : p1 + (p2 - p1) * phase;
        if (channel.path == AnimationPathType::Translation) {
            translation = result;
        } else {
            scale = result;
        }
    } else if (channel.path == AnimationPathType::Rotation) {
        const JPH::Quat q0(valuePrev[0], valuePrev[1], valuePrev[2], valuePrev[3]);
        const JPH::Quat q1(value0[0], value0[1], value0[2], value0[3]);
        const JPH::Quat q2(value1[0], value1[1], value1[2], value1[3]);
        const JPH::Quat q3(valueNext[0], valueNext[1], valueNext[2], valueNext[3]);
        rotation = useBicubic ? BicubicRotation(q0, q1, q2, q3, phase, bicubicTension) : q1.Normalized().SLERP(q2.Normalized(), phase).Normalized();
    }
}

void ApplyAuthoredPose(const Components::AnimatorComponent* animator, const ProceduralAnimationConfigComponent* config, RigBoneMap& map, float dt) noexcept {
    std::array<JPH::Vec3, kMaxRigNodes> targetTranslations {};
    std::array<JPH::Quat, kMaxRigNodes> targetRotations {};
    std::array<JPH::Vec3, kMaxRigNodes> targetScales {};

    const PoseInterpolationMode mode                = config != nullptr ? config->poseInterpolation : PoseInterpolationMode::SpringDamper;
    const float                 springStiffness     = config != nullptr ? config->springStiffness : map.poseSpringStiffness;
    const float                 springDampingFactor = config != nullptr ? config->springDampingFactor : map.poseSpringDampingFactor;
    const float                 bicubicTension      = config != nullptr ? config->bicubicTension : 0.0f;

    for (uint32_t node = 0; node < map.nodeCount; ++node) {
        targetTranslations[node] = map.bindLocalTransforms[node].GetTranslation();
        targetRotations[node]    = ExtractRotation(map.bindLocalTransforms[node]);
        targetScales[node]       = ExtractScale(map.bindLocalTransforms[node]);
    }

    int32_t activeTrack = -1;
    if (animator != nullptr && animator->prefab != nullptr && animator->currentTrackIdx >= 0 &&
        animator->currentTrackIdx < static_cast<int32_t>(animator->prefab->animations.size())) {
        activeTrack               = animator->currentTrackIdx;
        const AnimationClip& clip = animator->prefab->animations[static_cast<size_t>(activeTrack)];
        for (const AnimationChannel& channel: clip.channels) {
            if (channel.targetNodeIndex < 0 || channel.targetNodeIndex >= static_cast<int32_t>(map.nodeCount) || channel.path == AnimationPathType::Weights) {
                continue;
            }
            const size_t node = static_cast<size_t>(channel.targetNodeIndex);
            SamplePoseChannel(channel, animator->currentTrackTime, mode, bicubicTension, targetTranslations[node], targetRotations[node], targetScales[node]);
        }
    }

    // Non-semantic controls (fingers, face, accessories) receive the selected
    // authored curve directly. In spring-damper mode the 21 semantic controls
    // use those samples as physical targets; bicubic mode drives all controls
    // directly from the four-key curve.
    for (uint32_t node = 0; node < map.nodeCount; ++node) {
        map.localTransforms[node] = JPH::Mat44::sRotationTranslation(targetRotations[node], targetTranslations[node]).PreScaled(targetScales[node]);
    }

    if (!map.springPoseInitialized || mode == PoseInterpolationMode::Bicubic) {
        for (size_t semantic = 0; semantic < kCoreBoneCount; ++semantic) {
            const int32_t node = map.nodeIndices[semantic];
            if (node < 0 || node >= static_cast<int32_t>(map.nodeCount)) {
                continue;
            }
            map.poseTranslations[semantic]          = targetTranslations[static_cast<size_t>(node)];
            map.poseTranslationVelocities[semantic] = JPH::Vec3::sZero();
            map.poseRotations[semantic]             = targetRotations[static_cast<size_t>(node)];
            map.poseAngularVelocities[semantic]     = JPH::Vec3::sZero();
            map.poseScales[semantic]                = targetScales[static_cast<size_t>(node)];
            map.poseScaleVelocities[semantic]       = JPH::Vec3::sZero();
        }
        map.springPoseInitialized = true;
    }
    if (mode == PoseInterpolationMode::Bicubic) {
        map.springPoseTrack = activeTrack;
        return;
    }

    for (size_t semantic = 0; semantic < kCoreBoneCount; ++semantic) {
        const int32_t node = map.nodeIndices[semantic];
        if (node < 0 || node >= static_cast<int32_t>(map.nodeCount)) {
            continue;
        }
        const size_t nodeIndex = static_cast<size_t>(node);
        SpringVector(
            map.poseTranslations[semantic], map.poseTranslationVelocities[semantic], targetTranslations[nodeIndex], dt, springStiffness, springDampingFactor
        );
        SpringRotation(map.poseRotations[semantic], map.poseAngularVelocities[semantic], targetRotations[nodeIndex], dt, springStiffness, springDampingFactor);
        SpringVector(map.poseScales[semantic], map.poseScaleVelocities[semantic], targetScales[nodeIndex], dt, springStiffness, springDampingFactor);
        map.localTransforms[nodeIndex] =
            JPH::Mat44::sRotationTranslation(map.poseRotations[semantic], map.poseTranslations[semantic]).PreScaled(map.poseScales[semantic]);
    }
    map.springPoseTrack = activeTrack;
}

void ResolveForwardKinematics(RigBoneMap& map) noexcept {
    std::array<bool, kMaxRigNodes> computed {};
    std::array<bool, kMaxRigNodes> visiting {};

    auto resolveNode = [&](auto& self, int32_t node) -> JPH::Mat44 {
        if (node < 0 || node >= static_cast<int32_t>(map.nodeCount)) {
            return JPH::Mat44::sIdentity();
        }
        if (computed[static_cast<size_t>(node)]) {
            return map.modelTransforms[static_cast<size_t>(node)];
        }
        if (visiting[static_cast<size_t>(node)]) {
            map.modelTransforms[static_cast<size_t>(node)] = map.localTransforms[static_cast<size_t>(node)];
            computed[static_cast<size_t>(node)]            = true;
            return map.modelTransforms[static_cast<size_t>(node)];
        }

        visiting[static_cast<size_t>(node)]            = true;
        const int32_t parent                           = map.parentIndices[static_cast<size_t>(node)];
        map.modelTransforms[static_cast<size_t>(node)] = (parent >= 0 && parent < static_cast<int32_t>(map.nodeCount)) ?
                                                             self(self, parent) * map.localTransforms[static_cast<size_t>(node)] :
                                                             map.localTransforms[static_cast<size_t>(node)];
        visiting[static_cast<size_t>(node)]            = false;
        computed[static_cast<size_t>(node)]            = true;
        return map.modelTransforms[static_cast<size_t>(node)];
    };

    for (uint32_t node = 0; node < map.nodeCount; ++node) {
        static_cast<void>(resolveNode(resolveNode, static_cast<int32_t>(node)));
    }
}

void CaptureLocalPose(RigBoneMap& map) noexcept {
    for (uint32_t node = 0; node < map.nodeCount; ++node) {
        const int32_t parent      = map.parentIndices[node];
        map.localTransforms[node] = (parent >= 0 && parent < static_cast<int32_t>(map.nodeCount)) ?
                                        map.modelTransforms[static_cast<size_t>(parent)].Inversed() * map.modelTransforms[node] :
                                        map.modelTransforms[node];
    }
}

struct SkinBinding {
    Components::SkeletalMeshComponent* skeletalMesh = nullptr;
    const Skeleton*                    skeleton     = nullptr;
};

[[nodiscard]] SkinBinding FindSkinBinding(ECS::Registry& registry, Entity root, const ModelPrefab* prefab) noexcept {
    if (prefab == nullptr) {
        return {};
    }

    auto resolve = [&](Entity entity) -> SkinBinding {
        auto* mesh = registry.Get<Components::SkeletalMeshComponent>(entity);
        if (mesh == nullptr || mesh->skeletonIndex < 0 || mesh->skeletonIndex >= static_cast<int32_t>(prefab->skeletons.size())) {
            return {};
        }
        return {.skeletalMesh = mesh, .skeleton = &prefab->skeletons[static_cast<size_t>(mesh->skeletonIndex)]};
    };

    if (SkinBinding direct = resolve(root); direct.skeletalMesh != nullptr) {
        return direct;
    }

    for (Entity entity: registry.GetEntitiesWith<Components::SkeletalMeshComponent>()) {
        const auto* hierarchy = registry.Get<Components::HierarchyComponent>(entity);
        if (hierarchy != nullptr && hierarchy->parent == root) {
            if (SkinBinding child = resolve(entity); child.skeletalMesh != nullptr) {
                return child;
            }
        }
    }
    return {};
}

[[nodiscard]] JPH::Vec3 ModelToWorld(JPH::Vec3Arg rootPosition, JPH::QuatArg rootRotation, JPH::Vec3Arg modelPosition) noexcept {
    return JPH::Vec3(rootPosition) + rootRotation * modelPosition;
}

} // namespace

int32_t FindAnimationTrack(const ModelPrefab& prefab, std::string_view name) noexcept {
    const CanonicalName query = Canonicalize(name);
    if (query.size == 0) {
        return -1;
    }

    for (size_t index = 0; index < prefab.animations.size(); ++index) {
        const CanonicalName candidate = Canonicalize(std::string_view(prefab.animations[index].name));
        if (candidate.View() == query.View()) {
            return static_cast<int32_t>(index);
        }
    }
    for (size_t index = 0; index < prefab.animations.size(); ++index) {
        const CanonicalName candidate = Canonicalize(std::string_view(prefab.animations[index].name));
        if (candidate.View().find(query.View()) != std::string_view::npos) {
            return static_cast<int32_t>(index);
        }
    }
    return -1;
}

bool BuildBoneMap(const ModelPrefab& prefab, const Skeleton& skeleton, RigBoneMap& outMap) noexcept {
    ResetRigBoneMap(outMap);

    outMap.sourcePrefab = &prefab;
    outMap.nodeCount    = std::min<uint32_t>(static_cast<uint32_t>(prefab.nodes.size()), static_cast<uint32_t>(kMaxRigNodes));
    outMap.jointCount   = std::min<uint32_t>(static_cast<uint32_t>(skeleton.joints.size()), static_cast<uint32_t>(kMaxRigNodes));
    if (prefab.nodes.size() > kMaxRigNodes) {
        ZHLN::Log(
            "[ProceduralAnimation] Rig '{}' contains {} nodes; evaluating the first {} and safely detaching parents outside that window.", prefab.virtualPath,
            prefab.nodes.size(), kMaxRigNodes
        );
    }

    for (uint32_t node = 0; node < outMap.nodeCount; ++node) {
        const int32_t importedParent     = prefab.nodes[node].parentIndex;
        const bool    parentIsValid      = importedParent >= 0 && importedParent < static_cast<int32_t>(outMap.nodeCount) &&
                                           importedParent != static_cast<int32_t>(node);
        outMap.parentIndices[node]       = parentIsValid ? importedParent : -1;
        outMap.bindLocalTransforms[node] = prefab.nodes[node].localTransform;
        outMap.localTransforms[node]     = prefab.nodes[node].localTransform;
    }

    std::array<bool, kMaxRigNodes> claimed {};
    size_t                         coreMapped = 0;
    for (size_t semanticIndex = 0; semanticIndex < kCoreBoneCount; ++semanticIndex) {
        const auto semantic = static_cast<CharacterBone>(semanticIndex);
        int32_t    bestNode = -1;

        // First search exact normalized aliases across the entire rig. Only
        // fall back to suffix matching for exporter-specific prefixes. This
        // prevents Forearm.L from being consumed as UpperArm.L ("armL") and
        // Sup_Spine from being consumed as Spine when node order is unusual.
        for (uint32_t pass = 0; pass < 2 && bestNode < 0; ++pass) {
            const bool allowSuffix = pass != 0;
            for (uint32_t node = 0; node < outMap.nodeCount; ++node) {
                if (claimed[node]) {
                    continue;
                }
                const CanonicalName    canonical  = Canonicalize(std::string_view(prefab.nodes[node].name));
                const std::string_view normalized = StripKnownRigPrefix(canonical.View());
                if (MatchesBone(normalized, semantic, allowSuffix)) {
                    bestNode = static_cast<int32_t>(node);
                    break;
                }
            }
        }
        if (bestNode >= 0) {
            outMap.nodeIndices[semanticIndex]      = bestNode;
            claimed[static_cast<size_t>(bestNode)] = true;
            ++coreMapped;
        }
    }

    // Discover secondary chains from hierarchy rather than relying on glTF
    // storage order (exporters may emit depth-first or breadth-first nodes).
    std::array<bool, kMaxRigNodes> hairCandidate {};
    for (uint32_t node = 0; node < outMap.nodeCount; ++node) {
        const CanonicalName canonical = Canonicalize(std::string_view(prefab.nodes[node].name));
        hairCandidate[node]           = !claimed[node] && IsHairNode(canonical.View());
    }
    // A named hair/strand root often has generically named child joints. Mark
    // those descendants as candidates too.
    for (uint32_t pass = 0; pass < outMap.nodeCount; ++pass) {
        bool changed = false;
        for (uint32_t node = 0; node < outMap.nodeCount; ++node) {
            const int32_t parent = outMap.parentIndices[node];
            if (!claimed[node] && !hairCandidate[node] && parent >= 0 && parent < static_cast<int32_t>(outMap.nodeCount) &&
                hairCandidate[static_cast<size_t>(parent)]) {
                hairCandidate[node] = true;
                changed             = true;
            }
        }
        if (!changed) {
            break;
        }
    }

    std::array<bool, kMaxRigNodes>                          hairClaimed {};
    std::array<bool, HairStrandsComponent::kTotalParticles> hairSlotClaimed {};

    // Prefer explicit DEF-Hair_Sxx_yy addressing. Strands in production rigs
    // may contain four, five, or six links, so preserve the semantic gaps
    // instead of shifting the next strand into the previous strand's slots.
    for (uint32_t node = 0; node < outMap.nodeCount; ++node) {
        if (!hairCandidate[node] || claimed[node]) {
            continue;
        }
        const HairAddress address = ParseHairAddress(std::string_view(prefab.nodes[node].name));
        if (!address.IsValid()) {
            continue;
        }

        const size_t slot = static_cast<size_t>(address.strand) * HairStrandsComponent::kLinksPerStrand + static_cast<size_t>(address.link);
        if (hairSlotClaimed[slot]) {
            continue;
        }
        const size_t semanticIndex        = static_cast<size_t>(CharacterBone::HairStart) + slot;
        outMap.nodeIndices[semanticIndex] = static_cast<int32_t>(node);
        hairSlotClaimed[slot]             = true;
        hairClaimed[node]                 = true;
        claimed[node]                     = true;
    }

    // Fallback for rigs that name only the strand roots: assign each hierarchy
    // chain to the next entirely unmapped strand.
    size_t nextFallbackStrand = 0;
    for (uint32_t root = 0; root < outMap.nodeCount && nextFallbackStrand < HairStrandsComponent::kStrandCount; ++root) {
        const int32_t parent       = outMap.parentIndices[root];
        const bool    parentIsHair = parent >= 0 && parent < static_cast<int32_t>(outMap.nodeCount) && hairCandidate[static_cast<size_t>(parent)];
        if (!hairCandidate[root] || hairClaimed[root] || parentIsHair) {
            continue;
        }
        while (nextFallbackStrand < HairStrandsComponent::kStrandCount && hairSlotClaimed[nextFallbackStrand * HairStrandsComponent::kLinksPerStrand]) {
            ++nextFallbackStrand;
        }
        if (nextFallbackStrand >= HairStrandsComponent::kStrandCount) {
            break;
        }

        int32_t current = static_cast<int32_t>(root);
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand && current >= 0; ++link) {
            const size_t slot                         = nextFallbackStrand * HairStrandsComponent::kLinksPerStrand + link;
            const size_t semanticIndex                = static_cast<size_t>(CharacterBone::HairStart) + slot;
            outMap.nodeIndices[semanticIndex]         = current;
            hairSlotClaimed[slot]                     = true;
            claimed[static_cast<size_t>(current)]     = true;
            hairClaimed[static_cast<size_t>(current)] = true;

            int32_t next = -1;
            for (uint32_t node = 0; node < outMap.nodeCount; ++node) {
                if (hairCandidate[node] && !hairClaimed[node] && outMap.parentIndices[node] == current) {
                    next = static_cast<int32_t>(node);
                    break;
                }
            }
            current = next;
        }
        ++nextFallbackStrand;
    }

    // Preserve unusual unstructured hair nodes by filling only genuinely empty
    // slots. Explicit strand/link addresses always win.
    size_t freeHairSlot = 0;
    for (uint32_t node = 0; node < outMap.nodeCount && freeHairSlot < HairStrandsComponent::kTotalParticles; ++node) {
        if (!hairCandidate[node] || hairClaimed[node]) {
            continue;
        }
        while (freeHairSlot < HairStrandsComponent::kTotalParticles && hairSlotClaimed[freeHairSlot]) {
            ++freeHairSlot;
        }
        if (freeHairSlot >= HairStrandsComponent::kTotalParticles) {
            break;
        }
        outMap.nodeIndices[static_cast<size_t>(CharacterBone::HairStart) + freeHairSlot] = static_cast<int32_t>(node);
        hairSlotClaimed[freeHairSlot]                                                    = true;
        claimed[node]                                                                    = true;
        hairClaimed[node]                                                                = true;
    }

    size_t mappedHairStrands = 0;
    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        bool hasMappedLink = false;
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            hasMappedLink = hasMappedLink || hairSlotClaimed[strand * HairStrandsComponent::kLinksPerStrand + link];
        }
        mappedHairStrands += hasMappedLink ? 1u : 0u;
    }

    for (size_t semanticIndex = 0; semanticIndex < kBoneCount; ++semanticIndex) {
        const int32_t node = outMap.nodeIndices[semanticIndex];
        if (node < 0) {
            continue;
        }
        for (const Joint& joint: skeleton.joints) {
            if (joint.nodeIndex == node) {
                outMap.inverseBindMatrices[semanticIndex] = joint.inverseBindMatrix;
                break;
            }
        }
    }

    ResolveForwardKinematics(outMap);
    outMap.initialized = true;
    outMap.poseValid   = true;
    return coreMapped == kCoreBoneCount && mappedHairStrands == HairStrandsComponent::kStrandCount;
}

void BuildStandardProceduralRig(RigBoneMap& outMap) noexcept {
    ResetRigBoneMap(outMap);
    outMap.nodeCount  = static_cast<uint32_t>(kBoneCount);
    outMap.jointCount = static_cast<uint32_t>(kBoneCount);

    auto addBone = [&](CharacterBone bone, int32_t parentNode, JPH::Vec3Arg translation) {
        const size_t  semantic               = static_cast<size_t>(bone);
        const int32_t node                   = static_cast<int32_t>(semantic);
        outMap.nodeIndices[semantic]         = node;
        outMap.parentIndices[semantic]       = parentNode;
        outMap.bindLocalTransforms[semantic] = JPH::Mat44::sTranslation(translation);
        outMap.localTransforms[semantic]     = outMap.bindLocalTransforms[semantic];
    };

    addBone(CharacterBone::Root, -1, JPH::Vec3::sZero());
    addBone(CharacterBone::Hips, static_cast<int32_t>(CharacterBone::Root), JPH::Vec3(0.0f, 1.0f, 0.0f));
    addBone(CharacterBone::Spine, static_cast<int32_t>(CharacterBone::Hips), JPH::Vec3(0.0f, 0.15f, 0.0f));
    addBone(CharacterBone::SupSpine, static_cast<int32_t>(CharacterBone::Spine), JPH::Vec3(0.0f, 0.16f, 0.0f));
    addBone(CharacterBone::Chest, static_cast<int32_t>(CharacterBone::SupSpine), JPH::Vec3(0.0f, 0.17f, 0.0f));
    addBone(CharacterBone::Neck, static_cast<int32_t>(CharacterBone::Chest), JPH::Vec3(0.0f, 0.16f, 0.0f));
    addBone(CharacterBone::Head, static_cast<int32_t>(CharacterBone::Neck), JPH::Vec3(0.0f, 0.12f, 0.0f));

    addBone(CharacterBone::UpperArmL, static_cast<int32_t>(CharacterBone::Chest), JPH::Vec3(0.21f, 0.10f, 0.0f));
    addBone(CharacterBone::ForearmL, static_cast<int32_t>(CharacterBone::UpperArmL), JPH::Vec3(0.28f, 0.0f, 0.0f));
    addBone(CharacterBone::HandL, static_cast<int32_t>(CharacterBone::ForearmL), JPH::Vec3(0.24f, 0.0f, 0.0f));
    addBone(CharacterBone::UpperArmR, static_cast<int32_t>(CharacterBone::Chest), JPH::Vec3(-0.21f, 0.10f, 0.0f));
    addBone(CharacterBone::ForearmR, static_cast<int32_t>(CharacterBone::UpperArmR), JPH::Vec3(-0.28f, 0.0f, 0.0f));
    addBone(CharacterBone::HandR, static_cast<int32_t>(CharacterBone::ForearmR), JPH::Vec3(-0.24f, 0.0f, 0.0f));

    addBone(CharacterBone::ThighL, static_cast<int32_t>(CharacterBone::Hips), JPH::Vec3(0.12f, -0.06f, 0.0f));
    addBone(CharacterBone::ShinL, static_cast<int32_t>(CharacterBone::ThighL), JPH::Vec3(0.0f, -0.43f, 0.015f));
    addBone(CharacterBone::FootL, static_cast<int32_t>(CharacterBone::ShinL), JPH::Vec3(0.0f, -0.42f, 0.025f));
    addBone(CharacterBone::ToeL, static_cast<int32_t>(CharacterBone::FootL), JPH::Vec3(0.0f, -0.045f, 0.18f));
    addBone(CharacterBone::ThighR, static_cast<int32_t>(CharacterBone::Hips), JPH::Vec3(-0.12f, -0.06f, 0.0f));
    addBone(CharacterBone::ShinR, static_cast<int32_t>(CharacterBone::ThighR), JPH::Vec3(0.0f, -0.43f, 0.015f));
    addBone(CharacterBone::FootR, static_cast<int32_t>(CharacterBone::ShinR), JPH::Vec3(0.0f, -0.42f, 0.025f));
    addBone(CharacterBone::ToeR, static_cast<int32_t>(CharacterBone::FootR), JPH::Vec3(0.0f, -0.045f, 0.18f));

    constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        const float angle = kTwoPi * static_cast<float>(strand) / static_cast<float>(HairStrandsComponent::kStrandCount);
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t particle          = strand * HairStrandsComponent::kLinksPerStrand + link;
            const size_t semantic          = static_cast<size_t>(CharacterBone::HairStart) + particle;
            outMap.nodeIndices[semantic]   = static_cast<int32_t>(semantic);
            outMap.parentIndices[semantic] = (link == 0) ? static_cast<int32_t>(CharacterBone::Head) : static_cast<int32_t>(semantic - 1);
            const JPH::Vec3 translation = (link == 0) ? JPH::Vec3(std::cos(angle) * 0.150f, 0.075f, std::sin(angle) * 0.150f) : JPH::Vec3(0.0f, -0.105f, 0.0f);
            outMap.bindLocalTransforms[semantic] = JPH::Mat44::sTranslation(translation);
            outMap.localTransforms[semantic]     = outMap.bindLocalTransforms[semantic];
        }
    }

    ResolveForwardKinematics(outMap);
    for (size_t semantic = 0; semantic < kBoneCount; ++semantic) {
        const int32_t node = outMap.nodeIndices[semantic];
        if (node >= 0) {
            outMap.inverseBindMatrices[semantic] = outMap.modelTransforms[static_cast<size_t>(node)].Inversed();
        }
    }
    outMap.initialized = true;
    outMap.poseValid   = true;
}

void ProceduralAnimationSystem::Update(Engine& engine, float dt) noexcept {
    ZHLN::ScopedTimer timer("ECS System: Procedural Animation");

    auto& registry = engine.GetRegistry();
    auto& physics  = engine.GetPhysicsContext();
    auto& renderer = engine.GetRenderContext();

    for (Entity entity: registry.GetEntitiesWith<ProceduralLocomotionComponent>()) {
        auto* gait             = registry.Get<ProceduralLocomotionComponent>(entity);
        auto* hair             = registry.Get<HairStrandsComponent>(entity);
        auto* config           = registry.Get<ProceduralAnimationConfigComponent>(entity);
        auto* transform        = registry.Get<Components::TransformComponent>(entity);
        auto* physicsComponent = registry.Get<Components::PhysicsComponent>(entity);
        auto* boneMap          = registry.Get<RigBoneMap>(entity);
        if (gait == nullptr || transform == nullptr || boneMap == nullptr) {
            continue;
        }

        const auto*        animator = registry.Get<Components::AnimatorComponent>(entity);
        const ModelPrefab* prefab   = animator != nullptr ? animator->prefab : boneMap->sourcePrefab;
        SkinBinding        skin     = FindSkinBinding(registry, entity, prefab);

        if ((!boneMap->initialized || boneMap->sourcePrefab != prefab) && prefab != nullptr && skin.skeleton != nullptr) {
            const bool complete    = BuildBoneMap(*prefab, *skin.skeleton, *boneMap);
            boneMap->jointOffset   = skin.skeletalMesh->jointOffset;
            boneMap->skeletonIndex = skin.skeletalMesh->skeletonIndex;
            if (hair != nullptr) {
                hair->bindPoseInitialized = false;
                hair->initialized         = false;
            }

            size_t mappedCoreBones = 0;
            for (size_t semantic = 0; semantic < kCoreBoneCount; ++semantic) {
                mappedCoreBones += boneMap->nodeIndices[semantic] >= 0 ? 1u : 0u;
            }
            size_t mappedHairBones   = 0;
            size_t mappedHairStrands = 0;
            for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
                bool strandMapped = false;
                for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t semantic = static_cast<size_t>(CharacterBone::HairStart) + strand * HairStrandsComponent::kLinksPerStrand + link;
                    if (boneMap->nodeIndices[semantic] >= 0) {
                        ++mappedHairBones;
                        strandMapped = true;
                    }
                }
                mappedHairStrands += strandMapped ? 1u : 0u;
            }
            ZHLN::Log(
                "[ProceduralAnimation] Rig '{}': {}; mapped {}/{} core bones and {}/{} hair strands ({} deform hair bones).", prefab->virtualPath,
                complete ? "complete" : "partial", mappedCoreBones, kCoreBoneCount, mappedHairStrands, HairStrandsComponent::kStrandCount, mappedHairBones
            );
            if (animator != nullptr && animator->currentTrackIdx >= 0 && animator->currentTrackIdx < static_cast<int32_t>(prefab->animations.size())) {
                const AnimationClip& clip                    = prefab->animations[static_cast<size_t>(animator->currentTrackIdx)];
                size_t               usableTransformChannels = 0;
                for (const AnimationChannel& channel: clip.channels) {
                    usableTransformChannels += channel.path != AnimationPathType::Weights && channel.targetNodeIndex >= 0 &&
                                                       channel.targetNodeIndex < static_cast<int32_t>(boneMap->nodeCount) ?
                                                   1u :
                                                   0u;
                }
                ZHLN::Log(
                    "[ProceduralAnimation] Authored track {}: '{}' (time={}, duration={}, channels={}, usable transforms={}).", animator->currentTrackIdx,
                    clip.name, animator->currentTrackTime, clip.duration, clip.channels.size(), usableTransformChannels
                );
                if (usableTransformChannels == 0) {
                    ZHLN::Log("[ProceduralAnimation] WARNING: selected track has no usable transform channels; bind pose will be shown.");
                }
            } else {
                ZHLN::Log("[ProceduralAnimation] WARNING: no valid authored track selected; bind pose will be shown.");
            }
            if (!complete) {
                for (size_t semantic = 0; semantic < kCoreBoneCount; ++semantic) {
                    if (boneMap->nodeIndices[semantic] < 0) {
                        ZHLN::Log("[ProceduralAnimation] Missing core mapping: {}", kCoreBoneLabels[semantic]);
                    }
                }
                for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
                    const size_t rootSemantic = static_cast<size_t>(CharacterBone::HairStart) + strand * HairStrandsComponent::kLinksPerStrand;
                    bool         strandMapped = false;
                    for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                        strandMapped = strandMapped || boneMap->nodeIndices[rootSemantic + link] >= 0;
                    }
                    if (!strandMapped) {
                        ZHLN::Log("[ProceduralAnimation] Missing hair strand mapping: S{:02}", strand + 1);
                    }
                }
            }
        }
        if (!boneMap->initialized || boneMap->nodeCount == 0) {
            continue;
        }

        if (hair != nullptr && !hair->bindPoseInitialized) {
            std::copy_n(boneMap->bindLocalTransforms.begin(), boneMap->nodeCount, boneMap->localTransforms.begin());
            ResolveForwardKinematics(*boneMap);
            Animation::ConfigureHairBindPose(*hair, boneMap->modelTransforms.data(), *boneMap);
        }
        ApplyAuthoredPose(animator, config, *boneMap, dt);
        ResolveForwardKinematics(*boneMap);

        const bool authoredPoseOnly       = config != nullptr && config->authoredPoseOnly;
        const bool gaitEnabled            = !authoredPoseOnly && (config == nullptr || config->enableGait);
        const bool gravityBounceEnabled   = gaitEnabled && (config == nullptr || config->enableGravityBounce);
        const bool ikEnabled              = !authoredPoseOnly && (config == nullptr || config->enableLegIK);
        const bool accelerationEnabled    = !authoredPoseOnly && (config == nullptr || config->enableAccelerationTilt);
        const bool upperBodyEnabled       = !authoredPoseOnly && (config == nullptr || config->enableUpperBody);
        const bool secondaryMotionEnabled = !authoredPoseOnly && (config == nullptr || config->enableSecondaryMotion);

        const JPH::Vec3 velocityWorld = physicsComponent != nullptr ? physics.GetCharacterVelocity(physicsComponent->physicsHandle) : JPH::Vec3::sZero();
        const JPH::Quat rootRotation  = transform->rotation.Normalized();
        const JPH::Vec3 velocityLocal = rootRotation.Inversed() * velocityWorld;

        float angularVelocity = 0.0f;
        if (gait->orientationInitialized) {
            const JPH::Vec3 oldForward = gait->previousRootRotation * JPH::Vec3::sAxisZ();
            const JPH::Vec3 newForward = rootRotation * JPH::Vec3::sAxisZ();
            const float     dot        = std::clamp(oldForward.Dot(newForward), -1.0f, 1.0f);
            const float     sign       = oldForward.Cross(newForward).GetY() < 0.0f ? -1.0f : 1.0f;
            angularVelocity            = sign * std::acos(dot) / std::max(dt, 0.0001f);
        }
        gait->previousRootRotation   = rootRotation;
        gait->orientationInitialized = true;

        // Stages 1-2: velocity/acceleration calculus, parametric gait, and a
        // whole-body spring tilt around the estimated center of mass.
        if (gaitEnabled) {
            Animation::EvaluateGait(*gait, velocityLocal, angularVelocity, dt);
            if (!gravityBounceEnabled) {
                gait->gravityBounce = 0.0f;
                gait->pelvisBob     = 0.0f;
            }
        }
        if (accelerationEnabled) {
            Animation::ApplyAccelerationTilt(*gait, boneMap->modelTransforms.data(), *boneMap);
        }

        // Stage 3: terrain contact, pelvis reach correction, and two-bone IK.
        if (ikEnabled) {
            const Entity ignoredHandle = physicsComponent != nullptr ? physicsComponent->physicsHandle : Entity {};
            Animation::SolveLegGrounding(engine, transform->position, rootRotation, *gait, boneMap->modelTransforms.data(), *boneMap, ignoredHandle);
        } else if (gaitEnabled) {
            Animation::ApplyPelvisGaitOffset(*gait, boneMap->modelTransforms.data(), *boneMap, false);
        }

        // Stage 4: arm counter-swing and look-at.
        if (upperBodyEnabled) {
            const auto* lookAt = registry.Get<ProceduralLookAtComponent>(entity);
            Animation::SolveUpperBody(*gait, lookAt, transform->position, rootRotation, boneMap->modelTransforms.data(), *boneMap);
        }

        // Stage 5: worker-fiber XPBD secondary motion.
        if (hair != nullptr && secondaryMotionEnabled) {
            const int32_t   headNode          = boneMap->nodeIndices[static_cast<size_t>(CharacterBone::Head)];
            const JPH::Vec3 headModelPosition = (headNode >= 0 && headNode < static_cast<int32_t>(boneMap->nodeCount)) ?
                                                    boneMap->modelTransforms[static_cast<size_t>(headNode)].GetTranslation() :
                                                    JPH::Vec3(0.0f, 1.60f, 0.0f);
            const JPH::Quat headModelRotation = (headNode >= 0 && headNode < static_cast<int32_t>(boneMap->nodeCount)) ?
                                                    ExtractRotation(boneMap->modelTransforms[static_cast<size_t>(headNode)]) :
                                                    JPH::Quat::sIdentity();
            const JPH::Vec3 headWorldPosition = ModelToWorld(transform->position, rootRotation, headModelPosition);
            const JPH::Quat headWorldRotation = (rootRotation * headModelRotation).Normalized();

            {
                ZHLN::ScopedTimer hairTimer("Procedural Animation: 18-Strand XPBD");
                Animation::StepHairSimulation(*hair, headWorldPosition, headWorldRotation, velocityWorld, dt);
            }
            Animation::ExtractHairBoneTransforms(*hair, headWorldRotation, boneMap->modelTransforms.data(), *boneMap);

            // The secondary solver works in world space for collision stability;
            // convert its output back into the character's model space.
            const JPH::Quat inverseRoot = rootRotation.Inversed();
            for (size_t particle = 0; particle < HairStrandsComponent::kTotalParticles; ++particle) {
                const size_t  semantic = static_cast<size_t>(CharacterBone::HairStart) + particle;
                const int32_t node     = boneMap->nodeIndices[semantic];
                if (node < 0 || node >= static_cast<int32_t>(boneMap->nodeCount)) {
                    continue;
                }
                const JPH::Mat44 world         = boneMap->modelTransforms[static_cast<size_t>(node)];
                const JPH::Vec3  modelPosition = inverseRoot * (world.GetTranslation() - transform->position);
                const JPH::Quat  modelRotation = (inverseRoot * ExtractRotation(world)).Normalized();
                const JPH::Vec3  worldScale(world.GetColumn3(0).Length(), world.GetColumn3(1).Length(), world.GetColumn3(2).Length());
                boneMap->modelTransforms[static_cast<size_t>(node)] = JPH::Mat44::sRotationTranslation(modelRotation, modelPosition).PreScaled(worldScale);
            }
        } else if (hair != nullptr) {
            // Re-seed from the authored shape when secondary motion is enabled
            // again instead of resuming stale Verlet velocity.
            hair->initialized = false;
        }

        // Stage 6 is supplied by ArticulationSystem immediately after this
        // system. It consumes hit commands, decays motor stiffness, and blends
        // Jolt ragdoll poses over this kinematic target.

        // Stage 7: recover local transforms and perform one canonical FK pass.
        CaptureLocalPose(*boneMap);
        ResolveForwardKinematics(*boneMap);
        boneMap->poseValid = true;
        ++boneMap->poseVersion;

        // Stage 8: bake the imported skeleton order and stream one contiguous
        // matrix palette into the current Vulkan frame buffer.
        if (skin.skeletalMesh != nullptr && skin.skeleton != nullptr) {
            const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(skin.skeleton->joints.size()), static_cast<uint32_t>(kMaxRigNodes));
            std::array<JPH::Mat44, kMaxRigNodes> finalPalette {};
            std::ranges::fill(finalPalette, JPH::Mat44::sIdentity());
            for (uint32_t jointIndex = 0; jointIndex < count; ++jointIndex) {
                const Joint& joint = skin.skeleton->joints[jointIndex];
                if (joint.nodeIndex >= 0 && joint.nodeIndex < static_cast<int32_t>(boneMap->nodeCount)) {
                    finalPalette[jointIndex] = boneMap->modelTransforms[static_cast<size_t>(joint.nodeIndex)] * joint.inverseBindMatrix;
                }
            }
            boneMap->jointOffset   = skin.skeletalMesh->jointOffset;
            boneMap->jointCount    = count;
            boneMap->skeletonIndex = skin.skeletalMesh->skeletonIndex;
            renderer.UpdateJointMatrices(skin.skeletalMesh->jointOffset, finalPalette.data(), count);
        }
    }
}

void DrawProceduralDebugRig(
    RenderContext&                       renderContext,
    JPH::Vec3Arg                         rootPosition,
    JPH::QuatArg                         rootRotation,
    const RigBoneMap&                    boneMap,
    const ProceduralLocomotionComponent* gait
) noexcept {
    if (!boneMap.poseValid) {
        return;
    }

    const auto worldPosition = [&](int32_t node) {
        return ModelToWorld(rootPosition, rootRotation, boneMap.modelTransforms[static_cast<size_t>(node)].GetTranslation());
    };
    const auto drawCross = [&](JPH::Vec3Arg point, float radius, JPH::Vec4Arg color) {
        renderContext.DrawLine(point - JPH::Vec3(radius, 0.0f, 0.0f), point + JPH::Vec3(radius, 0.0f, 0.0f), color);
        renderContext.DrawLine(point - JPH::Vec3(0.0f, radius, 0.0f), point + JPH::Vec3(0.0f, radius, 0.0f), color);
        renderContext.DrawLine(point - JPH::Vec3(0.0f, 0.0f, radius), point + JPH::Vec3(0.0f, 0.0f, radius), color);
    };

    const JPH::Vec4 torsoColor(0.15f, 0.85f, 1.00f, 1.0f);
    const JPH::Vec4 armColor(1.00f, 0.60f, 0.12f, 1.0f);
    const JPH::Vec4 legColor(0.20f, 1.00f, 0.35f, 1.0f);
    const JPH::Vec4 hairColor(0.90f, 0.25f, 1.00f, 1.0f);

    for (size_t semantic = 0; semantic < kCoreBoneCount; ++semantic) {
        const int32_t node = boneMap.nodeIndices[semantic];
        if (node < 0 || node >= static_cast<int32_t>(boneMap.nodeCount)) {
            continue;
        }
        const int32_t parent = boneMap.parentIndices[static_cast<size_t>(node)];
        JPH::Vec4     color  = torsoColor;
        if (semantic >= static_cast<size_t>(CharacterBone::UpperArmL) && semantic <= static_cast<size_t>(CharacterBone::HandR)) {
            color = armColor;
        } else if (semantic >= static_cast<size_t>(CharacterBone::ThighL)) {
            color = legColor;
        }
        if (parent >= 0 && parent < static_cast<int32_t>(boneMap.nodeCount)) {
            renderContext.DrawLine(worldPosition(parent), worldPosition(node), color);
        }
        drawCross(worldPosition(node), semantic == static_cast<size_t>(CharacterBone::Head) ? 0.07f : 0.025f, color);
    }

    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        for (size_t link = 0; link + 1 < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t  particle0 = strand * HairStrandsComponent::kLinksPerStrand + link;
            const size_t  particle1 = particle0 + 1;
            const int32_t node0     = boneMap.nodeIndices[static_cast<size_t>(CharacterBone::HairStart) + particle0];
            const int32_t node1     = boneMap.nodeIndices[static_cast<size_t>(CharacterBone::HairStart) + particle1];
            if (node0 >= 0 && node1 >= 0 && node0 < static_cast<int32_t>(boneMap.nodeCount) && node1 < static_cast<int32_t>(boneMap.nodeCount)) {
                renderContext.DrawLine(worldPosition(node0), worldPosition(node1), hairColor);
            }
        }
    }

    if (gait != nullptr) {
        const JPH::Vec4 normalColor(1.0f, 0.95f, 0.12f, 1.0f);
        const JPH::Vec3 footL = ModelToWorld(rootPosition, rootRotation, gait->localFootTargetL);
        const JPH::Vec3 footR = ModelToWorld(rootPosition, rootRotation, gait->localFootTargetR);
        renderContext.DrawLine(footL, footL + gait->footNormalL * 0.22f, normalColor);
        renderContext.DrawLine(footR, footR + gait->footNormalR * 0.22f, normalColor);

        // Sagittal stride wheel: cyan ticks are pass poses, orange ticks are
        // reach poses, and the spoke follows the distance-driven gait phase.
        constexpr float wheelRadius      = 0.24f;
        const JPH::Vec3 wheelCenterModel = gait->centerOfMassModel + JPH::Vec3(-0.48f, 0.0f, 0.0f);
        const JPH::Vec4 wheelColor(0.40f, 0.45f, 0.55f, 0.85f);
        const JPH::Vec4 passColor(0.10f, 0.95f, 1.00f, 1.0f);
        const JPH::Vec4 reachColor(1.00f, 0.42f, 0.08f, 1.0f);
        drawCross(ModelToWorld(rootPosition, rootRotation, gait->centerOfMassModel), 0.032f, normalColor);
        for (uint32_t segment = 0; segment < 24; ++segment) {
            const float     angle0 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(segment) / 24.0f;
            const float     angle1 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(segment + 1) / 24.0f;
            const JPH::Vec3 p0     = wheelCenterModel + JPH::Vec3(0.0f, std::sin(angle0) * wheelRadius, std::cos(angle0) * wheelRadius);
            const JPH::Vec3 p1     = wheelCenterModel + JPH::Vec3(0.0f, std::sin(angle1) * wheelRadius, std::cos(angle1) * wheelRadius);
            renderContext.DrawLine(ModelToWorld(rootPosition, rootRotation, p0), ModelToWorld(rootPosition, rootRotation, p1), wheelColor);
        }
        const JPH::Vec3 wheelMarkerModel = wheelCenterModel +
                                           JPH::Vec3(0.0f, std::sin(gait->strideWheelAngle) * wheelRadius, std::cos(gait->strideWheelAngle) * wheelRadius);
        const JPH::Vec4 markerColor      = gait->passWeightL > gait->reachWeightL ? passColor : reachColor;
        renderContext.DrawLine(
            ModelToWorld(rootPosition, rootRotation, wheelCenterModel), ModelToWorld(rootPosition, rootRotation, wheelMarkerModel), markerColor
        );
        drawCross(ModelToWorld(rootPosition, rootRotation, wheelMarkerModel), 0.025f, markerColor);
        drawCross(ModelToWorld(rootPosition, rootRotation, wheelCenterModel + JPH::Vec3(0.0f, wheelRadius, 0.0f)), 0.018f, passColor);
        drawCross(ModelToWorld(rootPosition, rootRotation, wheelCenterModel + JPH::Vec3(0.0f, 0.0f, wheelRadius)), 0.018f, reachColor);
    }
}

} // namespace ZHLN
