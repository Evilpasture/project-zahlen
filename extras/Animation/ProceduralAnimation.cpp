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
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <numbers>
#include <span>
#include <string_view>
#include <utility>

module ZHLN.ProceduralAnimation;

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
    size_t strand = HairStrandsComponent::kStrandCount;
    size_t link   = HairStrandsComponent::kLinksPerStrand;

    [[nodiscard]] bool IsValid() const noexcept {
        return strand < HairStrandsComponent::kStrandCount && link < HairStrandsComponent::kLinksPerStrand;
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

    std::array<size_t, 2> numbers {};
    size_t                numberCount = 0;
    for (size_t i = hairEnd; i < name.size() && numberCount < numbers.size();) {
        if (name[i] < '0' || name[i] > '9') {
            ++i;
            continue;
        }
        size_t value = 0;
        while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
            value = value * 10u + static_cast<size_t>(name[i] - '0');
            ++i;
        }
        numbers[numberCount++] = value;
    }

    if (numberCount != 2 || numbers[0] == 0 || numbers[1] == 0) {
        return {};
    }
    HairAddress result {.strand = numbers[0] - 1, .link = numbers[1] - 1};
    return result.IsValid() ? result : HairAddress {};
}

void FillIdentity(std::span<JPH::Mat44> matrices) noexcept {
    std::ranges::fill(matrices, JPH::Mat44::sIdentity());
}

void ResetRigBoneMap(RigBoneMap& map) noexcept {
    map.nodeIndices.fill(InvalidRigNode);
    FillIdentity(map.inverseBindMatrices);
    map.parentIndices.fill(InvalidRigNode);
    FillIdentity(map.bindLocalTransforms);
    FillIdentity(map.localTransforms);
    FillIdentity(map.modelTransforms);
    map.childOfConstraints.fill({});
    map.childOfConstraintCount = 0;
    map.poseTranslations.fill(JPH::Vec3::sZero());
    map.poseTranslationVelocities.fill(JPH::Vec3::sZero());
    map.poseRotations.fill(JPH::Quat::sIdentity());
    map.poseAngularVelocities.fill(JPH::Vec3::sZero());
    map.poseScales.fill(JPH::Vec3::sReplicate(1.0f));
    map.poseScaleVelocities.fill(JPH::Vec3::sZero());
    map.springPoseTrack              = -1;
    map.springPoseInitialized        = false;
    map.sourcePrefab                 = nullptr;
    map.nodeCount                    = 0;
    map.jointOffset                  = 0;
    map.jointCount                   = 0;
    map.skeletonIndex                = -1;
    map.poseVersion                  = 0;
    map.synchronizedAttachmentCount  = 0;
    map.synchronizedSkinPaletteCount = 0;
    map.initialized                  = false;
    map.poseValid                    = false;
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

    for (size_t node = 0; node < map.nodeCount; ++node) {
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
    for (size_t node = 0; node < map.nodeCount; ++node) {
        map.localTransforms[node] = JPH::Mat44::sRotationTranslation(targetRotations[node], targetTranslations[node]).PreScaled(targetScales[node]);
    }

    if (!map.springPoseInitialized || mode == PoseInterpolationMode::Bicubic) {
        for (size_t semantic = 0; semantic < kCoreBoneCount; ++semantic) {
            const RigNodeIndex node = map.nodeIndices[semantic];
            if (!IsValidRigNode(node, map.nodeCount)) {
                continue;
            }
            map.poseTranslations[semantic]          = targetTranslations[node];
            map.poseTranslationVelocities[semantic] = JPH::Vec3::sZero();
            map.poseRotations[semantic]             = targetRotations[node];
            map.poseAngularVelocities[semantic]     = JPH::Vec3::sZero();
            map.poseScales[semantic]                = targetScales[node];
            map.poseScaleVelocities[semantic]       = JPH::Vec3::sZero();
        }
        map.springPoseInitialized = true;
    }
    if (mode == PoseInterpolationMode::Bicubic) {
        map.springPoseTrack = activeTrack;
        return;
    }

    for (size_t semantic = 0; semantic < kCoreBoneCount; ++semantic) {
        const RigNodeIndex node = map.nodeIndices[semantic];
        if (!IsValidRigNode(node, map.nodeCount)) {
            continue;
        }
        const RigNodeIndex nodeIndex = node;
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

struct AuthoredUpperBodyCoverage {
    bool arms      = false;
    bool torsoHead = false;
};

[[nodiscard]] AuthoredUpperBodyCoverage FindAuthoredUpperBodyCoverage(const Components::AnimatorComponent* animator, const RigBoneMap& map) noexcept {
    AuthoredUpperBodyCoverage coverage;
    if (animator == nullptr || animator->prefab == nullptr || animator->currentTrackIdx < 0 ||
        animator->currentTrackIdx >= static_cast<int32_t>(animator->prefab->animations.size())) {
        return coverage;
    }

    constexpr std::array armBones = {
        CharacterBone::UpperArmL, CharacterBone::ForearmL, CharacterBone::HandL, CharacterBone::UpperArmR, CharacterBone::ForearmR, CharacterBone::HandR,
    };
    constexpr std::array torsoHeadBones = {
        CharacterBone::Spine, CharacterBone::SupSpine, CharacterBone::Chest, CharacterBone::Neck, CharacterBone::Head,
    };

    const AnimationClip& clip = animator->prefab->animations[static_cast<size_t>(animator->currentTrackIdx)];
    for (const AnimationChannel& channel: clip.channels) {
        if (channel.path == AnimationPathType::Weights || channel.targetNodeIndex < 0) {
            continue;
        }
        const RigNodeIndex target = static_cast<RigNodeIndex>(channel.targetNodeIndex);
        for (CharacterBone bone: armBones) {
            coverage.arms = coverage.arms || map.nodeIndices[BoneSlot(bone)] == target;
        }
        for (CharacterBone bone: torsoHeadBones) {
            coverage.torsoHead = coverage.torsoHead || map.nodeIndices[BoneSlot(bone)] == target;
        }
    }
    return coverage;
}

[[nodiscard]] bool IsValidTrack(const Components::AnimatorComponent& animator, int32_t track) noexcept {
    return animator.prefab != nullptr && track >= 0 && track < static_cast<int32_t>(animator.prefab->animations.size());
}

void SynchronizeLocomotionTrack(
    Components::AnimatorComponent&       animator,
    ProceduralLocomotionTracksComponent& tracks,
    const Components::MovementComponent* movement,
    const ProceduralLocomotionComponent& gait,
    float                                speed
) noexcept {
    const bool moving  = speed > std::max(tracks.movementThreshold, 0.0f);
    const bool running = moving && ((movement != nullptr && movement->isSprinting) || speed >= std::max(tracks.runSpeedThreshold, 0.0f));

    int32_t desiredTrack = tracks.idleTrack;
    if (moving) {
        desiredTrack = running && IsValidTrack(animator, tracks.runTrack) ? tracks.runTrack : tracks.walkTrack;
        if (!IsValidTrack(animator, desiredTrack)) {
            desiredTrack = IsValidTrack(animator, tracks.runTrack) ? tracks.runTrack : tracks.idleTrack;
        }
    }
    if (!IsValidTrack(animator, desiredTrack)) {
        return;
    }

    if (animator.currentTrackIdx != desiredTrack) {
        animator.prevTrackIdx      = animator.currentTrackIdx;
        animator.prevTrackTime     = animator.currentTrackTime;
        animator.prevPlaybackSpeed = animator.currentPlaybackSpeed;
        animator.currentTrackIdx   = desiredTrack;
        animator.currentTrackTime  = 0.0f;
        animator.blendFactor       = 0.0f;
        animator.isFinished        = false;
    }

    tracks.passWeight  = 0.5f * (gait.passWeightL + gait.passWeightR);
    tracks.reachWeight = 0.5f * (gait.reachWeightL + gait.reachWeightR);
    if (moving && tracks.synchronizeToStrideWheel) {
        const float wheelPhase = gait.phase;
        // Two authored keys represent opposing reach poses. Ping-pong across
        // the stride wheel makes pass occur halfway between them and returns to
        // key zero without a loop discontinuity.
        const float          posePhase = Animation::EvaluateTwoKeyPosePhase(wheelPhase);
        const AnimationClip& clip      = animator.prefab->animations[static_cast<size_t>(desiredTrack)];
        tracks.synchronizedPhase       = posePhase;
        tracks.synchronizedTime        = posePhase * std::max(clip.duration, 0.0f);
        animator.currentTrackTime      = tracks.synchronizedTime;
    } else {
        tracks.synchronizedPhase = 0.0f;
        tracks.synchronizedTime  = animator.currentTrackTime;
    }
}

void ResolveForwardKinematics(RigBoneMap& map) noexcept {
    std::array<bool, kMaxRigNodes> computed {};
    std::array<bool, kMaxRigNodes> visiting {};

    auto resolveNode = [&](auto& self, RigNodeIndex node) -> JPH::Mat44 {
        if (!IsValidRigNode(node, map.nodeCount)) {
            return JPH::Mat44::sIdentity();
        }
        if (computed[node]) {
            return map.modelTransforms[node];
        }
        if (visiting[node]) {
            map.modelTransforms[node] = map.localTransforms[node];
            computed[node]            = true;
            return map.modelTransforms[node];
        }

        visiting[node]            = true;
        const RigNodeIndex parent = map.parentIndices[node];
        map.modelTransforms[node] = IsValidRigNode(parent, map.nodeCount) ? self(self, parent) * map.localTransforms[node] : map.localTransforms[node];
        visiting[node]            = false;
        computed[node]            = true;
        return map.modelTransforms[node];
    };

    for (RigNodeIndex node = 0; node < map.nodeCount; ++node) {
        static_cast<void>(resolveNode(resolveNode, node));
    }
}

void CaptureLocalPose(RigBoneMap& map) noexcept {
    for (RigNodeIndex node = 0; node < map.nodeCount; ++node) {
        const RigNodeIndex parent = map.parentIndices[node];
        map.localTransforms[node] = IsValidRigNode(parent, map.nodeCount) ? map.modelTransforms[parent].Inversed() * map.modelTransforms[node] :
                                                                            map.modelTransforms[node];
    }
}

[[nodiscard]] bool IsNodeDescendant(const RigBoneMap& map, RigNodeIndex node, RigNodeIndex ancestor) noexcept {
    RigNodeIndex cursor = node;
    for (size_t depth = 0; depth < map.nodeCount && IsValidRigNode(cursor, map.nodeCount); ++depth) {
        if (cursor == ancestor) {
            return true;
        }
        cursor = map.parentIndices[cursor];
    }
    return false;
}

void ConfigureHumanoidChildOfConstraints(RigBoneMap& map) noexcept {
    map.childOfConstraints.fill({});
    map.childOfConstraintCount = 0;

    auto addConstraint = [&](CharacterBone parentBone, CharacterBone childBone, RigChildOfKind kind) {
        const RigNodeIndex parent = map.nodeIndices[BoneSlot(parentBone)];
        const RigNodeIndex child  = map.nodeIndices[BoneSlot(childBone)];
        if (!IsValidRigNode(parent, map.nodeCount) || !IsValidRigNode(child, map.nodeCount) || IsNodeDescendant(map, child, parent) ||
            map.childOfConstraintCount >= map.childOfConstraints.size()) {
            return;
        }
        RigChildOfConstraint& constraint = map.childOfConstraints[map.childOfConstraintCount++];
        constraint.parent                = parent;
        constraint.child                 = child;
        constraint.kind                  = kind;
        constraint.bindRelative          = map.modelTransforms[parent].Inversed() * map.modelTransforms[child];
        constraint.localPoseDelta        = JPH::Mat44::sIdentity();
    };

    addConstraint(CharacterBone::ForearmL, CharacterBone::HandL, RigChildOfKind::Hand);
    addConstraint(CharacterBone::ForearmR, CharacterBone::HandR, RigChildOfKind::Hand);
    addConstraint(CharacterBone::SupSpine, CharacterBone::Chest, RigChildOfKind::Chest);
}

struct SkinBinding {
    Components::SkeletalMeshComponent* skeletalMesh = nullptr;
    const Skeleton*                    skeleton     = nullptr;
};

[[nodiscard]] size_t ScoreSkeletonForProceduralPose(const Skeleton& skeleton) noexcept {
    size_t coreMatches = 0;
    for (size_t semanticIndex = 0; semanticIndex < kCoreBoneCount; ++semanticIndex) {
        const CharacterBone semantic = static_cast<CharacterBone>(semanticIndex);
        bool                matched  = false;
        for (const Joint& joint: skeleton.joints) {
            const CanonicalName    canonical  = Canonicalize(std::string_view(joint.name));
            const std::string_view normalized = StripKnownRigPrefix(canonical.View());
            if (MatchesBone(normalized, semantic, false) || MatchesBone(normalized, semantic, true)) {
                matched = true;
                break;
            }
        }
        coreMatches += matched ? 1u : 0u;
    }

    size_t hairMatches = 0;
    for (const Joint& joint: skeleton.joints) {
        hairMatches += IsHairNode(Canonicalize(std::string_view(joint.name)).View()) ? 1u : 0u;
    }
    return coreMatches * 1000u + hairMatches;
}

[[nodiscard]] SkinBinding FindSkinBinding(ECS::Registry& registry, Entity root, const ModelPrefab* prefab) noexcept {
    if (prefab == nullptr) {
        return {};
    }

    SkinBinding best;
    size_t      bestScore = 0;
    auto        consider  = [&](Entity entity) {
        auto* mesh = registry.Get<Components::SkeletalMeshComponent>(entity);
        if (mesh == nullptr || mesh->skeletonIndex < 0 || mesh->skeletonIndex >= static_cast<int32_t>(prefab->skeletons.size())) {
            return;
        }
        const Skeleton& skeleton = prefab->skeletons[static_cast<size_t>(mesh->skeletonIndex)];
        const size_t    score    = ScoreSkeletonForProceduralPose(skeleton);
        if (best.skeletalMesh == nullptr || score > bestScore) {
            best      = {.skeletalMesh = mesh, .skeleton = &skeleton};
            bestScore = score;
        }
    };

    consider(root);
    for (Entity entity: registry.GetEntitiesWith<Components::SkeletalMeshComponent>()) {
        const auto* hierarchy = registry.Get<Components::HierarchyComponent>(entity);
        if (hierarchy != nullptr && hierarchy->parent == root) {
            consider(entity);
        }
    }
    return best;
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
    outMap.nodeCount    = std::min(prefab.nodes.size(), kMaxRigNodes);
    outMap.jointCount   = static_cast<uint32_t>(std::min(skeleton.joints.size(), kMaxRigNodes));
    if (prefab.nodes.size() > kMaxRigNodes) {
        ZHLN::Log(
            "[ProceduralAnimation] Rig '{}' contains {} nodes; evaluating the first {} and safely detaching parents outside that window.", prefab.virtualPath,
            prefab.nodes.size(), kMaxRigNodes
        );
    }

    for (RigNodeIndex node = 0; node < outMap.nodeCount; ++node) {
        const int32_t importedParent     = prefab.nodes[node].parentIndex;
        const bool    parentIsValid      = importedParent >= 0 && static_cast<RigNodeIndex>(importedParent) < outMap.nodeCount &&
                                           static_cast<RigNodeIndex>(importedParent) != node;
        outMap.parentIndices[node]       = parentIsValid ? static_cast<RigNodeIndex>(importedParent) : InvalidRigNode;
        outMap.bindLocalTransforms[node] = prefab.nodes[node].localTransform;
        outMap.localTransforms[node]     = prefab.nodes[node].localTransform;
    }

    std::array<bool, kMaxRigNodes> claimed {};
    size_t                         coreMapped = 0;
    for (size_t semanticIndex = 0; semanticIndex < kCoreBoneCount; ++semanticIndex) {
        const auto   semantic = static_cast<CharacterBone>(semanticIndex);
        RigNodeIndex bestNode = InvalidRigNode;

        // First search exact normalized aliases across the entire rig. Only
        // fall back to suffix matching for exporter-specific prefixes. This
        // prevents Forearm.L from being consumed as UpperArm.L ("armL") and
        // Sup_Spine from being consumed as Spine when node order is unusual.
        for (size_t pass = 0; pass < 2 && bestNode == InvalidRigNode; ++pass) {
            const bool allowSuffix = pass != 0;
            for (RigNodeIndex node = 0; node < outMap.nodeCount; ++node) {
                if (claimed[node]) {
                    continue;
                }
                const CanonicalName    canonical  = Canonicalize(std::string_view(prefab.nodes[node].name));
                const std::string_view normalized = StripKnownRigPrefix(canonical.View());
                if (MatchesBone(normalized, semantic, allowSuffix)) {
                    bestNode = node;
                    break;
                }
            }
        }
        if (bestNode != InvalidRigNode) {
            outMap.nodeIndices[semanticIndex] = bestNode;
            claimed[bestNode]                 = true;
            ++coreMapped;
        }
    }

    // Discover secondary chains from hierarchy rather than relying on glTF
    // storage order (exporters may emit depth-first or breadth-first nodes).
    std::array<bool, kMaxRigNodes> hairCandidate {};
    for (RigNodeIndex node = 0; node < outMap.nodeCount; ++node) {
        const CanonicalName canonical = Canonicalize(std::string_view(prefab.nodes[node].name));
        hairCandidate[node]           = !claimed[node] && IsHairNode(canonical.View());
    }
    // A named hair/strand root often has generically named child joints. Mark
    // those descendants as candidates too.
    for (size_t pass = 0; pass < outMap.nodeCount; ++pass) {
        bool changed = false;
        for (RigNodeIndex node = 0; node < outMap.nodeCount; ++node) {
            const RigNodeIndex parent = outMap.parentIndices[node];
            if (!claimed[node] && !hairCandidate[node] && IsValidRigNode(parent, outMap.nodeCount) && hairCandidate[parent]) {
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
    for (RigNodeIndex node = 0; node < outMap.nodeCount; ++node) {
        if (!hairCandidate[node] || claimed[node]) {
            continue;
        }
        const HairAddress address = ParseHairAddress(std::string_view(prefab.nodes[node].name));
        if (!address.IsValid()) {
            continue;
        }

        const size_t slot = address.strand * HairStrandsComponent::kLinksPerStrand + address.link;
        if (hairSlotClaimed[slot]) {
            continue;
        }
        const size_t semanticIndex        = BoneSlot(CharacterBone::HairStart) + slot;
        outMap.nodeIndices[semanticIndex] = node;
        hairSlotClaimed[slot]             = true;
        hairClaimed[node]                 = true;
        claimed[node]                     = true;
    }

    // Fallback for rigs that name only the strand roots: assign each hierarchy
    // chain to the next entirely unmapped strand.
    size_t nextFallbackStrand = 0;
    for (RigNodeIndex root = 0; root < outMap.nodeCount && nextFallbackStrand < HairStrandsComponent::kStrandCount; ++root) {
        const RigNodeIndex parent       = outMap.parentIndices[root];
        const bool         parentIsHair = IsValidRigNode(parent, outMap.nodeCount) && hairCandidate[parent];
        if (!hairCandidate[root] || hairClaimed[root] || parentIsHair) {
            continue;
        }
        while (nextFallbackStrand < HairStrandsComponent::kStrandCount && hairSlotClaimed[nextFallbackStrand * HairStrandsComponent::kLinksPerStrand]) {
            ++nextFallbackStrand;
        }
        if (nextFallbackStrand >= HairStrandsComponent::kStrandCount) {
            break;
        }

        RigNodeIndex current = root;
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand && IsValidRigNode(current, outMap.nodeCount); ++link) {
            const size_t slot                 = nextFallbackStrand * HairStrandsComponent::kLinksPerStrand + link;
            const size_t semanticIndex        = BoneSlot(CharacterBone::HairStart) + slot;
            outMap.nodeIndices[semanticIndex] = current;
            hairSlotClaimed[slot]             = true;
            claimed[current]                  = true;
            hairClaimed[current]              = true;

            RigNodeIndex next = InvalidRigNode;
            for (RigNodeIndex node = 0; node < outMap.nodeCount; ++node) {
                if (hairCandidate[node] && !hairClaimed[node] && outMap.parentIndices[node] == current) {
                    next = node;
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
    for (RigNodeIndex node = 0; node < outMap.nodeCount && freeHairSlot < HairStrandsComponent::kTotalParticles; ++node) {
        if (!hairCandidate[node] || hairClaimed[node]) {
            continue;
        }
        while (freeHairSlot < HairStrandsComponent::kTotalParticles && hairSlotClaimed[freeHairSlot]) {
            ++freeHairSlot;
        }
        if (freeHairSlot >= HairStrandsComponent::kTotalParticles) {
            break;
        }
        outMap.nodeIndices[BoneSlot(CharacterBone::HairStart) + freeHairSlot] = node;
        hairSlotClaimed[freeHairSlot]                                         = true;
        claimed[node]                                                         = true;
        hairClaimed[node]                                                     = true;
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
        const RigNodeIndex node = outMap.nodeIndices[semanticIndex];
        if (!IsValidRigNode(node, outMap.nodeCount)) {
            continue;
        }
        for (const Joint& joint: skeleton.joints) {
            if (joint.nodeIndex >= 0 && static_cast<RigNodeIndex>(joint.nodeIndex) == node) {
                outMap.inverseBindMatrices[semanticIndex] = joint.inverseBindMatrix;
                break;
            }
        }
    }

    ResolveForwardKinematics(outMap);
    ConfigureHumanoidChildOfConstraints(outMap);
    outMap.initialized = true;
    outMap.poseValid   = true;
    return coreMapped == kCoreBoneCount && mappedHairStrands == HairStrandsComponent::kStrandCount;
}

void BuildStandardProceduralRig(RigBoneMap& outMap) noexcept {
    ResetRigBoneMap(outMap);
    outMap.nodeCount  = kBoneCount;
    outMap.jointCount = static_cast<uint32_t>(kBoneCount);

    auto addBone = [&](CharacterBone bone, RigNodeIndex parentNode, JPH::Vec3Arg translation) {
        const size_t semantic                = BoneSlot(bone);
        outMap.nodeIndices[semantic]         = semantic;
        outMap.parentIndices[semantic]       = parentNode;
        outMap.bindLocalTransforms[semantic] = JPH::Mat44::sTranslation(translation);
        outMap.localTransforms[semantic]     = outMap.bindLocalTransforms[semantic];
    };

    addBone(CharacterBone::Root, InvalidRigNode, JPH::Vec3::sZero());
    addBone(CharacterBone::Hips, BoneSlot(CharacterBone::Root), JPH::Vec3(0.0f, 1.0f, 0.0f));
    addBone(CharacterBone::Spine, BoneSlot(CharacterBone::Hips), JPH::Vec3(0.0f, 0.15f, 0.0f));
    addBone(CharacterBone::SupSpine, BoneSlot(CharacterBone::Spine), JPH::Vec3(0.0f, 0.16f, 0.0f));
    addBone(CharacterBone::Chest, BoneSlot(CharacterBone::SupSpine), JPH::Vec3(0.0f, 0.17f, 0.0f));
    addBone(CharacterBone::Neck, BoneSlot(CharacterBone::Chest), JPH::Vec3(0.0f, 0.16f, 0.0f));
    addBone(CharacterBone::Head, BoneSlot(CharacterBone::Neck), JPH::Vec3(0.0f, 0.12f, 0.0f));

    addBone(CharacterBone::UpperArmL, BoneSlot(CharacterBone::Chest), JPH::Vec3(0.21f, 0.10f, 0.0f));
    addBone(CharacterBone::ForearmL, BoneSlot(CharacterBone::UpperArmL), JPH::Vec3(0.28f, 0.0f, 0.0f));
    addBone(CharacterBone::HandL, BoneSlot(CharacterBone::ForearmL), JPH::Vec3(0.24f, 0.0f, 0.0f));
    addBone(CharacterBone::UpperArmR, BoneSlot(CharacterBone::Chest), JPH::Vec3(-0.21f, 0.10f, 0.0f));
    addBone(CharacterBone::ForearmR, BoneSlot(CharacterBone::UpperArmR), JPH::Vec3(-0.28f, 0.0f, 0.0f));
    addBone(CharacterBone::HandR, BoneSlot(CharacterBone::ForearmR), JPH::Vec3(-0.24f, 0.0f, 0.0f));

    addBone(CharacterBone::ThighL, BoneSlot(CharacterBone::Hips), JPH::Vec3(0.12f, -0.06f, 0.0f));
    addBone(CharacterBone::ShinL, BoneSlot(CharacterBone::ThighL), JPH::Vec3(0.0f, -0.43f, 0.06f));
    addBone(CharacterBone::FootL, BoneSlot(CharacterBone::ShinL), JPH::Vec3(0.0f, -0.42f, -0.06f));
    addBone(CharacterBone::ToeL, BoneSlot(CharacterBone::FootL), JPH::Vec3(0.0f, -0.045f, 0.18f));
    addBone(CharacterBone::ThighR, BoneSlot(CharacterBone::Hips), JPH::Vec3(-0.12f, -0.06f, 0.0f));
    addBone(CharacterBone::ShinR, BoneSlot(CharacterBone::ThighR), JPH::Vec3(0.0f, -0.43f, 0.06f));
    addBone(CharacterBone::FootR, BoneSlot(CharacterBone::ShinR), JPH::Vec3(0.0f, -0.42f, -0.06f));
    addBone(CharacterBone::ToeR, BoneSlot(CharacterBone::FootR), JPH::Vec3(0.0f, -0.045f, 0.18f));

    constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        const float angle = kTwoPi * static_cast<float>(strand) / static_cast<float>(HairStrandsComponent::kStrandCount);
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t particle          = strand * HairStrandsComponent::kLinksPerStrand + link;
            const size_t semantic          = BoneSlot(CharacterBone::HairStart) + particle;
            outMap.nodeIndices[semantic]   = semantic;
            outMap.parentIndices[semantic] = (link == 0) ? BoneSlot(CharacterBone::Head) : semantic - 1;
            const JPH::Vec3 translation = (link == 0) ? JPH::Vec3(std::cos(angle) * 0.105f, 0.17f, std::sin(angle) * 0.105f) : JPH::Vec3(0.0f, -0.105f, 0.0f);
            outMap.bindLocalTransforms[semantic] = JPH::Mat44::sTranslation(translation);
            outMap.localTransforms[semantic]     = outMap.bindLocalTransforms[semantic];
        }
    }

    ResolveForwardKinematics(outMap);
    for (size_t semantic = 0; semantic < kBoneCount; ++semantic) {
        const RigNodeIndex node = outMap.nodeIndices[semantic];
        if (IsValidRigNode(node, outMap.nodeCount)) {
            outMap.inverseBindMatrices[semantic] = outMap.modelTransforms[node].Inversed();
        }
    }
    outMap.initialized = true;
    outMap.poseValid   = true;
}

void ProceduralAnimation::Register(Engine& engine) {
    auto& registry = engine.GetRegistry();
    registry.RegisterComponent<ProceduralLocomotionComponent>("ProceduralLocomotionComponent");
    registry.RegisterComponent<HairStrandsComponent>("HairStrandsComponent");
    registry.RegisterComponent<ProceduralLookAtComponent>("ProceduralLookAtComponent");
    registry.RegisterComponent<ProceduralAnimationConfigComponent>("ProceduralAnimationConfigComponent");
    registry.RegisterComponent<ProceduralLocomotionTracksComponent>("ProceduralLocomotionTracksComponent");
    registry.RegisterComponent<RigBoneMap>("RigBoneMap");

    using namespace ECS;
    auto&      graph    = engine.GetUpdateGraph();
    const bool inserted = graph.AddSystemBefore(
        {
            .update_func = [](Engine& target, float dt) { ProceduralAnimation::Update(target, dt); },
            .name        = "ProceduralAnimationSystem",
            .access_pattern =
                {
                    Read<Components::PhysicsComponent>(),
                    Write<Components::TransformComponent>(),
                    Read<Components::HierarchyComponent>(),
                    Read<Components::MeshComponent>(),
                    Read<ProceduralLookAtComponent>(),
                    Read<ProceduralAnimationConfigComponent>(),
                    Read<Components::SkeletalMeshComponent>(),
                    Write<ProceduralLocomotionComponent>(),
                    Write<ProceduralLocomotionTracksComponent>(),
                    Write<HairStrandsComponent>(),
                    Write<RigBoneMap>(),
                    Write<Components::KinematicPoseOverrideComponent>(),
                },
            .enabled = true,
        },
        "ArticulationSystem"
    );
    if (inserted) {
        graph.Compile();
        ZHLN::Log("[ProceduralAnimation] Registered optional subsystem before ArticulationSystem.");
    }
}

void ProceduralAnimation::ResolveModelTransforms(RigBoneMap& boneMap) noexcept {
    ResolveForwardKinematics(boneMap);
}

void ProceduralAnimation::CaptureChildOfPoseDeltas(RigBoneMap& boneMap) noexcept {
    for (size_t index = 0; index < boneMap.childOfConstraintCount; ++index) {
        RigChildOfConstraint& constraint = boneMap.childOfConstraints[index];
        if (!IsValidRigNode(constraint.child, boneMap.nodeCount)) {
            continue;
        }
        constraint.localPoseDelta = boneMap.bindLocalTransforms[constraint.child].Inversed() * boneMap.localTransforms[constraint.child];
    }
}

size_t ProceduralAnimation::ApplyChildOfConstraints(RigBoneMap& boneMap, bool applyHands, bool applyChest) noexcept {
    size_t appliedCount = 0;
    for (size_t index = 0; index < boneMap.childOfConstraintCount; ++index) {
        const RigChildOfConstraint& constraint = boneMap.childOfConstraints[index];
        if ((constraint.kind == RigChildOfKind::Hand && !applyHands) || (constraint.kind == RigChildOfKind::Chest && !applyChest)) {
            continue;
        }
        if (!IsValidRigNode(constraint.parent, boneMap.nodeCount) || !IsValidRigNode(constraint.child, boneMap.nodeCount)) {
            continue;
        }

        const JPH::Mat44 previousChild    = boneMap.modelTransforms[constraint.child];
        const JPH::Mat44 constrainedChild = boneMap.modelTransforms[constraint.parent] * constraint.bindRelative * constraint.localPoseDelta;
        const JPH::Mat44 modelCorrection  = constrainedChild * previousChild.Inversed();
        for (RigNodeIndex node = 0; node < boneMap.nodeCount; ++node) {
            if (IsNodeDescendant(boneMap, node, constraint.child)) {
                boneMap.modelTransforms[node] = modelCorrection * boneMap.modelTransforms[node];
            }
        }
        ++appliedCount;
    }
    return appliedCount;
}

size_t ProceduralAnimation::BuildSkinningPalette(const Skeleton& skeleton, const RigBoneMap& boneMap, std::span<JPH::Mat44> output) noexcept {
    const size_t count = std::min(skeleton.joints.size(), output.size());
    std::fill_n(output.begin(), count, JPH::Mat44::sIdentity());
    for (size_t jointIndex = 0; jointIndex < count; ++jointIndex) {
        const Joint&       joint = skeleton.joints[jointIndex];
        const RigNodeIndex node  = joint.nodeIndex >= 0 ? static_cast<RigNodeIndex>(joint.nodeIndex) : InvalidRigNode;
        if (IsValidRigNode(node, boneMap.nodeCount)) {
            output[jointIndex] = boneMap.modelTransforms[node] * joint.inverseBindMatrix;
        }
    }
    return count;
}

size_t ProceduralAnimation::SyncNonSkinnedAttachments(ECS::Registry& registry, Entity rootEntity, const RigBoneMap& boneMap) noexcept {
    size_t synchronizedCount = 0;
    for (Entity childEntity: registry.GetEntitiesWith<Components::MeshComponent>()) {
        const auto* hierarchy = registry.Get<Components::HierarchyComponent>(childEntity);
        if (hierarchy == nullptr || hierarchy->parent != rootEntity) {
            continue;
        }
        const auto* mesh = registry.Get<Components::MeshComponent>(childEntity);
        if (mesh == nullptr || mesh->nodeIndex < 0) {
            continue;
        }
        const RigNodeIndex node = static_cast<RigNodeIndex>(mesh->nodeIndex);
        if (!IsValidRigNode(node, boneMap.nodeCount)) {
            continue;
        }
        const auto* skeletalMesh = registry.Get<Components::SkeletalMeshComponent>(childEntity);
        if (skeletalMesh != nullptr && skeletalMesh->skeletonIndex >= 0) {
            continue;
        }
        if (auto* childTransform = registry.Get<Components::TransformComponent>(childEntity)) {
            const JPH::Mat44& modelTransform = boneMap.modelTransforms[node];
            childTransform->position         = modelTransform.GetTranslation();
            childTransform->rotation         = ExtractRotation(modelTransform);
            childTransform->scale            = ExtractScale(modelTransform);
            ++synchronizedCount;
        }
    }
    return synchronizedCount;
}

void ProceduralAnimation::Update(Engine& engine, float dt) noexcept {
    ZHLN::ScopedTimer timer("ECS System: Procedural Animation");

    auto& registry = engine.GetRegistry();
    auto& physics  = engine.GetPhysicsContext();
    auto& renderer = engine.GetRenderContext();

    for (Entity entity: registry.GetEntitiesWith<ProceduralLocomotionComponent>()) {
        auto* gait             = registry.Get<ProceduralLocomotionComponent>(entity);
        auto* hair             = registry.Get<HairStrandsComponent>(entity);
        auto* config           = registry.Get<ProceduralAnimationConfigComponent>(entity);
        auto* tracks           = registry.Get<ProceduralLocomotionTracksComponent>(entity);
        auto* movement         = registry.Get<Components::MovementComponent>(entity);
        auto* transform        = registry.Get<Components::TransformComponent>(entity);
        auto* physicsComponent = registry.Get<Components::PhysicsComponent>(entity);
        auto* poseOverride     = registry.Get<Components::KinematicPoseOverrideComponent>(entity);
        auto* boneMap          = registry.Get<RigBoneMap>(entity);

        // Articulation consumes only the generic core pose hook. Guarantee that
        // a procedural ragdoll publishes through that hook even when spawn code
        // forgot to attach it explicitly.
        if (poseOverride == nullptr && registry.Get<Components::RagdollComponent>(entity) != nullptr) {
            poseOverride = &registry.Add(entity, Components::KinematicPoseOverrideComponent {});
            ZHLN::Log("[ProceduralAnimation] Added missing KinematicPoseOverrideComponent to ragdoll entity {}.", entity.index);
        }
        if (poseOverride != nullptr) {
            poseOverride->valid = false;
        }
        if (gait == nullptr || transform == nullptr || boneMap == nullptr) {
            continue;
        }

        auto*              animator = registry.Get<Components::AnimatorComponent>(entity);
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
                mappedCoreBones += IsValidRigNode(boneMap->nodeIndices[semantic], boneMap->nodeCount) ? 1u : 0u;
            }
            size_t mappedHairBones   = 0;
            size_t mappedHairStrands = 0;
            for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
                bool strandMapped = false;
                for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t semantic = BoneSlot(CharacterBone::HairStart) + strand * HairStrandsComponent::kLinksPerStrand + link;
                    if (IsValidRigNode(boneMap->nodeIndices[semantic], boneMap->nodeCount)) {
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
            if (boneMap->childOfConstraintCount > 0) {
                ZHLN::Log("[ProceduralAnimation] Added {} semantic child-of constraints for detached hands/chest.", boneMap->childOfConstraintCount);
            }
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
                    if (!IsValidRigNode(boneMap->nodeIndices[semantic], boneMap->nodeCount)) {
                        ZHLN::Log("[ProceduralAnimation] Missing core mapping: {}", kCoreBoneLabels[semantic]);
                    }
                }
                for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
                    const size_t rootSemantic = BoneSlot(CharacterBone::HairStart) + strand * HairStrandsComponent::kLinksPerStrand;
                    bool         strandMapped = false;
                    for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                        strandMapped = strandMapped || IsValidRigNode(boneMap->nodeIndices[rootSemantic + link], boneMap->nodeCount);
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
        const bool authoredPoseOnly       = config != nullptr && config->authoredPoseOnly;
        const bool gaitEnabled            = !authoredPoseOnly && (config == nullptr || config->enableGait);
        const bool gravityBounceEnabled   = gaitEnabled && (config == nullptr || config->enableGravityBounce);
        const bool ikEnabled              = !authoredPoseOnly && (config == nullptr || config->enableLegIK);
        const bool accelerationEnabled    = !authoredPoseOnly && (config == nullptr || config->enableAccelerationTilt);
        const bool upperBodyEnabled       = !authoredPoseOnly && (config == nullptr || config->enableUpperBody);
        const bool secondaryMotionEnabled = !authoredPoseOnly && (config == nullptr || config->enableSecondaryMotion);
        const bool handChildOfEnabled     = config == nullptr || config->enforceHandChildOf;
        const bool chestChildOfEnabled    = config == nullptr || config->enforceChestChildOf;
        const bool childOfEnabled         = handChildOfEnabled || chestChildOfEnabled;
        const bool locomotionSyncEnabled  = animator != nullptr && tracks != nullptr && tracks->synchronizeToStrideWheel;

        const JPH::Vec3 velocityWorld   = physicsComponent != nullptr ? physics.GetCharacterVelocity(physicsComponent->physicsHandle) : JPH::Vec3::sZero();
        const JPH::Quat rootRotation    = transform->rotation.Normalized();
        const JPH::Vec3 velocityLocal   = rootRotation.Inversed() * velocityWorld;
        const float     horizontalSpeed = std::sqrt(velocityLocal.GetX() * velocityLocal.GetX() + velocityLocal.GetZ() * velocityLocal.GetZ());

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

        // Advance the stride wheel before sampling locomotion clips so the two
        // authored reach keys and their interpolated pass pose stay phase locked.
        if (gaitEnabled || locomotionSyncEnabled) {
            Animation::EvaluateGait(*gait, velocityLocal, angularVelocity, dt);
        }
        if (animator != nullptr && tracks != nullptr) {
            SynchronizeLocomotionTrack(*animator, *tracks, movement, *gait, horizontalSpeed);
        }

        ApplyAuthoredPose(animator, config, *boneMap, dt);
        if (childOfEnabled) {
            ProceduralAnimation::CaptureChildOfPoseDeltas(*boneMap);
        }
        ResolveForwardKinematics(*boneMap);

        // Stages 1-2: apply optional gait and whole-body COM layers after the
        // synchronized authored pose has been evaluated.
        if (gaitEnabled && !gravityBounceEnabled) {
            gait->gravityBounce = 0.0f;
            gait->pelvisBob     = 0.0f;
        }
        if (accelerationEnabled) {
            Animation::ApplyAccelerationTilt(*gait, boneMap->modelTransforms.data(), *boneMap);
        }

        // Stage 3: terrain contact, pelvis reach correction, and two-bone IK.
        if (ikEnabled) {
            const Entity ignoredHandle          = physicsComponent != nullptr ? physicsComponent->physicsHandle : Entity {};
            const float  legIKWeight            = config != nullptr ? config->legIKWeight : 1.0f;
            const float  pelvisDropWeight       = config != nullptr ? config->pelvisDropWeight : 1.0f;
            const float  maxHeightCorrection    = config != nullptr ? config->maxFootHeightCorrection : 0.18f;
            const bool   preserveAuthoredFootXZ = config == nullptr || config->preserveAuthoredFootXZ;
            const bool   worldLockFeet          = config != nullptr && config->worldLockFeet;
            Animation::SolveLegGrounding(
                engine, transform->position, rootRotation, *gait, boneMap->modelTransforms.data(), *boneMap, ignoredHandle, legIKWeight, preserveAuthoredFootXZ,
                worldLockFeet, maxHeightCorrection, dt, pelvisDropWeight
            );
        } else if (gaitEnabled) {
            Animation::ApplyPelvisGaitOffset(*gait, boneMap->modelTransforms.data(), *boneMap, false);
        }

        // Stage 4: authored upper-body channels win by default. Procedural arm
        // swing/look-at only fill groups not keyed by the active GLB track.
        if (upperBodyEnabled) {
            const AuthoredUpperBodyCoverage coverage      = FindAuthoredUpperBodyCoverage(animator, *boneMap);
            const bool                      forceLayering = config != nullptr && config->layerUpperBodyOverAuthoredChannels;
            const bool                      applyArmSwing = forceLayering || !coverage.arms;
            const bool                      applyLookAt   = forceLayering || !coverage.torsoHead;
            const auto*                     lookAt        = registry.Get<ProceduralLookAtComponent>(entity);
            if (applyArmSwing || applyLookAt) {
                Animation::SolveUpperBody(
                    *gait, lookAt, transform->position, rootRotation, boneMap->modelTransforms.data(), *boneMap, applyArmSwing, applyLookAt
                );
            }
        }

        // Stage 5: worker-fiber XPBD secondary motion.
        if (hair != nullptr && secondaryMotionEnabled) {
            const RigNodeIndex headNode          = boneMap->nodeIndices[BoneSlot(CharacterBone::Head)];
            const bool         hasHead           = IsValidRigNode(headNode, boneMap->nodeCount);
            const JPH::Vec3    headModelPosition = hasHead ? boneMap->modelTransforms[headNode].GetTranslation() : JPH::Vec3(0.0f, 1.60f, 0.0f);
            const JPH::Quat    headModelRotation = hasHead ? ExtractRotation(boneMap->modelTransforms[headNode]) : JPH::Quat::sIdentity();
            const JPH::Vec3    headWorldPosition = ModelToWorld(transform->position, rootRotation, headModelPosition);
            const JPH::Quat    headWorldRotation = (rootRotation * headModelRotation).Normalized();

            {
                ZHLN::ScopedTimer hairTimer("Procedural Animation: 18-Strand XPBD");
                Animation::StepHairSimulation(*hair, headWorldPosition, headWorldRotation, velocityWorld, dt);
            }
            Animation::ExtractHairBoneTransforms(*hair, headWorldRotation, boneMap->modelTransforms.data(), *boneMap);

            // The secondary solver works in world space for collision stability;
            // convert its output back into the character's model space.
            const JPH::Quat inverseRoot = rootRotation.Inversed();
            for (size_t particle = 0; particle < HairStrandsComponent::kTotalParticles; ++particle) {
                const size_t       semantic = BoneSlot(CharacterBone::HairStart) + particle;
                const RigNodeIndex node     = boneMap->nodeIndices[semantic];
                if (!IsValidRigNode(node, boneMap->nodeCount)) {
                    continue;
                }
                const JPH::Mat44 world         = boneMap->modelTransforms[node];
                const JPH::Vec3  modelPosition = inverseRoot * (world.GetTranslation() - transform->position);
                const JPH::Quat  modelRotation = (inverseRoot * ExtractRotation(world)).Normalized();
                const JPH::Vec3  worldScale(world.GetColumn3(0).Length(), world.GetColumn3(1).Length(), world.GetColumn3(2).Length());
                boneMap->modelTransforms[node] = JPH::Mat44::sRotationTranslation(modelRotation, modelPosition).PreScaled(worldScale);
            }
        } else if (hair != nullptr) {
            // Re-seed from the authored shape when secondary motion is enabled
            // again instead of resuming stale Verlet velocity.
            hair->initialized = false;
        }

        // Stage 6 is supplied by ArticulationSystem immediately after this
        // system. It consumes hit commands, decays motor stiffness, and blends
        // Jolt ragdoll poses over this kinematic target.

        // Stage 7: enforce semantic child-of relationships before recovering
        // local transforms. Detached hands carry fingers; a detached chest
        // carries neck/head and upper-body attachments with the torso.
        if (childOfEnabled) {
            ProceduralAnimation::ApplyChildOfConstraints(*boneMap, handChildOfEnabled, chestChildOfEnabled);
        }
        CaptureLocalPose(*boneMap);
        ResolveForwardKinematics(*boneMap);
        boneMap->poseValid = true;
        ++boneMap->poseVersion;
        if (poseOverride != nullptr) {
            poseOverride->valid = false;
        }

        // Stage 8: every distinct skin palette used by this character must
        // receive the evaluated node pose. A GLB may split body, feet, clothing,
        // and hair across multiple skins/joint offsets.
        std::array<uint32_t, kMaxRigNodes> uploadedOffsets {};
        uploadedOffsets.fill(std::numeric_limits<uint32_t>::max());
        size_t uploadedPaletteCount = 0;

        auto uploadSkin = [&](Components::SkeletalMeshComponent* skeletalMesh) {
            if (skeletalMesh == nullptr || skeletalMesh->skeletonIndex < 0 || skeletalMesh->skeletonIndex >= static_cast<int32_t>(prefab->skeletons.size())) {
                return;
            }
            for (size_t i = 0; i < uploadedPaletteCount; ++i) {
                if (uploadedOffsets[i] == skeletalMesh->jointOffset) {
                    return;
                }
            }
            if (uploadedPaletteCount >= uploadedOffsets.size()) {
                return;
            }

            const Skeleton&                      skeleton = prefab->skeletons[static_cast<size_t>(skeletalMesh->skeletonIndex)];
            std::array<JPH::Mat44, kMaxRigNodes> palette {};
            const size_t                         paletteCount = ProceduralAnimation::BuildSkinningPalette(skeleton, *boneMap, palette);
            renderer.UpdateJointMatrices(skeletalMesh->jointOffset, palette.data(), static_cast<uint32_t>(paletteCount));
            uploadedOffsets[uploadedPaletteCount++] = skeletalMesh->jointOffset;

            if (skeletalMesh == skin.skeletalMesh) {
                if (poseOverride != nullptr) {
                    const size_t overrideCount = std::min(paletteCount, Components::KinematicPoseOverrideComponent::MaxJoints);
                    for (size_t jointIndex = 0; jointIndex < overrideCount; ++jointIndex) {
                        const int32_t      importedNode           = skeleton.joints[jointIndex].nodeIndex;
                        const RigNodeIndex node                   = importedNode >= 0 ? static_cast<RigNodeIndex>(importedNode) : InvalidRigNode;
                        poseOverride->modelTransforms[jointIndex] = IsValidRigNode(node, boneMap->nodeCount) ? boneMap->modelTransforms[node] :
                                                                                                               JPH::Mat44::sIdentity();
                    }
                    poseOverride->jointCount  = static_cast<uint32_t>(overrideCount);
                    poseOverride->poseVersion = boneMap->poseVersion;
                    poseOverride->valid       = true;
                }
                boneMap->jointOffset   = skeletalMesh->jointOffset;
                boneMap->jointCount    = static_cast<uint32_t>(paletteCount);
                boneMap->skeletonIndex = skeletalMesh->skeletonIndex;
            }
        };

        // Upload the best semantic skeleton first so its generic motor target is
        // always published even when another part shares the same joint offset.
        uploadSkin(skin.skeletalMesh);
        uploadSkin(registry.Get<Components::SkeletalMeshComponent>(entity));
        for (Entity childEntity: registry.GetEntitiesWith<Components::SkeletalMeshComponent>()) {
            const auto* hierarchy = registry.Get<Components::HierarchyComponent>(childEntity);
            if (hierarchy != nullptr && hierarchy->parent == entity) {
                uploadSkin(registry.Get<Components::SkeletalMeshComponent>(childEntity));
            }
        }

        const size_t previousPaletteCount     = boneMap->synchronizedSkinPaletteCount;
        boneMap->synchronizedSkinPaletteCount = uploadedPaletteCount;
        if (boneMap->poseVersion == 1 || previousPaletteCount != uploadedPaletteCount) {
            ZHLN::Log("[ProceduralAnimation] Entity {} uploaded {} distinct skin palettes.", entity.index, uploadedPaletteCount);
        }

        // Non-skinned child entities are attachments, not palette consumers.
        // Publish their evaluated node TRS before core TransformSystem runs.
        const size_t previousAttachmentCount = boneMap->synchronizedAttachmentCount;
        boneMap->synchronizedAttachmentCount = ProceduralAnimation::SyncNonSkinnedAttachments(registry, entity, *boneMap);
        if (boneMap->poseVersion == 1 || previousAttachmentCount != boneMap->synchronizedAttachmentCount) {
            ZHLN::Log("[ProceduralAnimation] Entity {} synchronized {} non-skinned node attachments.", entity.index, boneMap->synchronizedAttachmentCount);
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

    const auto worldPosition = [&](RigNodeIndex node) { return ModelToWorld(rootPosition, rootRotation, boneMap.modelTransforms[node].GetTranslation()); };
    const auto drawCross     = [&](JPH::Vec3Arg point, float radius, JPH::Vec4Arg color) {
        renderContext.DrawLine(point - JPH::Vec3(radius, 0.0f, 0.0f), point + JPH::Vec3(radius, 0.0f, 0.0f), color);
        renderContext.DrawLine(point - JPH::Vec3(0.0f, radius, 0.0f), point + JPH::Vec3(0.0f, radius, 0.0f), color);
        renderContext.DrawLine(point - JPH::Vec3(0.0f, 0.0f, radius), point + JPH::Vec3(0.0f, 0.0f, radius), color);
    };

    const JPH::Vec4 torsoColor(0.15f, 0.85f, 1.00f, 1.0f);
    const JPH::Vec4 armColor(1.00f, 0.60f, 0.12f, 1.0f);
    const JPH::Vec4 legColor(0.20f, 1.00f, 0.35f, 1.0f);
    const JPH::Vec4 hairColor(0.90f, 0.25f, 1.00f, 1.0f);

    for (size_t semantic = 0; semantic < kCoreBoneCount; ++semantic) {
        const RigNodeIndex node = boneMap.nodeIndices[semantic];
        if (!IsValidRigNode(node, boneMap.nodeCount)) {
            continue;
        }
        const RigNodeIndex parent = boneMap.parentIndices[node];
        JPH::Vec4          color  = torsoColor;
        if (semantic >= BoneSlot(CharacterBone::UpperArmL) && semantic <= BoneSlot(CharacterBone::HandR)) {
            color = armColor;
        } else if (semantic >= BoneSlot(CharacterBone::ThighL)) {
            color = legColor;
        }
        if (IsValidRigNode(parent, boneMap.nodeCount)) {
            renderContext.DrawLine(worldPosition(parent), worldPosition(node), color);
        }
        drawCross(worldPosition(node), semantic == BoneSlot(CharacterBone::Head) ? 0.07f : 0.025f, color);
    }

    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        for (size_t link = 0; link + 1 < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t       particle0 = strand * HairStrandsComponent::kLinksPerStrand + link;
            const size_t       particle1 = particle0 + 1;
            const RigNodeIndex node0     = boneMap.nodeIndices[BoneSlot(CharacterBone::HairStart) + particle0];
            const RigNodeIndex node1     = boneMap.nodeIndices[BoneSlot(CharacterBone::HairStart) + particle1];
            if (IsValidRigNode(node0, boneMap.nodeCount) && IsValidRigNode(node1, boneMap.nodeCount)) {
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
        // +Z is forward and +Y is up. Forward rolling therefore rotates around
        // +X, which maps an initial +Z spoke toward -Y (clockwise when viewed
        // from the character's right side).
        for (uint32_t segment = 0; segment < 24; ++segment) {
            const float     angle0 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(segment) / 24.0f;
            const float     angle1 = 2.0f * std::numbers::pi_v<float> * static_cast<float>(segment + 1) / 24.0f;
            const JPH::Vec3 p0     = wheelCenterModel + JPH::Vec3(0.0f, -std::sin(angle0) * wheelRadius, std::cos(angle0) * wheelRadius);
            const JPH::Vec3 p1     = wheelCenterModel + JPH::Vec3(0.0f, -std::sin(angle1) * wheelRadius, std::cos(angle1) * wheelRadius);
            renderContext.DrawLine(ModelToWorld(rootPosition, rootRotation, p0), ModelToWorld(rootPosition, rootRotation, p1), wheelColor);
        }
        const JPH::Vec3 wheelMarkerModel = wheelCenterModel +
                                           JPH::Vec3(0.0f, -std::sin(gait->strideWheelAngle) * wheelRadius, std::cos(gait->strideWheelAngle) * wheelRadius);
        const JPH::Vec4 markerColor      = gait->passWeightL > gait->reachWeightL ? passColor : reachColor;
        renderContext.DrawLine(
            ModelToWorld(rootPosition, rootRotation, wheelCenterModel), ModelToWorld(rootPosition, rootRotation, wheelMarkerModel), markerColor
        );
        drawCross(ModelToWorld(rootPosition, rootRotation, wheelMarkerModel), 0.025f, markerColor);
        drawCross(ModelToWorld(rootPosition, rootRotation, wheelCenterModel + JPH::Vec3(0.0f, wheelRadius, 0.0f)), 0.018f, passColor);
        drawCross(ModelToWorld(rootPosition, rootRotation, wheelCenterModel - JPH::Vec3(0.0f, wheelRadius, 0.0f)), 0.018f, passColor);
        drawCross(ModelToWorld(rootPosition, rootRotation, wheelCenterModel + JPH::Vec3(0.0f, 0.0f, wheelRadius)), 0.018f, reachColor);
        drawCross(ModelToWorld(rootPosition, rootRotation, wheelCenterModel - JPH::Vec3(0.0f, 0.0f, wheelRadius)), 0.018f, reachColor);
    }
}

void ProceduralAnimation::DrawDebugRig(
    RenderContext&                       renderContext,
    JPH::Vec3Arg                         rootPosition,
    JPH::QuatArg                         rootRotation,
    const RigBoneMap&                    boneMap,
    const ProceduralLocomotionComponent* gait
) noexcept {
    DrawProceduralDebugRig(renderContext, rootPosition, rootRotation, boneMap, gait);
}

} // namespace ZHLN
