// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// clang-format off
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Animation/ProceduralMath.hpp>
// clang-format on
#include <Zahlen/Engine/Engine.hpp>
#include <Animation/IK.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

module ZHLN.ProceduralAnimation;

namespace ZHLN::Animation {

constexpr float kGaitTwoPi = 2.0f * std::numbers::pi_v<float>;

// Anonymous, not `namespace Detail`: this is a module implementation unit, so
// none of it is reachable from outside the module in the first place. The
// unnamed namespace gives these helpers internal linkage without inventing a
// name whose only job would be to say "private" about something already private.
namespace {

[[nodiscard]] float WrapUnit(float value) noexcept {
    value = std::fmod(value, 1.0f);
    return value < 0.0f ? value + 1.0f : value;
}

[[nodiscard]] float SmoothStep(float value) noexcept {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

void SpringScalar(float& value, float& velocity, float target, float dt, float frequency, float damping) noexcept {
    const float safeDt   = std::clamp(dt, 0.0f, 0.05f);
    const float omega    = kGaitTwoPi * std::max(frequency, 0.01f);
    const float f        = 1.0f + 2.0f * safeDt * std::max(damping, 0.0f) * omega;
    const float oo       = omega * omega;
    const float hoo      = safeDt * oo;
    const float hhoo     = safeDt * hoo;
    const float invDet   = 1.0f / (f + hhoo);
    const float oldValue = value;
    value                = (f * value + safeDt * velocity + hhoo * target) * invDet;
    velocity             = (velocity + hoo * (target - oldValue)) * invDet;
}

[[nodiscard]] bool IsDescendant(const RigBoneMap& map, RigNodeIndex node, RigNodeIndex ancestor) noexcept {
    const size_t safeNodeCount = std::min(map.nodeCount, kMaxRigNodes);
    if (!IsValidRigNode(node, safeNodeCount) || !IsValidRigNode(ancestor, safeNodeCount)) {
        return false;
    }

    RigNodeIndex cursor = node;
    for (size_t depth = 0; depth < safeNodeCount; ++depth) {
        if (!IsValidRigNode(cursor, safeNodeCount)) {
            return false;
        }
        if (cursor == ancestor) {
            return true;
        }
        cursor = map.parentIndices[cursor];
    }
    return false; // A cycle or an over-deep malformed hierarchy.
}

void TranslateSubtree(const RigBoneMap& map, Mat4x4* transforms, RigNodeIndex rootNode, FVec3 delta) noexcept {
    if (transforms == nullptr || !IsValidRigNode(rootNode, map.nodeCount)) {
        return;
    }
    for (size_t node = 0; node < map.nodeCount; ++node) {
        if (IsDescendant(map, node, rootNode)) {
            ProceduralMath::SetTranslation(transforms[node], ProceduralMath::Translation(transforms[node]) + delta);
        }
    }
}

[[nodiscard]] FVec3 MatrixScale(const Mat4x4& matrix) noexcept {
    return FVec3(ProceduralMath::Column3(matrix, 0).Length(), ProceduralMath::Column3(matrix, 1).Length(), ProceduralMath::Column3(matrix, 2).Length());
}

[[nodiscard]] Quat MatrixRotation(const Mat4x4& matrix) noexcept {
    const FVec3 scale = MatrixScale(matrix);
    const FVec3 x     = scale.x > 1.0e-6f ? ProceduralMath::Column3(matrix, 0) / scale.x : FVec3::UnitX();
    const FVec3 y     = scale.y > 1.0e-6f ? ProceduralMath::Column3(matrix, 1) / scale.y : FVec3::UnitY();
    const FVec3 z     = scale.z > 1.0e-6f ? ProceduralMath::Column3(matrix, 2) / scale.z : FVec3::UnitZ();
    return ProceduralMath::Rotation(Mat4x4::FromCols(FVec4(x, 0.0f), FVec4(y, 0.0f), FVec4(z, 0.0f), FVec4(0.0f, 0.0f, 0.0f, 1.0f)));
}

[[nodiscard]] Mat4x4 BlendTransform(const Mat4x4& authored, const Mat4x4& solved, float weight) noexcept {
    const float     easedWeight   = SmoothStep(weight);
    const FVec3 translation   = ProceduralMath::Translation(authored) + (ProceduralMath::Translation(solved) - ProceduralMath::Translation(authored)) * easedWeight;
    const FVec3 authoredScale = MatrixScale(authored);
    const FVec3 solvedScale   = MatrixScale(solved);
    const FVec3 scale         = authoredScale + (solvedScale - authoredScale) * easedWeight;
    const Quat rotation      = Slerp(MatrixRotation(authored), MatrixRotation(solved), easedWeight).Normalized();
    return ProceduralMath::Transform(rotation, translation, scale);
}

void RotateSubtreeAroundPivot(const RigBoneMap& map, Mat4x4* transforms, RigNodeIndex rootNode, FVec3 pivot, Quat rotation) noexcept {
    if (transforms == nullptr || !IsValidRigNode(rootNode, map.nodeCount)) {
        return;
    }

    for (size_t node = 0; node < map.nodeCount; ++node) {
        if (!IsDescendant(map, node, rootNode)) {
            continue;
        }

        const FVec3 oldPosition = ProceduralMath::Translation(transforms[node]);
        const FVec3 oldScale    = MatrixScale(transforms[node]);
        const FVec3 newPosition = pivot + rotation * (oldPosition - pivot);
        const Quat oldRotation = MatrixRotation(transforms[node]);
        transforms[node]            = ProceduralMath::Transform((rotation * oldRotation).Normalized(), newPosition, oldScale);
    }
}

void RotateSubtree(const RigBoneMap& map, Mat4x4* transforms, RigNodeIndex rootNode, Quat rotation) noexcept {
    if (transforms == nullptr || !IsValidRigNode(rootNode, map.nodeCount)) {
        return;
    }
    RotateSubtreeAroundPivot(map, transforms, rootNode, ProceduralMath::Translation(transforms[rootNode]), rotation);
}

[[nodiscard]] RigNodeIndex Node(const RigBoneMap& map, CharacterBone bone) noexcept {
    return map.nodeIndices[BoneSlot(bone)];
}

[[nodiscard]] FVec3 SafeNormalized(FVec3 value, FVec3 fallback) noexcept {
    return value.LengthSquared() > 1.0e-8f ? value.Normalized() : FVec3(fallback);
}

} // namespace

/**
 * Smooth cosine pelvis bounce between support contacts. The unconstrained
 * amplitude is chosen so acceleration at the apex equals -bounceGravity. Since
 * the support interval shrinks with speed, fast motion naturally gets flatter
 * without changing the gravity parameter.
 */
float EvaluateGravityBounce(const ProceduralLocomotionComponent& gait, float speed) noexcept {
    if (speed < 0.035f || gait.bounceGravity <= 0.0f || gait.maxBounceHeight <= 0.0f) {
        return 0.0f;
    }

    const float supportInterval  = gait.strideLength / (2.0f * std::max(speed, 0.01f));
    const float gravityAmplitude = gait.bounceGravity * supportInterval * supportInterval / (2.0f * std::numbers::pi_v<float> * std::numbers::pi_v<float>);
    const float amplitude        = std::min(gravityAmplitude, gait.maxBounceHeight);
    const float supportPhase     = WrapUnit(gait.phase * 2.0f);
    return 0.5f * amplitude * (1.0f - std::cos(kGaitTwoPi * supportPhase));
}

/** Maps one stride-wheel revolution onto two opposing authored reach keys. */
float EvaluateTwoKeyPosePhase(float stridePhase) noexcept {
    const float phase = WrapUnit(stridePhase);
    return phase <= 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
}

/**
 * Smoothly interpolates all tunable gait parameters from currentPreset toward
 * targetPreset. Called once per frame before EvaluateGait so the stride clock,
 * foot trajectories, bounce, sway, and arm swing all see the blended values.
 * The blend uses a critically-damped exponential approach so transitions feel
 * natural at any framerate and never overshoot.
 */
void BlendGaitParameters(ProceduralLocomotionComponent& gait, float dt) noexcept {
    // Advance the blend weight toward 1.0 at the configured speed.
    const float safeDt     = std::clamp(dt, 0.0f, 0.05f);
    const float blendSpeed = std::max(gait.gaitBlendSpeed, 0.01f);
    const float blendDelta = safeDt * blendSpeed;
    gait.gaitBlendWeight   = std::clamp(gait.gaitBlendWeight + blendDelta, 0.0f, 1.0f);

    // Use smoothstep for a more natural ease-in/ease-out curve.
    const float weight = SmoothStep(gait.gaitBlendWeight);

    // Interpolate all gait parameters.
    gait.strideLength     = gait.currentPreset.strideLength + (gait.targetPreset.strideLength - gait.currentPreset.strideLength) * weight;
    gait.stepHeight       = gait.currentPreset.stepHeight + (gait.targetPreset.stepHeight - gait.currentPreset.stepHeight) * weight;
    gait.maxBounceHeight  = gait.currentPreset.maxBounceHeight + (gait.targetPreset.maxBounceHeight - gait.currentPreset.maxBounceHeight) * weight;
    gait.bounceGravity    = gait.currentPreset.bounceGravity + (gait.targetPreset.bounceGravity - gait.currentPreset.bounceGravity) * weight;
    gait.pelvisSwayScale  = gait.currentPreset.pelvisSwayScale + (gait.targetPreset.pelvisSwayScale - gait.currentPreset.pelvisSwayScale) * weight;
    gait.armSwingScale    = gait.currentPreset.armSwingScale + (gait.targetPreset.armSwingScale - gait.currentPreset.armSwingScale) * weight;
    gait.forwardLeanScale = gait.currentPreset.forwardLeanScale + (gait.targetPreset.forwardLeanScale - gait.currentPreset.forwardLeanScale) * weight;
    gait.lateralBankScale = gait.currentPreset.lateralBankScale + (gait.targetPreset.lateralBankScale - gait.currentPreset.lateralBankScale) * weight;

    // When the blend completes, snap current to target so the next transition
    // starts from the correct baseline.
    if (gait.gaitBlendWeight >= 1.0f) {
        gait.currentPreset   = gait.targetPreset;
        gait.gaitBlendWeight = 0.0f;
    }
}

/**
 * Stage 1 + 2: extract directional acceleration, advance the distance-driven
 * stride clock, and evaluate alternating cubic/parabolic foot trajectories.
 * Velocity is expected in character-local space.
 */
void EvaluateGait(ProceduralLocomotionComponent& gait, FVec3 velocity, float angularVelocity, float dt) noexcept {
    const float safeDt           = std::max(dt, 0.0001f);
    gait.directionalAcceleration = (velocity - gait.previousVelocity) / safeDt;
    gait.previousVelocity        = velocity;
    gait.turnRate                = angularVelocity;

    const float speed     = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
    const float cycleRate = speed / std::max(gait.strideLength, 0.01f);
    gait.phase            = WrapUnit(gait.phase + cycleRate * std::max(dt, 0.0f));
    gait.strideWheelAngle = gait.phase * kGaitTwoPi;

    auto evaluateFoot = [&](float phaseOffset, FVec3& target, float& plantWeight, float& passWeight, float& reachWeight) {
        const float p       = WrapUnit(gait.phase + phaseOffset);
        const bool  isSwing = p < 0.5f;
        const float t       = isSwing ? p * 2.0f : (p - 0.5f) * 2.0f;
        const float smoothT = t * t * (3.0f - 2.0f * t);

        // During swing the foot travels heel-to-toe. The stance trajectory is
        // only a prediction; SolveLegGrounding replaces it with a world lock.
        const float z = isSwing ? (smoothT - 0.5f) * gait.strideLength : (0.5f - t) * gait.strideLength;
        const float y = isSwing ? 4.0f * t * (1.0f - t) * gait.stepHeight : 0.0f;

        target.y = y;
        target.z = z;
        if (isSwing) {
            plantWeight = 0.0f;
        } else {
            constexpr float kContactBlendFraction = 0.12f;
            const float     fadeIn                = SmoothStep(t / kContactBlendFraction);
            const float     fadeOut               = SmoothStep((1.0f - t) / kContactBlendFraction);
            plantWeight                           = fadeIn * fadeOut;
        }

        const float wheelPass  = std::sin(kGaitTwoPi * p);
        const float wheelReach = std::cos(kGaitTwoPi * p);
        passWeight             = wheelPass * wheelPass;
        reachWeight            = wheelReach * wheelReach;
    };

    // The stride solve writes addresses, so it runs on Jolt locals and the
    // component is stored once at the end -- a member converts on assignment,
    // not on binding to a FVec3&.
    FVec3 footTargetL = gait.localFootTargetL;
    FVec3 footTargetR = gait.localFootTargetR;
    evaluateFoot(0.0f, footTargetL, gait.plantWeightL, gait.passWeightL, gait.reachWeightL);
    evaluateFoot(0.5f, footTargetR, gait.plantWeightR, gait.passWeightR, gait.reachWeightR);
    footTargetL.x = 0.14f;
    footTargetR.x = -0.14f;

    if (speed < 0.035f) {
        footTargetL.z = 0.0f;
        footTargetR.z = 0.0f;
        footTargetL.y = 0.0f;
        footTargetR.y = 0.0f;
        gait.plantWeightL  = 1.0f;
        gait.plantWeightR  = 1.0f;
        gait.gravityBounce = 0.0f;
        gait.pelvisBob     = 0.0f;
        gait.pelvisSway    = 0.0f;
    } else {
        gait.gravityBounce = EvaluateGravityBounce(gait, speed);
        gait.pelvisBob     = gait.gravityBounce;
        // Completely disable lateral sway for now to diagnose waddling issue.
        // Human walking has almost no lateral translation - only vertical bob
        // and pelvis rotation.
        gait.pelvisSway = 0.0f;
    }

    gait.localFootTargetL = footTargetL;
    gait.localFootTargetR = footTargetR;

    const float targetForwardLean = std::clamp(-gait.directionalAcceleration.z * 0.018f * gait.forwardLeanScale, -0.22f, 0.22f);
    const float centripetal       = speed * angularVelocity;
    const float targetLateralBank = std::clamp((gait.directionalAcceleration.x * 0.008f - centripetal * 0.018f) * gait.lateralBankScale, -0.28f, 0.28f);
    SpringScalar(gait.forwardLean, gait.tiltPitchVelocity, targetForwardLean, dt, 5.5f, 0.88f);
    SpringScalar(gait.lateralBank, gait.tiltRollVelocity, targetLateralBank, dt, 5.5f, 0.88f);
}

void EvaluateGait(ProceduralLocomotionComponent& gait, FVec3 velocity, float dt) noexcept {
    EvaluateGait(gait, velocity, 0.0f, dt);
}

/** Rotates the upper body around an estimated center of mass. The legs are
 *  excluded so planted feet stay world-locked. */
void ApplyAccelerationTilt(ProceduralLocomotionComponent& gait, Mat4x4* nodeTransforms, const RigBoneMap& map) noexcept {
    if (nodeTransforms == nullptr || map.nodeCount == 0) {
        return;
    }

    const RigNodeIndex hipsNode  = Node(map, CharacterBone::Hips);
    const RigNodeIndex spineNode = Node(map, CharacterBone::Spine);
    const RigNodeIndex chestNode = Node(map, CharacterBone::Chest);
    if (!IsValidRigNode(hipsNode, map.nodeCount)) {
        return;
    }

    const FVec3 hipsPosition  = ProceduralMath::Translation(nodeTransforms[hipsNode]);
    const FVec3 chestPosition = IsValidRigNode(chestNode, map.nodeCount) ? ProceduralMath::Translation(nodeTransforms[chestNode]) :
                                                                               hipsPosition + FVec3(0.0f, 0.35f, 0.0f);
    // Approximate the humanoid COM from pelvis and torso masses. This is a
    // physical weighted center, not an animation interpolation.
    gait.centerOfMassModel = (hipsPosition * 0.68f + chestPosition * 0.32f);

    const Quat pitch = Quat::FromAxisAngle(FVec3::UnitX(), gait.forwardLean);
    const Quat roll  = Quat::FromAxisAngle(FVec3::UnitZ(), gait.lateralBank);
    // Rotate from spine so legs are not displaced.
    const RigNodeIndex tiltRoot = IsValidRigNode(spineNode, map.nodeCount) ? spineNode : hipsNode;
    RotateSubtreeAroundPivot(map, nodeTransforms, tiltRoot, gait.centerOfMassModel, (pitch * roll).Normalized());
}

Mat4x4 CorrectBoneDirection(
    const Mat4x4& authoredTransform,
    FVec3      currentDirection,
    FVec3      solvedDirection,
    FVec3      solvedPosition
) noexcept {
    const Quat correction =
        FromTo(SafeNormalized(currentDirection, FVec3(0.0f, -1.0f, 0.0f)), SafeNormalized(solvedDirection, FVec3(0.0f, -1.0f, 0.0f)));
    const Quat rotation = (correction * MatrixRotation(authoredTransform)).Normalized();
    return ProceduralMath::Transform(rotation, solvedPosition, MatrixScale(authoredTransform));
}

FVec3 LimitGroundNormal(FVec3 modelNormal, float maxSidewaysRadians, float maxForwardRadians) noexcept {
    const FVec3 normal = SafeNormalized(modelNormal, FVec3::UnitY());
    // Decompose the normal into forward ankle pitch (around X) and sideways
    // ankle roll (around Z). Reconstructing after independent clamps avoids the
    // unrestricted sFromTo correction folding a foot sharply onto its side.
    const float     pitch         = std::clamp(std::asin(std::clamp(normal.z, -1.0f, 1.0f)), -std::abs(maxForwardRadians), std::abs(maxForwardRadians));
    const float     roll          = std::clamp(std::atan2(-normal.x, normal.y), -std::abs(maxSidewaysRadians), std::abs(maxSidewaysRadians));
    const Quat pitchRotation = Quat::FromAxisAngle(FVec3::UnitX(), pitch);
    const Quat rollRotation  = Quat::FromAxisAngle(FVec3::UnitZ(), roll);
    return SafeNormalized((rollRotation * pitchRotation) * FVec3::UnitY(), FVec3::UnitY());
}

Mat4x4 AlignFootToGround(
    const Mat4x4& authoredFoot,
    FVec3      target,
    FVec3      modelNormal,
    float             maxSidewaysRadians,
    float             maxForwardRadians
) noexcept {
    const FVec3 limitedNormal = LimitGroundNormal(modelNormal, maxSidewaysRadians, maxForwardRadians);
    const Quat correction    = FromTo(FVec3::UnitY(), limitedNormal);
    const Quat rotation      = (correction * MatrixRotation(authoredFoot)).Normalized();
    return ProceduralMath::Transform(rotation, target, MatrixScale(authoredFoot));
}

size_t SetModelTransformAndCarrySubtree(Mat4x4* nodeTransforms, const RigBoneMap& map, RigNodeIndex rootNode, const Mat4x4& target) noexcept {
    if (nodeTransforms == nullptr || !IsValidRigNode(rootNode, map.nodeCount)) {
        return 0;
    }

    // Model-space procedural solvers replace a semantic control directly. Its
    // imported child transform nodes were evaluated before that replacement,
    // so carry the same correction through the complete subtree immediately.
    const Mat4x4 correction = target * Inverse(nodeTransforms[rootNode]);
    size_t           carried    = 0;
    for (RigNodeIndex node = 0; node < map.nodeCount; ++node) {
        if (IsDescendant(map, node, rootNode)) {
            nodeTransforms[node] = correction * nodeTransforms[node];
            ++carried;
        }
    }
    return carried;
}

void ApplyIKReachTilt(
    ProceduralLocomotionComponent& gait,
    Mat4x4*                    nodeTransforms,
    const RigBoneMap&              map,
    FVec3                   targetL,
    FVec3                   targetR,
    float                          weightL,
    float                          weightR,
    float                          maxLegExtension,
    float                          maxBodyTiltRadians,
    float                          dt
) noexcept {
    if (nodeTransforms == nullptr || map.nodeCount == 0) {
        return;
    }

    const RigNodeIndex hipsNode = Node(map, CharacterBone::Hips);
    if (!IsValidRigNode(hipsNode, map.nodeCount)) {
        return;
    }

    FVec3 requestedShift = FVec3::Zero();
    FVec3 supportPivot   = FVec3::Zero();
    float     supportWeight  = 0.0f;
    auto      accumulateLeg  = [&](CharacterBone thighBone, CharacterBone shinBone, CharacterBone footBone, FVec3 target, float rawWeight) {
        const float weight = SmoothStep(std::clamp(rawWeight, 0.0f, 1.0f));
        if (weight <= 0.001f) {
            return;
        }
        const RigNodeIndex thighNode = Node(map, thighBone);
        const RigNodeIndex shinNode  = Node(map, shinBone);
        const RigNodeIndex footNode  = Node(map, footBone);
        if (!IsValidRigNode(thighNode, map.nodeCount) || !IsValidRigNode(shinNode, map.nodeCount) || !IsValidRigNode(footNode, map.nodeCount)) {
            return;
        }

        supportPivot += target * weight;
        supportWeight += weight;
        const FVec3 thighPosition = ProceduralMath::Translation(nodeTransforms[thighNode]);
        const float     upperLength   = (ProceduralMath::Translation(nodeTransforms[shinNode]) - thighPosition).Length();
        const float     lowerLength   = (ProceduralMath::Translation(nodeTransforms[footNode]) - ProceduralMath::Translation(nodeTransforms[shinNode])).Length();
        const float     maxReach      = (upperLength + lowerLength) * std::clamp(maxLegExtension, 0.50f, 0.999f);
        const float     excess        = std::max((target - thighPosition).Length() - maxReach, 0.0f);
        FVec3       horizontal    = target - thighPosition;
        horizontal.y = 0.0f;
        if (excess > 0.0f && horizontal.LengthSquared() > 1.0e-6f) {
            requestedShift += horizontal.Normalized() * (excess * weight);
        }
    };

    accumulateLeg(CharacterBone::ThighL, CharacterBone::ShinL, CharacterBone::FootL, targetL, weightL);
    accumulateLeg(CharacterBone::ThighR, CharacterBone::ShinR, CharacterBone::FootR, targetR, weightR);

    if (supportWeight > 0.001f) {
        supportPivot /= supportWeight;
        requestedShift /= supportWeight;
    } else {
        supportPivot = ProceduralMath::Translation(nodeTransforms[hipsNode]) - FVec3(0.0f, 1.0f, 0.0f);
    }

    const float maxTilt     = std::max(maxBodyTiltRadians, 0.0f);
    float       targetPitch = 0.0f;
    float       targetRoll  = 0.0f;
    if (requestedShift.LengthSquared() > 1.0e-8f && maxTilt > 0.0f) {
        const float     lever     = std::max((ProceduralMath::Translation(nodeTransforms[hipsNode]) - supportPivot).Length(), 0.25f);
        const float     angle     = std::min(std::atan2(requestedShift.Length(), lever), maxTilt);
        const FVec3 direction = requestedShift.Normalized();
        targetPitch               = direction.z * angle;
        targetRoll                = -direction.x * angle;
    }

    // Use a low-frequency, overdamped spring for IK body tilt. The target
    // changes at stride rate (~1–2 Hz walking); a 6 Hz spring rings at its
    // natural frequency between each foot switch, producing a visible lateral
    // waddle. 2.5 Hz with damping 1.8 tracks the target smoothly without
    // overshoot.
    SpringScalar(gait.ikBodyTiltPitch, gait.ikBodyTiltPitchVelocity, targetPitch, dt, 2.5f, 1.8f);
    SpringScalar(gait.ikBodyTiltRoll, gait.ikBodyTiltRollVelocity, targetRoll, dt, 2.5f, 1.8f);
    const float tiltMagnitude = std::sqrt(gait.ikBodyTiltPitch * gait.ikBodyTiltPitch + gait.ikBodyTiltRoll * gait.ikBodyTiltRoll);
    if (tiltMagnitude > maxTilt && tiltMagnitude > 1.0e-6f) {
        const float scale = maxTilt / tiltMagnitude;
        gait.ikBodyTiltPitch *= scale;
        gait.ikBodyTiltRoll *= scale;
    }

    // Rotate the upper body (spine and above) around the hips, not the entire
    // hips subtree. Rotating from the hips displaces the thighs, which changes
    // IK reach and causes planted feet to slide — violating the IK contract.
    const RigNodeIndex spineNode = Node(map, CharacterBone::Spine);
    const RigNodeIndex tiltRoot  = IsValidRigNode(spineNode, map.nodeCount) ? spineNode : hipsNode;
    const Quat    pitch     = Quat::FromAxisAngle(FVec3::UnitX(), gait.ikBodyTiltPitch);
    const Quat    roll      = Quat::FromAxisAngle(FVec3::UnitZ(), gait.ikBodyTiltRoll);
    RotateSubtreeAroundPivot(map, nodeTransforms, tiltRoot, ProceduralMath::Translation(nodeTransforms[hipsNode]), (roll * pitch).Normalized());
}

/** Applies gait offsets with vertical on hips and lateral on spine.
 *  Vertical motion (bob + drop) moves the whole body up/down — the legs
 *  compress/extend via IK, creating the gravity bounce feel. Lateral
 *  sway stays on the spine to avoid displacing the thighs sideways,
 *  which would break IK reach and cause foot sliding. */
void ApplyPelvisGaitOffset(const ProceduralLocomotionComponent& gait, Mat4x4* nodeTransforms, const RigBoneMap& map, bool includeDrop) noexcept {
    if (nodeTransforms == nullptr) {
        return;
    }
    // Vertical: bob + drop on hips — whole body rises/falls with gravity.
    const RigNodeIndex hipsNode = Node(map, CharacterBone::Hips);
    if (IsValidRigNode(hipsNode, map.nodeCount)) {
        const float drop = includeDrop ? gait.pelvisDrop : 0.0f;
        TranslateSubtree(map, nodeTransforms, hipsNode, FVec3(0.0f, gait.pelvisBob + drop, 0.0f));
    }
    // Lateral: sway on spine only — avoids displacing thighs sideways.
    if (gait.pelvisSway != 0.0f) {
        const RigNodeIndex spineNode = Node(map, CharacterBone::Spine);
        if (IsValidRigNode(spineNode, map.nodeCount)) {
            TranslateSubtree(map, nodeTransforms, spineNode, FVec3(gait.pelvisSway, 0.0f, 0.0f));
        }
    }
}

/** Stage 3: terrain projection, pelvis reach correction, and analytic leg IK. */
void SolveLegGrounding(
    const PhysicsContext&          physics,
    FVec3                   rootPosition,
    Quat                   rootRotation,
    ProceduralLocomotionComponent& gait,
    Mat4x4*                    nodeTransforms,
    const RigBoneMap&              map,
    Physics::BodyID            ignoredPhysicsID,
    float                          ikWeight,
    bool                           preserveAuthoredFootXZ,
    bool                           worldLockFeet,
    float                          maxFootHeightCorrection,
    float                          dt,
    float                          pelvisDropWeight,
    float                          maxLegExtension,
    float                          maxBodyTiltRadians,
    float                          maxAnkleSidewaysRadians,
    float                          maxAnkleForwardRadians
) noexcept {
    if (nodeTransforms == nullptr || map.nodeCount == 0) {
        return;
    }

    const Quat inverseRootRot = rootRotation.Inversed();
    const auto      toWorld        = [&](FVec3 local) { return FVec3(rootPosition) + rootRotation * local; };
    const auto      toModel        = [&](FVec3 world) { return inverseRootRot * (world - FVec3(rootPosition)); };

    struct FootContact {
        FVec3 position;
        FVec3 normal;
    };

    auto raycastFoot = [&](FVec3 localTarget, bool planted, bool enteringPlant, FVec3& lockedWorld, bool& lockValid) -> FootContact {
        FVec3 desiredWorld = toWorld(localTarget);
        FVec3 probeWorld   = (planted && lockValid && !enteringPlant) ? lockedWorld : desiredWorld;
        probeWorld.y = desiredWorld.y;

        const auto hit = physics.Raycast((probeWorld + FVec3(0.0f, 0.75f, 0.0f)).Cast<double>(), {0.0f, -1.0f, 0.0f}, 1.80f, ignoredPhysicsID);

        FootContact contact {.position = desiredWorld, .normal = hit.hasHit ? SafeNormalized(hit.normal, FVec3::UnitY()) : FVec3::UnitY()};
        if (hit.hasHit) {
            const float maxCorrection    = std::max(maxFootHeightCorrection, 0.0f);
            const float heightCorrection = std::clamp(static_cast<float>(hit.position.y) - desiredWorld.y, -maxCorrection, maxCorrection);
            contact.position.y = desiredWorld.y + heightCorrection;
        }

        if (planted) {
            if (!lockValid || enteringPlant) {
                lockedWorld = contact.position;
                lockValid   = true;
            } else {
                // Preserve planted X/Z while allowing the contact height and
                // normal to follow moving or uneven support geometry.
                lockedWorld.y = contact.position.y;
            }
            contact.position = lockedWorld;
        } else {
            lockValid = false;
        }
        return contact;
    };

    auto footProbe = [&](CharacterBone footBone, FVec3 proceduralTarget) {
        if (!preserveAuthoredFootXZ) {
            return FVec3(proceduralTarget);
        }
        const RigNodeIndex footNode = Node(map, footBone);
        return IsValidRigNode(footNode, map.nodeCount) ? ProceduralMath::Translation(nodeTransforms[footNode]) : FVec3(proceduralTarget);
    };

    const FVec3 probeL = footProbe(CharacterBone::FootL, gait.localFootTargetL);
    const FVec3 probeR = footProbe(CharacterBone::FootR, gait.localFootTargetR);
    const bool      lockL  = worldLockFeet && gait.plantWeightL > 0.5f;
    const bool      lockR  = worldLockFeet && gait.plantWeightR > 0.5f;
    // The world lock is an out-parameter, so it steps through a Jolt local.
    FVec3   lockedWorldL = gait.plantedFootWorldL;
    FVec3   lockedWorldR = gait.plantedFootWorldR;
    FootContact contactL     = raycastFoot(probeL, lockL, lockL && !gait.wasPlantedL, lockedWorldL, gait.footLockValidL);
    FootContact contactR     = raycastFoot(probeR, lockR, lockR && !gait.wasPlantedR, lockedWorldR, gait.footLockValidR);
    gait.plantedFootWorldL   = lockedWorldL;
    gait.plantedFootWorldR   = lockedWorldR;
    gait.wasPlantedL         = lockL;
    gait.wasPlantedR         = lockR;
    gait.footNormalL         = contactL.normal;
    gait.footNormalR         = contactR.normal;

    const FVec3 targetModelL = toModel(contactL.position);
    const FVec3 targetModelR = toModel(contactR.position);
    gait.localFootTargetL        = targetModelL;
    gait.localFootTargetR        = targetModelR;

    const RigNodeIndex thighLNode = Node(map, CharacterBone::ThighL);
    const RigNodeIndex thighRNode = Node(map, CharacterBone::ThighR);

    float requiredDrop   = 0.0f;
    auto  accumulateDrop = [&](RigNodeIndex thighNode, FVec3 target, float plantWeight) {
        if (!IsValidRigNode(thighNode, map.nodeCount)) {
            return;
        }
        const float distance = (target - ProceduralMath::Translation(nodeTransforms[thighNode])).Length();
        const float excess   = std::max(0.0f, distance - std::max(gait.legReach, 0.05f));
        requiredDrop         = std::max(requiredDrop, excess * SmoothStep(plantWeight));
    };
    accumulateDrop(thighLNode, targetModelL, gait.plantWeightL);
    accumulateDrop(thighRNode, targetModelR, gait.plantWeightR);

    gait.targetPelvisDrop = -std::clamp(requiredDrop * std::clamp(pelvisDropWeight, 0.0f, 1.0f), 0.0f, 0.38f);
    // Overdamped, low-frequency pelvis drop spring. The target alternates at
    // stride rate (~1–2 Hz walking); a 5 Hz critically-damped spring rings at
    // its natural frequency between foot switches, producing a visible lateral
    // waddle. 2 Hz with damping 1.8 tracks terrain changes without oscillation.
    SpringScalar(gait.pelvisDrop, gait.pelvisDropVelocity, gait.targetPelvisDrop, dt, 2.0f, 1.8f);

    ApplyPelvisGaitOffset(gait, nodeTransforms, map, true);
    ApplyIKReachTilt(
        gait, nodeTransforms, map, targetModelL, targetModelR, gait.plantWeightL * ikWeight, gait.plantWeightR * ikWeight, maxLegExtension, maxBodyTiltRadians,
        dt
    );

    gait.ikReachClampedL = false;
    gait.ikReachClampedR = false;
    auto solveLeg        = [&](CharacterBone thighBone, CharacterBone shinBone, CharacterBone footBone, FVec3 target, FVec3 worldNormal,
                               float plantWeight, bool& reachClamped) {
        const float solveWeight = std::clamp(plantWeight * ikWeight, 0.0f, 1.0f);
        if (solveWeight <= 0.001f) {
            return; // Preserve the authored swing pose completely.
        }

        const RigNodeIndex thighNode = Node(map, thighBone);
        const RigNodeIndex shinNode  = Node(map, shinBone);
        const RigNodeIndex footNode  = Node(map, footBone);
        if (!IsValidRigNode(thighNode, map.nodeCount) || !IsValidRigNode(shinNode, map.nodeCount) || !IsValidRigNode(footNode, map.nodeCount)) {
            return;
        }

        const FVec3 thighPosition = ProceduralMath::Translation(nodeTransforms[thighNode]);
        const FVec3 shinPosition  = ProceduralMath::Translation(nodeTransforms[shinNode]);
        const FVec3 footPosition  = ProceduralMath::Translation(nodeTransforms[footNode]);
        const float     upperLength   = std::max((shinPosition - thighPosition).Length(), 0.001f);
        const float     lowerLength   = std::max((footPosition - shinPosition).Length(), 0.001f);

        // Preserve the authored knee plane. The IK correction should bend from
        // the keyframed shin direction rather than resetting every plant to a
        // hard-coded character-forward pole.
        const FVec3 targetAxis   = SafeNormalized(target - thighPosition, FVec3(0.0f, -1.0f, 0.0f));
        FVec3       authoredPole = (shinPosition - thighPosition) - targetAxis * Dot((shinPosition - thighPosition), targetAxis);
        if (authoredPole.LengthSquared() < 1.0e-6f) {
            const float     poleX = thighBone == CharacterBone::ThighL ? 0.08f : -0.08f;
            const FVec3 fallbackPole(poleX, 0.0f, 1.0f);
            authoredPole = fallbackPole - targetAxis * Dot(fallbackPole, targetAxis);
        }
        const FVec3 pole = SafeNormalized(authoredPole, FVec3::UnitZ());
        const auto      ik   = IK::SolveTwoBoneIK({
            .upperPosition  = thighPosition,
            .targetPosition = target,
            .poleVector     = pole,
            .upperLength    = upperLength,
            .lowerLength    = lowerLength,
            .maxExtension   = maxLegExtension,
        });
        if (!ik.valid) {
            return;
        }

        // Blend the end target, then solve the chain again. Blending thigh,
        // knee, and ankle positions independently changes segment lengths; a
        // second analytic solve keeps both lengths exact at every IK weight.
        const float     poseWeight  = SmoothStep(solveWeight);
        const FVec3 posedTarget = footPosition + (ik.endPosition - footPosition) * poseWeight;
        const auto      posedIK     = IK::SolveTwoBoneIK({
            .upperPosition  = thighPosition,
            .targetPosition = posedTarget,
            .poleVector     = pole,
            .upperLength    = upperLength,
            .lowerLength    = lowerLength,
            .maxExtension   = maxLegExtension,
        });
        if (!posedIK.valid) {
            return;
        }
        reachClamped = ik.reachClamped || posedIK.reachClamped;

        // Derive correction axes from the evaluated model-space pose rather
        // than assuming that imported bones use a particular local axis.
        const FVec3 currentUpperDir = SafeNormalized(shinPosition - thighPosition, FVec3(0.0f, -1.0f, 0.0f));
        const FVec3 currentLowerDir = SafeNormalized(footPosition - shinPosition, FVec3(0.0f, -1.0f, 0.0f));

        const Mat4x4 authoredThigh = nodeTransforms[thighNode];
        const Mat4x4 authoredShin  = nodeTransforms[shinNode];
        const Mat4x4 authoredFoot  = nodeTransforms[footNode];

        const Mat4x4 solvedThigh = CorrectBoneDirection(authoredThigh, currentUpperDir, posedIK.upperDirection, thighPosition);
        const Mat4x4 solvedShin  = CorrectBoneDirection(authoredShin, currentLowerDir, posedIK.lowerDirection, posedIK.midPosition);

        // Flat ground is the identity correction: sFromTo(+Y, +Y).
        const FVec3  modelNormal = SafeNormalized(inverseRootRot * worldNormal, FVec3::UnitY());
        const Mat4x4 solvedFoot  = AlignFootToGround(authoredFoot, posedIK.endPosition, modelNormal, maxAnkleSidewaysRadians, maxAnkleForwardRadians);

        const Mat4x4 posedThigh = solvedThigh;
        const Mat4x4 posedShin  = solvedShin;
        Mat4x4       posedFoot  = BlendTransform(authoredFoot, solvedFoot, solveWeight);
        ProceduralMath::SetTranslation(posedFoot, posedIK.endPosition);

        // Apply proximal-to-distal targets while carrying each imported subtree.
        // Child transform nodes (foot shells, shoe pieces, sockets) were already
        // evaluated from the authored pose and otherwise remain visually behind.
        SetModelTransformAndCarrySubtree(nodeTransforms, map, thighNode, posedThigh);
        SetModelTransformAndCarrySubtree(nodeTransforms, map, shinNode, posedShin);
        SetModelTransformAndCarrySubtree(nodeTransforms, map, footNode, posedFoot);
    };

    solveLeg(CharacterBone::ThighL, CharacterBone::ShinL, CharacterBone::FootL, targetModelL, contactL.normal, gait.plantWeightL, gait.ikReachClampedL);
    solveLeg(CharacterBone::ThighR, CharacterBone::ShinR, CharacterBone::FootR, targetModelR, contactR.normal, gait.plantWeightR, gait.ikReachClampedR);
}

/** Stage 4: anti-phase arm swing and distributed look-at. */
void SolveUpperBody(
    const ProceduralLocomotionComponent& gait,
    const ProceduralLookAtComponent*     lookAt,
    FVec3                         rootPosition,
    Quat                         rootRotation,
    Mat4x4*                          nodeTransforms,
    const RigBoneMap&                    map,
    bool                                 applyArmSwing,
    bool                                 applyLookAt
) noexcept {
    if (nodeTransforms == nullptr || map.nodeCount == 0) {
        return;
    }

    if (applyArmSwing) {
        const float        horizontalSpeed = std::sqrt(gait.previousVelocity.x * gait.previousVelocity.x + gait.previousVelocity.z * gait.previousVelocity.z);
        const float        swingWeight = std::clamp(horizontalSpeed * 0.35f, 0.0f, 1.0f);
        const float        armAngle    = std::sin(kGaitTwoPi * gait.phase) * 0.58f * swingWeight * gait.armSwingScale;
        const RigNodeIndex armL        = Node(map, CharacterBone::UpperArmL);
        const RigNodeIndex armR        = Node(map, CharacterBone::UpperArmR);
        if (IsValidRigNode(armL, map.nodeCount)) {
            RotateSubtree(map, nodeTransforms, armL, Quat::FromAxisAngle(FVec3::UnitX(), -armAngle));
        }
        if (IsValidRigNode(armR, map.nodeCount)) {
            RotateSubtree(map, nodeTransforms, armR, Quat::FromAxisAngle(FVec3::UnitX(), armAngle));
        }
    }

    if (!applyLookAt || lookAt == nullptr || lookAt->weight <= 0.001f) {
        return;
    }

    const RigNodeIndex headNode = Node(map, CharacterBone::Head);
    if (!IsValidRigNode(headNode, map.nodeCount)) {
        return;
    }

    const Quat inverseRootRot = rootRotation.Inversed();
    const FVec3 targetModel    = inverseRootRot * (FVec3(lookAt->targetWorldPos) - FVec3(rootPosition));
    const FVec3 headPosition   = ProceduralMath::Translation(nodeTransforms[headNode]);
    FVec3       targetDir      = targetModel - headPosition;
    if (targetDir.LengthSquared() < 1.0e-8f) {
        return;
    }
    targetDir = targetDir.Normalized();

    const FVec3 currentForward = SafeNormalized(ProceduralMath::Direction(nodeTransforms[headNode], FVec3::UnitZ()), FVec3::UnitZ());
    const float     aimAngle       = std::acos(std::clamp(Dot(currentForward, targetDir), -1.0f, 1.0f));
    const float     maxAngle       = std::clamp(ProceduralMath::Radians(lookAt->maxAngleDeg), 0.0f, std::numbers::pi_v<float>);
    Quat       fullAim        = FromTo(currentForward, targetDir);
    if (aimAngle > maxAngle && aimAngle > 1.0e-5f) {
        fullAim = Slerp(Quat::Identity(), fullAim, maxAngle / aimAngle).Normalized();
    }

    const float                                          clampedWeight    = std::clamp(lookAt->weight, 0.0f, 1.0f);
    const std::array<std::pair<CharacterBone, float>, 3> lookDistribution = {{
        {CharacterBone::Spine, 0.15f},
        {CharacterBone::Chest, 0.25f},
        {CharacterBone::Head, 0.60f},
    }};
    for (const auto& [bone, weight]: lookDistribution) {
        const RigNodeIndex node = Node(map, bone);
        if (IsValidRigNode(node, map.nodeCount)) {
            const Quat partial = Slerp(Quat::Identity(), fullAim, weight * clampedWeight).Normalized();
            RotateSubtree(map, nodeTransforms, node, partial);
        }
    }
}

void SolveUpperBody(
    const ProceduralLocomotionComponent& gait,
    const ProceduralLookAtComponent*     lookAt,
    Mat4x4*                          nodeTransforms,
    const RigBoneMap&                    map
) noexcept {
    SolveUpperBody(gait, lookAt, FVec3::Zero(), Quat::Identity(), nodeTransforms, map, true, true);
}

} // namespace ZHLN::Animation
