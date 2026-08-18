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
#include <Zahlen/Engine.hpp>
#include <Zahlen/IK.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

module ZHLN.ProceduralAnimation;

namespace ZHLN::Animation {

inline constexpr float kGaitTwoPi = 2.0f * std::numbers::pi_v<float>;

namespace Detail {

[[nodiscard]] inline float WrapUnit(float value) noexcept {
    value = std::fmod(value, 1.0f);
    return value < 0.0f ? value + 1.0f : value;
}

[[nodiscard]] inline float SmoothStep(float value) noexcept {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

inline void SpringScalar(float& value, float& velocity, float target, float dt, float frequency, float damping) noexcept {
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

[[nodiscard]] inline bool IsDescendant(const RigBoneMap& map, RigNodeIndex node, RigNodeIndex ancestor) noexcept {
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

inline void TranslateSubtree(const RigBoneMap& map, JPH::Mat44* transforms, RigNodeIndex rootNode, JPH::Vec3Arg delta) noexcept {
    if (transforms == nullptr || !IsValidRigNode(rootNode, map.nodeCount)) {
        return;
    }
    for (size_t node = 0; node < map.nodeCount; ++node) {
        if (IsDescendant(map, node, rootNode)) {
            transforms[node].SetTranslation(transforms[node].GetTranslation() + delta);
        }
    }
}

[[nodiscard]] inline JPH::Vec3 MatrixScale(const JPH::Mat44& matrix) noexcept {
    return JPH::Vec3(matrix.GetColumn3(0).Length(), matrix.GetColumn3(1).Length(), matrix.GetColumn3(2).Length());
}

[[nodiscard]] inline JPH::Quat MatrixRotation(const JPH::Mat44& matrix) noexcept {
    const JPH::Vec3 scale = MatrixScale(matrix);
    const JPH::Vec3 x     = scale.GetX() > 1.0e-6f ? matrix.GetColumn3(0) / scale.GetX() : JPH::Vec3::sAxisX();
    const JPH::Vec3 y     = scale.GetY() > 1.0e-6f ? matrix.GetColumn3(1) / scale.GetY() : JPH::Vec3::sAxisY();
    const JPH::Vec3 z     = scale.GetZ() > 1.0e-6f ? matrix.GetColumn3(2) / scale.GetZ() : JPH::Vec3::sAxisZ();
    return JPH::Mat44(JPH::Vec4(x, 0.0f), JPH::Vec4(y, 0.0f), JPH::Vec4(z, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)).GetQuaternion().Normalized();
}

[[nodiscard]] inline JPH::Mat44 BlendTransform(const JPH::Mat44& authored, const JPH::Mat44& solved, float weight) noexcept {
    const float     easedWeight   = SmoothStep(weight);
    const JPH::Vec3 translation   = authored.GetTranslation() + (solved.GetTranslation() - authored.GetTranslation()) * easedWeight;
    const JPH::Vec3 authoredScale = MatrixScale(authored);
    const JPH::Vec3 solvedScale   = MatrixScale(solved);
    const JPH::Vec3 scale         = authoredScale + (solvedScale - authoredScale) * easedWeight;
    const JPH::Quat rotation      = MatrixRotation(authored).SLERP(MatrixRotation(solved), easedWeight).Normalized();
    return JPH::Mat44::sRotationTranslation(rotation, translation).PreScaled(scale);
}

inline void RotateSubtreeAroundPivot(const RigBoneMap& map, JPH::Mat44* transforms, RigNodeIndex rootNode, JPH::Vec3Arg pivot, JPH::QuatArg rotation) noexcept {
    if (transforms == nullptr || !IsValidRigNode(rootNode, map.nodeCount)) {
        return;
    }

    for (size_t node = 0; node < map.nodeCount; ++node) {
        if (!IsDescendant(map, node, rootNode)) {
            continue;
        }

        const JPH::Vec3 oldPosition = transforms[node].GetTranslation();
        const JPH::Vec3 oldScale    = MatrixScale(transforms[node]);
        const JPH::Vec3 newPosition = pivot + rotation * (oldPosition - pivot);
        const JPH::Quat oldRotation = MatrixRotation(transforms[node]);
        transforms[node]            = JPH::Mat44::sRotationTranslation((rotation * oldRotation).Normalized(), newPosition).PreScaled(oldScale);
    }
}

inline void RotateSubtree(const RigBoneMap& map, JPH::Mat44* transforms, RigNodeIndex rootNode, JPH::QuatArg rotation) noexcept {
    if (transforms == nullptr || !IsValidRigNode(rootNode, map.nodeCount)) {
        return;
    }
    RotateSubtreeAroundPivot(map, transforms, rootNode, transforms[rootNode].GetTranslation(), rotation);
}

[[nodiscard]] inline RigNodeIndex Node(const RigBoneMap& map, CharacterBone bone) noexcept {
    return map.nodeIndices[BoneSlot(bone)];
}

[[nodiscard]] inline JPH::Vec3 SafeNormalized(JPH::Vec3Arg value, JPH::Vec3Arg fallback) noexcept {
    return value.LengthSq() > 1.0e-8f ? value.Normalized() : JPH::Vec3(fallback);
}

} // namespace Detail

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
    const float supportPhase     = Detail::WrapUnit(gait.phase * 2.0f);
    return 0.5f * amplitude * (1.0f - std::cos(kGaitTwoPi * supportPhase));
}

/** Maps one stride-wheel revolution onto two opposing authored reach keys. */
float EvaluateTwoKeyPosePhase(float stridePhase) noexcept {
    const float phase = Detail::WrapUnit(stridePhase);
    return phase <= 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
}

/**
 * Stage 1 + 2: extract directional acceleration, advance the distance-driven
 * stride clock, and evaluate alternating cubic/parabolic foot trajectories.
 * Velocity is expected in character-local space.
 */
void EvaluateGait(ProceduralLocomotionComponent& gait, JPH::Vec3Arg velocity, float angularVelocity, float dt) noexcept {
    const float safeDt           = std::max(dt, 0.0001f);
    gait.directionalAcceleration = (velocity - gait.previousVelocity) / safeDt;
    gait.previousVelocity        = velocity;
    gait.turnRate                = angularVelocity;

    const float speed     = std::sqrt(velocity.GetX() * velocity.GetX() + velocity.GetZ() * velocity.GetZ());
    const float cycleRate = speed / std::max(gait.strideLength, 0.01f);
    gait.phase            = Detail::WrapUnit(gait.phase + cycleRate * std::max(dt, 0.0f));
    gait.strideWheelAngle = gait.phase * kGaitTwoPi;

    auto evaluateFoot = [&](float phaseOffset, JPH::Vec3& target, float& plantWeight, float& passWeight, float& reachWeight) {
        const float p       = Detail::WrapUnit(gait.phase + phaseOffset);
        const bool  isSwing = p < 0.5f;
        const float t       = isSwing ? p * 2.0f : (p - 0.5f) * 2.0f;
        const float smoothT = t * t * (3.0f - 2.0f * t);

        // During swing the foot travels heel-to-toe. The stance trajectory is
        // only a prediction; SolveLegGrounding replaces it with a world lock.
        const float z = isSwing ? (smoothT - 0.5f) * gait.strideLength : (0.5f - t) * gait.strideLength;
        const float y = isSwing ? 4.0f * t * (1.0f - t) * gait.stepHeight : 0.0f;

        target.SetY(y);
        target.SetZ(z);
        if (isSwing) {
            plantWeight = 0.0f;
        } else {
            constexpr float kContactBlendFraction = 0.12f;
            const float     fadeIn                = Detail::SmoothStep(t / kContactBlendFraction);
            const float     fadeOut               = Detail::SmoothStep((1.0f - t) / kContactBlendFraction);
            plantWeight                           = fadeIn * fadeOut;
        }

        const float wheelPass  = std::sin(kGaitTwoPi * p);
        const float wheelReach = std::cos(kGaitTwoPi * p);
        passWeight             = wheelPass * wheelPass;
        reachWeight            = wheelReach * wheelReach;
    };

    evaluateFoot(0.0f, gait.localFootTargetL, gait.plantWeightL, gait.passWeightL, gait.reachWeightL);
    evaluateFoot(0.5f, gait.localFootTargetR, gait.plantWeightR, gait.passWeightR, gait.reachWeightR);
    gait.localFootTargetL.SetX(0.14f);
    gait.localFootTargetR.SetX(-0.14f);

    if (speed < 0.035f) {
        gait.localFootTargetL.SetZ(0.0f);
        gait.localFootTargetR.SetZ(0.0f);
        gait.localFootTargetL.SetY(0.0f);
        gait.localFootTargetR.SetY(0.0f);
        gait.plantWeightL  = 1.0f;
        gait.plantWeightR  = 1.0f;
        gait.gravityBounce = 0.0f;
        gait.pelvisBob     = 0.0f;
        gait.pelvisSway    = 0.0f;
    } else {
        gait.gravityBounce = EvaluateGravityBounce(gait, speed);
        gait.pelvisBob     = gait.gravityBounce;
        gait.pelvisSway    = std::sin(kGaitTwoPi * gait.phase) * 0.035f;
    }

    const float targetForwardLean = std::clamp(-gait.directionalAcceleration.GetZ() * 0.018f, -0.22f, 0.22f);
    const float centripetal       = speed * angularVelocity;
    const float targetLateralBank = std::clamp(gait.directionalAcceleration.GetX() * 0.008f - centripetal * 0.018f, -0.28f, 0.28f);
    Detail::SpringScalar(gait.forwardLean, gait.tiltPitchVelocity, targetForwardLean, dt, 5.5f, 0.88f);
    Detail::SpringScalar(gait.lateralBank, gait.tiltRollVelocity, targetLateralBank, dt, 5.5f, 0.88f);
}

void EvaluateGait(ProceduralLocomotionComponent& gait, JPH::Vec3Arg velocity, float dt) noexcept {
    EvaluateGait(gait, velocity, 0.0f, dt);
}

/** Rotates the complete posed body around an estimated center of mass. */
void ApplyAccelerationTilt(ProceduralLocomotionComponent& gait, JPH::Mat44* nodeTransforms, const RigBoneMap& map) noexcept {
    if (nodeTransforms == nullptr || map.nodeCount == 0) {
        return;
    }

    const RigNodeIndex hipsNode  = Detail::Node(map, CharacterBone::Hips);
    const RigNodeIndex chestNode = Detail::Node(map, CharacterBone::Chest);
    if (!IsValidRigNode(hipsNode, map.nodeCount)) {
        return;
    }

    const JPH::Vec3 hipsPosition  = nodeTransforms[hipsNode].GetTranslation();
    const JPH::Vec3 chestPosition = IsValidRigNode(chestNode, map.nodeCount) ? nodeTransforms[chestNode].GetTranslation() :
                                                                               hipsPosition + JPH::Vec3(0.0f, 0.35f, 0.0f);
    // Approximate the humanoid COM from pelvis and torso masses. This is a
    // physical weighted center, not an animation interpolation.
    gait.centerOfMassModel = (hipsPosition * 0.68f + chestPosition * 0.32f);

    const JPH::Quat pitch = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), gait.forwardLean);
    const JPH::Quat roll  = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), gait.lateralBank);
    Detail::RotateSubtreeAroundPivot(map, nodeTransforms, hipsNode, gait.centerOfMassModel, (pitch * roll).Normalized());
}

JPH::Mat44 CorrectBoneDirection(
    const JPH::Mat44& authoredTransform,
    JPH::Vec3Arg      currentDirection,
    JPH::Vec3Arg      solvedDirection,
    JPH::Vec3Arg      solvedPosition
) noexcept {
    const JPH::Quat correction = JPH::Quat::sFromTo(
        Detail::SafeNormalized(currentDirection, JPH::Vec3(0.0f, -1.0f, 0.0f)), Detail::SafeNormalized(solvedDirection, JPH::Vec3(0.0f, -1.0f, 0.0f))
    );
    const JPH::Quat rotation = (correction * Detail::MatrixRotation(authoredTransform)).Normalized();
    return JPH::Mat44::sRotationTranslation(rotation, solvedPosition).PreScaled(Detail::MatrixScale(authoredTransform));
}

JPH::Mat44 AlignFootToGround(const JPH::Mat44& authoredFoot, JPH::Vec3Arg target, JPH::Vec3Arg modelNormal) noexcept {
    const JPH::Quat correction = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), Detail::SafeNormalized(modelNormal, JPH::Vec3::sAxisY()));
    const JPH::Quat rotation   = (correction * Detail::MatrixRotation(authoredFoot)).Normalized();
    return JPH::Mat44::sRotationTranslation(rotation, target).PreScaled(Detail::MatrixScale(authoredFoot));
}

/** Applies gait sway/bounce independently from analytical foot IK. */
void ApplyPelvisGaitOffset(const ProceduralLocomotionComponent& gait, JPH::Mat44* nodeTransforms, const RigBoneMap& map, bool includeDrop) noexcept {
    if (nodeTransforms == nullptr) {
        return;
    }
    const RigNodeIndex hipsNode = Detail::Node(map, CharacterBone::Hips);
    if (IsValidRigNode(hipsNode, map.nodeCount)) {
        const float drop = includeDrop ? gait.pelvisDrop : 0.0f;
        Detail::TranslateSubtree(map, nodeTransforms, hipsNode, JPH::Vec3(gait.pelvisSway, gait.pelvisBob + drop, 0.0f));
    }
}

/** Stage 3: terrain projection, pelvis reach correction, and analytic leg IK. */
void SolveLegGrounding(
    Engine&                        engine,
    JPH::Vec3Arg                   rootPosition,
    JPH::QuatArg                   rootRotation,
    ProceduralLocomotionComponent& gait,
    JPH::Mat44*                    nodeTransforms,
    const RigBoneMap&              map,
    Entity                         ignoredPhysicsHandle,
    float                          ikWeight,
    bool                           preserveAuthoredFootXZ,
    bool                           worldLockFeet,
    float                          maxFootHeightCorrection,
    float                          dt,
    float                          pelvisDropWeight
) noexcept {
    if (nodeTransforms == nullptr || map.nodeCount == 0) {
        return;
    }

    auto&           physics        = engine.GetPhysicsContext();
    const JPH::Quat inverseRootRot = rootRotation.Inversed();
    const auto      toWorld        = [&](JPH::Vec3Arg local) { return JPH::Vec3(rootPosition) + rootRotation * local; };
    const auto      toModel        = [&](JPH::Vec3Arg world) { return inverseRootRot * (world - JPH::Vec3(rootPosition)); };

    struct FootContact {
        JPH::Vec3 position;
        JPH::Vec3 normal;
    };

    auto raycastFoot = [&](JPH::Vec3Arg localTarget, bool planted, bool enteringPlant, JPH::Vec3& lockedWorld, bool& lockValid) -> FootContact {
        JPH::Vec3 desiredWorld = toWorld(localTarget);
        JPH::Vec3 probeWorld   = (planted && lockValid && !enteringPlant) ? lockedWorld : desiredWorld;
        probeWorld.SetY(desiredWorld.GetY());

        const auto hit = physics.Raycast(JPH::RVec3(probeWorld + JPH::Vec3(0.0f, 0.75f, 0.0f)), JPH::Vec3(0.0f, -1.0f, 0.0f), 1.80f, ignoredPhysicsHandle);

        FootContact contact {.position = desiredWorld, .normal = hit.hasHit ? Detail::SafeNormalized(hit.normal, JPH::Vec3::sAxisY()) : JPH::Vec3::sAxisY()};
        if (hit.hasHit) {
            const float maxCorrection    = std::max(maxFootHeightCorrection, 0.0f);
            const float heightCorrection = std::clamp(JPH::Vec3(hit.position).GetY() - desiredWorld.GetY(), -maxCorrection, maxCorrection);
            contact.position.SetY(desiredWorld.GetY() + heightCorrection);
        }

        if (planted) {
            if (!lockValid || enteringPlant) {
                lockedWorld = contact.position;
                lockValid   = true;
            } else {
                // Preserve planted X/Z while allowing the contact height and
                // normal to follow moving or uneven support geometry.
                lockedWorld.SetY(contact.position.GetY());
            }
            contact.position = lockedWorld;
        } else {
            lockValid = false;
        }
        return contact;
    };

    auto footProbe = [&](CharacterBone footBone, JPH::Vec3Arg proceduralTarget) {
        if (!preserveAuthoredFootXZ) {
            return JPH::Vec3(proceduralTarget);
        }
        const RigNodeIndex footNode = Detail::Node(map, footBone);
        return IsValidRigNode(footNode, map.nodeCount) ? nodeTransforms[footNode].GetTranslation() : JPH::Vec3(proceduralTarget);
    };

    const JPH::Vec3 probeL   = footProbe(CharacterBone::FootL, gait.localFootTargetL);
    const JPH::Vec3 probeR   = footProbe(CharacterBone::FootR, gait.localFootTargetR);
    const bool      lockL    = worldLockFeet && gait.plantWeightL > 0.5f;
    const bool      lockR    = worldLockFeet && gait.plantWeightR > 0.5f;
    FootContact     contactL = raycastFoot(probeL, lockL, lockL && !gait.wasPlantedL, gait.plantedFootWorldL, gait.footLockValidL);
    FootContact     contactR = raycastFoot(probeR, lockR, lockR && !gait.wasPlantedR, gait.plantedFootWorldR, gait.footLockValidR);
    gait.wasPlantedL         = lockL;
    gait.wasPlantedR         = lockR;
    gait.footNormalL         = contactL.normal;
    gait.footNormalR         = contactR.normal;

    const JPH::Vec3 targetModelL = toModel(contactL.position);
    const JPH::Vec3 targetModelR = toModel(contactR.position);
    gait.localFootTargetL        = targetModelL;
    gait.localFootTargetR        = targetModelR;

    const RigNodeIndex thighLNode = Detail::Node(map, CharacterBone::ThighL);
    const RigNodeIndex thighRNode = Detail::Node(map, CharacterBone::ThighR);

    float requiredDrop   = 0.0f;
    auto  accumulateDrop = [&](RigNodeIndex thighNode, JPH::Vec3Arg target, float plantWeight) {
        if (!IsValidRigNode(thighNode, map.nodeCount)) {
            return;
        }
        const float distance = (target - nodeTransforms[thighNode].GetTranslation()).Length();
        const float excess   = std::max(0.0f, distance - std::max(gait.legReach, 0.05f));
        requiredDrop         = std::max(requiredDrop, excess * Detail::SmoothStep(plantWeight));
    };
    accumulateDrop(thighLNode, targetModelL, gait.plantWeightL);
    accumulateDrop(thighRNode, targetModelR, gait.plantWeightR);

    gait.targetPelvisDrop = -std::clamp(requiredDrop * std::clamp(pelvisDropWeight, 0.0f, 1.0f), 0.0f, 0.38f);
    Detail::SpringScalar(gait.pelvisDrop, gait.pelvisDropVelocity, gait.targetPelvisDrop, dt, 5.0f, 1.0f);

    ApplyPelvisGaitOffset(gait, nodeTransforms, map, true);

    auto solveLeg = [&](CharacterBone thighBone, CharacterBone shinBone, CharacterBone footBone, CharacterBone toeBone, JPH::Vec3Arg target,
                        JPH::Vec3Arg worldNormal, float plantWeight) {
        const float solveWeight = std::clamp(plantWeight * ikWeight, 0.0f, 1.0f);
        if (solveWeight <= 0.001f) {
            return; // Preserve the authored swing pose completely.
        }

        const RigNodeIndex thighNode = Detail::Node(map, thighBone);
        const RigNodeIndex shinNode  = Detail::Node(map, shinBone);
        const RigNodeIndex footNode  = Detail::Node(map, footBone);
        const RigNodeIndex toeNode   = Detail::Node(map, toeBone);
        if (!IsValidRigNode(thighNode, map.nodeCount) || !IsValidRigNode(shinNode, map.nodeCount) || !IsValidRigNode(footNode, map.nodeCount)) {
            return;
        }

        const JPH::Vec3 thighPosition = nodeTransforms[thighNode].GetTranslation();
        const JPH::Vec3 shinPosition  = nodeTransforms[shinNode].GetTranslation();
        const JPH::Vec3 footPosition  = nodeTransforms[footNode].GetTranslation();
        const float     upperLength   = std::max((shinPosition - thighPosition).Length(), 0.001f);
        const float     lowerLength   = std::max((footPosition - shinPosition).Length(), 0.001f);

        // Preserve the authored knee plane. The IK correction should bend from
        // the keyframed shin direction rather than resetting every plant to a
        // hard-coded character-forward pole.
        const JPH::Vec3 targetAxis   = Detail::SafeNormalized(target - thighPosition, JPH::Vec3(0.0f, -1.0f, 0.0f));
        JPH::Vec3       authoredPole = (shinPosition - thighPosition) - targetAxis * (shinPosition - thighPosition).Dot(targetAxis);
        if (authoredPole.LengthSq() < 1.0e-6f) {
            const float     poleX = thighBone == CharacterBone::ThighL ? 0.08f : -0.08f;
            const JPH::Vec3 fallbackPole(poleX, 0.0f, 1.0f);
            authoredPole = fallbackPole - targetAxis * fallbackPole.Dot(targetAxis);
        }
        const JPH::Vec3 pole = Detail::SafeNormalized(authoredPole, JPH::Vec3::sAxisZ());
        const auto      ik   = IK::SolveTwoBoneIK({
            .upperPosition  = thighPosition,
            .targetPosition = target,
            .poleVector     = pole,
            .upperLength    = upperLength,
            .lowerLength    = lowerLength,
        });
        if (!ik.valid) {
            return;
        }

        // Derive correction axes from the evaluated model-space pose rather
        // than assuming that imported bones use a particular local axis.
        const JPH::Vec3 currentUpperDir = Detail::SafeNormalized(shinPosition - thighPosition, JPH::Vec3(0.0f, -1.0f, 0.0f));
        const JPH::Vec3 currentLowerDir = Detail::SafeNormalized(footPosition - shinPosition, JPH::Vec3(0.0f, -1.0f, 0.0f));

        const JPH::Mat44 authoredThigh = nodeTransforms[thighNode];
        const JPH::Mat44 authoredShin  = nodeTransforms[shinNode];
        const JPH::Mat44 authoredFoot  = nodeTransforms[footNode];

        const JPH::Mat44 solvedThigh = CorrectBoneDirection(authoredThigh, currentUpperDir, ik.upperDirection, thighPosition);
        const JPH::Mat44 solvedShin  = CorrectBoneDirection(authoredShin, currentLowerDir, ik.lowerDirection, ik.midPosition);

        // Flat ground is the identity correction: sFromTo(+Y, +Y).
        const JPH::Vec3  modelNormal = Detail::SafeNormalized(inverseRootRot * worldNormal, JPH::Vec3::sAxisY());
        const JPH::Mat44 solvedFoot  = AlignFootToGround(authoredFoot, target, modelNormal);

        nodeTransforms[thighNode] = Detail::BlendTransform(authoredThigh, solvedThigh, solveWeight);
        nodeTransforms[shinNode]  = Detail::BlendTransform(authoredShin, solvedShin, solveWeight);
        nodeTransforms[footNode]  = Detail::BlendTransform(authoredFoot, solvedFoot, solveWeight);

        if (IsValidRigNode(toeNode, map.nodeCount)) {
            const JPH::Mat44 authoredToe = nodeTransforms[toeNode];
            const JPH::Mat44 solvedToe   = solvedFoot * map.bindLocalTransforms[toeNode];
            nodeTransforms[toeNode]      = Detail::BlendTransform(authoredToe, solvedToe, solveWeight);
        }
    };

    solveLeg(CharacterBone::ThighL, CharacterBone::ShinL, CharacterBone::FootL, CharacterBone::ToeL, targetModelL, contactL.normal, gait.plantWeightL);
    solveLeg(CharacterBone::ThighR, CharacterBone::ShinR, CharacterBone::FootR, CharacterBone::ToeR, targetModelR, contactR.normal, gait.plantWeightR);
}

/** Stage 4: anti-phase arm swing and distributed look-at. */
void SolveUpperBody(
    const ProceduralLocomotionComponent& gait,
    const ProceduralLookAtComponent*     lookAt,
    JPH::Vec3Arg                         rootPosition,
    JPH::QuatArg                         rootRotation,
    JPH::Mat44*                          nodeTransforms,
    const RigBoneMap&                    map,
    bool                                 applyArmSwing,
    bool                                 applyLookAt
) noexcept {
    if (nodeTransforms == nullptr || map.nodeCount == 0) {
        return;
    }

    if (applyArmSwing) {
        const float horizontalSpeed =
            std::sqrt(gait.previousVelocity.GetX() * gait.previousVelocity.GetX() + gait.previousVelocity.GetZ() * gait.previousVelocity.GetZ());
        const float        swingWeight = std::clamp(horizontalSpeed * 0.35f, 0.0f, 1.0f);
        const float        armAngle    = std::sin(kGaitTwoPi * gait.phase) * 0.58f * swingWeight;
        const RigNodeIndex armL        = Detail::Node(map, CharacterBone::UpperArmL);
        const RigNodeIndex armR        = Detail::Node(map, CharacterBone::UpperArmR);
        if (IsValidRigNode(armL, map.nodeCount)) {
            Detail::RotateSubtree(map, nodeTransforms, armL, JPH::Quat::sRotation(JPH::Vec3::sAxisX(), -armAngle));
        }
        if (IsValidRigNode(armR, map.nodeCount)) {
            Detail::RotateSubtree(map, nodeTransforms, armR, JPH::Quat::sRotation(JPH::Vec3::sAxisX(), armAngle));
        }
    }

    if (!applyLookAt || lookAt == nullptr || lookAt->weight <= 0.001f) {
        return;
    }

    const RigNodeIndex headNode = Detail::Node(map, CharacterBone::Head);
    if (!IsValidRigNode(headNode, map.nodeCount)) {
        return;
    }

    const JPH::Quat inverseRootRot = rootRotation.Inversed();
    const JPH::Vec3 targetModel    = inverseRootRot * (lookAt->targetWorldPos - JPH::Vec3(rootPosition));
    const JPH::Vec3 headPosition   = nodeTransforms[headNode].GetTranslation();
    JPH::Vec3       targetDir      = targetModel - headPosition;
    if (targetDir.LengthSq() < 1.0e-8f) {
        return;
    }
    targetDir = targetDir.Normalized();

    const JPH::Vec3 currentForward = Detail::SafeNormalized(nodeTransforms[headNode].Multiply3x3(JPH::Vec3::sAxisZ()), JPH::Vec3::sAxisZ());
    const float     aimAngle       = std::acos(std::clamp(currentForward.Dot(targetDir), -1.0f, 1.0f));
    const float     maxAngle       = std::clamp(JPH::DegreesToRadians(lookAt->maxAngleDeg), 0.0f, std::numbers::pi_v<float>);
    JPH::Quat       fullAim        = JPH::Quat::sFromTo(currentForward, targetDir);
    if (aimAngle > maxAngle && aimAngle > 1.0e-5f) {
        fullAim = JPH::Quat::sIdentity().SLERP(fullAim, maxAngle / aimAngle).Normalized();
    }

    const float                                          clampedWeight    = std::clamp(lookAt->weight, 0.0f, 1.0f);
    const std::array<std::pair<CharacterBone, float>, 3> lookDistribution = {{
        {CharacterBone::Spine, 0.15f},
        {CharacterBone::Chest, 0.25f},
        {CharacterBone::Head, 0.60f},
    }};
    for (const auto& [bone, weight]: lookDistribution) {
        const RigNodeIndex node = Detail::Node(map, bone);
        if (IsValidRigNode(node, map.nodeCount)) {
            const JPH::Quat partial = JPH::Quat::sIdentity().SLERP(fullAim, weight * clampedWeight).Normalized();
            Detail::RotateSubtree(map, nodeTransforms, node, partial);
        }
    }
}

void SolveUpperBody(
    const ProceduralLocomotionComponent& gait,
    const ProceduralLookAtComponent*     lookAt,
    JPH::Mat44*                          nodeTransforms,
    const RigBoneMap&                    map
) noexcept {
    SolveUpperBody(gait, lookAt, JPH::Vec3::sZero(), JPH::Quat::sIdentity(), nodeTransforms, map, true, true);
}

} // namespace ZHLN::Animation
