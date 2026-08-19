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
#include <Zahlen/IK.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

module ZHLN.ProceduralAnimation;

namespace ZHLN::Animation {
namespace ItemDetail {

[[nodiscard]] float SmoothStep(float value) noexcept {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

[[nodiscard]] JPH::Vec3 SafeNormalized(JPH::Vec3Arg value, JPH::Vec3Arg fallback) noexcept {
    return value.LengthSq() > 1.0e-8f ? value.Normalized() : JPH::Vec3(fallback);
}

[[nodiscard]] JPH::Vec3 MatrixScale(const JPH::Mat44& matrix) noexcept {
    return JPH::Vec3(matrix.GetColumn3(0).Length(), matrix.GetColumn3(1).Length(), matrix.GetColumn3(2).Length());
}

[[nodiscard]] JPH::Quat MatrixRotation(const JPH::Mat44& matrix) noexcept {
    const JPH::Vec3 scale = MatrixScale(matrix);
    const JPH::Vec3 x     = scale.GetX() > 1.0e-6f ? matrix.GetColumn3(0) / scale.GetX() : JPH::Vec3::sAxisX();
    const JPH::Vec3 y     = scale.GetY() > 1.0e-6f ? matrix.GetColumn3(1) / scale.GetY() : JPH::Vec3::sAxisY();
    const JPH::Vec3 z     = scale.GetZ() > 1.0e-6f ? matrix.GetColumn3(2) / scale.GetZ() : JPH::Vec3::sAxisZ();
    return JPH::Mat44(JPH::Vec4(x, 0.0f), JPH::Vec4(y, 0.0f), JPH::Vec4(z, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)).GetQuaternion().Normalized();
}

[[nodiscard]] bool IsDescendant(const RigBoneMap& map, RigNodeIndex node, RigNodeIndex ancestor) noexcept {
    RigNodeIndex cursor = node;
    for (size_t depth = 0; depth < map.nodeCount && IsValidRigNode(cursor, map.nodeCount); ++depth) {
        if (cursor == ancestor) {
            return true;
        }
        cursor = map.parentIndices[cursor];
    }
    return false;
}

void RotateSubtreeAroundPivot(const RigBoneMap& map, JPH::Mat44* transforms, RigNodeIndex rootNode, JPH::Vec3Arg pivot, JPH::QuatArg rotation) noexcept {
    if (transforms == nullptr || !IsValidRigNode(rootNode, map.nodeCount)) {
        return;
    }
    for (RigNodeIndex node = 0; node < map.nodeCount; ++node) {
        if (!IsDescendant(map, node, rootNode)) {
            continue;
        }
        const JPH::Vec3 position    = pivot + rotation * (transforms[node].GetTranslation() - pivot);
        const JPH::Quat orientation = (rotation * MatrixRotation(transforms[node])).Normalized();
        transforms[node]            = JPH::Mat44::sRotationTranslation(orientation, position).PreScaled(MatrixScale(transforms[node]));
    }
}

void SpringVector(JPH::Vec3& value, JPH::Vec3& velocity, JPH::Vec3Arg target, float dt, float stiffness, float damping) noexcept {
    const float     safeDt             = std::clamp(dt, 0.0f, 0.05f);
    const float     safeStiffness      = std::max(stiffness, 0.0f);
    const float     dampingCoefficient = 2.0f * std::sqrt(safeStiffness) * std::max(damping, 0.0f);
    const float     f                  = 1.0f + safeDt * dampingCoefficient;
    const float     hoo                = safeDt * safeStiffness;
    const float     hhoo               = safeDt * hoo;
    const float     inverse            = 1.0f / (f + hhoo);
    const JPH::Vec3 old                = value;
    value                              = (value * f + velocity * safeDt + JPH::Vec3(target) * hhoo) * inverse;
    velocity                           = (velocity + (JPH::Vec3(target) - old) * hoo) * inverse;
}

void SpringRotation(JPH::Quat& value, JPH::Vec3& velocity, JPH::QuatArg target, float dt, float stiffness, float damping) noexcept {
    const float safeDt             = std::clamp(dt, 0.0f, 0.05f);
    const float safeStiffness      = std::max(stiffness, 0.0f);
    const float dampingCoefficient = 2.0f * std::sqrt(safeStiffness) * std::max(damping, 0.0f);
    JPH::Quat   error              = (target * value.Inversed()).Normalized();
    if (error.GetW() < 0.0f) {
        error = JPH::Quat(-error.GetX(), -error.GetY(), -error.GetZ(), -error.GetW());
    }
    JPH::Vec3 axis;
    float     angle = 0.0f;
    error.GetAxisAngle(axis, angle);
    const float denominator   = 1.0f + dampingCoefficient * safeDt + safeStiffness * safeDt * safeDt;
    velocity                  = (velocity + axis * (angle * safeStiffness * safeDt)) / denominator;
    const JPH::Vec3 step      = velocity * safeDt;
    const float     stepAngle = step.Length();
    if (stepAngle > 1.0e-7f) {
        value = (JPH::Quat::sRotation(step / stepAngle, std::min(stepAngle, std::numbers::pi_v<float>)) * value).Normalized();
    }
}

[[nodiscard]] float CurlForDigit(FingerDigit digit, const FingerCurlDesc& curl) noexcept {
    switch (digit) {
        case FingerDigit::Thumb:
            return curl.thumb;
        case FingerDigit::Index:
            return curl.index;
        case FingerDigit::Middle:
            return curl.middle;
        case FingerDigit::Ring:
            return curl.ring;
        case FingerDigit::Pinky:
            return curl.pinky;
    }
    return 0.0f;
}

} // namespace ItemDetail

JPH::Mat44 SolveItemBasePose(
    const ItemHandlingComponent& handling,
    const JPH::Mat44&            primaryHandModel,
    const JPH::Mat44&            chestModel,
    const JPH::Mat44&            worldToModel,
    JPH::Vec3Arg                 headPosModel,
    JPH::Vec3Arg                 aimDirModel,
    const JPH::Mat44&            worldAnchor
) noexcept {
    switch (handling.driverMode) {
        case ItemDriverMode::HandAnchored:
            return primaryHandModel;
        case ItemDriverMode::AimGuided: {
            const JPH::Quat aimRotation = JPH::Quat::sFromTo(JPH::Vec3::sAxisZ(), ItemDetail::SafeNormalized(aimDirModel, JPH::Vec3::sAxisZ()));
            const JPH::Vec3 hipPosition = JPH::Vec3(headPosModel) + aimRotation * handling.hipLocalOffset.GetTranslation();
            const JPH::Vec3 aimPosition = JPH::Vec3(headPosModel) + aimRotation * handling.aimLocalOffset.GetTranslation();
            const float     blend       = ItemDetail::SmoothStep(handling.aimProgress);
            return JPH::Mat44::sRotationTranslation(aimRotation, hipPosition + (aimPosition - hipPosition) * blend);
        }
        case ItemDriverMode::BodyMounted:
            return chestModel * handling.hipLocalOffset;
        case ItemDriverMode::WorldAnchored:
            return worldToModel * worldAnchor;
    }
    return JPH::Mat44::sIdentity();
}

float UpdateGripWeight(GripPoint& grip, float dt) noexcept {
    JPH::Vec3 scalar(grip.evaluatedIKWeight, 0.0f, 0.0f);
    JPH::Vec3 velocity(grip.ikWeightVelocity, 0.0f, 0.0f);
    ItemDetail::SpringVector(scalar, velocity, JPH::Vec3(std::clamp(grip.ikWeight, 0.0f, 1.0f), 0.0f, 0.0f), dt, 180.0f, 1.0f);
    grip.evaluatedIKWeight = std::clamp(scalar.GetX(), 0.0f, 1.0f);
    grip.ikWeightVelocity  = velocity.GetX();
    return grip.evaluatedIKWeight;
}

void UpdateItemDynamics(
    Engine&                engine,
    Entity                 characterEntity,
    ItemHandlingComponent& handling,
    JPH::Vec3Arg           rootPosition,
    JPH::QuatArg           rootRotation,
    float                  dt
) noexcept {
    const JPH::Mat44 rootWorld      = JPH::Mat44::sRotationTranslation(rootRotation, rootPosition);
    const JPH::Mat44 driverWorld    = rootWorld * handling.itemModelTransform;
    const JPH::Vec3  driverPosition = driverWorld.GetTranslation();
    const JPH::Quat  driverRotation = ItemDetail::MatrixRotation(driverWorld);
    const float      inertia        = std::clamp(handling.sway.massKg * 0.035f, 0.01f, 0.35f);
    if (handling.sway.driverInitialized) {
        const JPH::Vec3 localDriverDelta = driverRotation.Inversed() * (driverPosition - handling.sway.previousDriverPosition);
        handling.sway.positionOffset -= localDriverDelta * inertia;
        JPH::Quat delta = (driverRotation * handling.sway.previousDriverRotation.Inversed()).Normalized();
        if (delta.GetW() < 0.0f) {
            delta = JPH::Quat(-delta.GetX(), -delta.GetY(), -delta.GetZ(), -delta.GetW());
        }
        JPH::Vec3 axis;
        float     angle = 0.0f;
        delta.GetAxisAngle(axis, angle);
        if (angle > 1.0e-6f) {
            const JPH::Vec3 localAxis = driverRotation.Inversed() * ItemDetail::SafeNormalized(axis, JPH::Vec3::sAxisY());
            handling.sway.rotationOffset =
                (JPH::Quat::sRotation(ItemDetail::SafeNormalized(localAxis, JPH::Vec3::sAxisY()), -angle * inertia) * handling.sway.rotationOffset)
                    .Normalized();
        }
    }
    handling.sway.previousDriverPosition = driverPosition;
    handling.sway.previousDriverRotation = driverRotation;
    handling.sway.driverInitialized      = true;

    JPH::Vec3 obstaclePositionTarget = JPH::Vec3::sZero();
    JPH::Quat obstacleRotationTarget = JPH::Quat::sIdentity();
    if (handling.avoidance.probeDistance > 0.01f) {
        const JPH::Mat44 worldItem = rootWorld * handling.itemModelTransform;
        const JPH::Vec3  origin    = worldItem.GetTranslation();
        const JPH::Vec3  forward   = ItemDetail::SafeNormalized(worldItem.Multiply3x3(JPH::Vec3::sAxisZ()), rootRotation * JPH::Vec3::sAxisZ());
        Entity           ignoredPhysics {};
        if (const auto* physicsComponent = engine.GetRegistry().Get<Components::PhysicsComponent>(characterEntity)) {
            ignoredPhysics = physicsComponent->physicsHandle;
        }
        const auto hit = engine.GetPhysicsContext().Raycast(JPH::RVec3(origin), forward, handling.avoidance.probeDistance, ignoredPhysics);
        if (hit.hasHit && std::isfinite(hit.fraction)) {
            const float penetration = handling.avoidance.probeDistance * (1.0f - std::clamp(hit.fraction, 0.0f, 1.0f));
            const float pushback    = std::clamp(penetration * std::max(handling.avoidance.pushbackScale, 0.0f), 0.0f, handling.avoidance.probeDistance);
            const float tilt        = std::clamp(penetration * handling.avoidance.tiltScale, -JPH::DegreesToRadians(35.0f), JPH::DegreesToRadians(35.0f));
            obstaclePositionTarget  = JPH::Vec3(0.0f, 0.0f, -pushback);
            obstacleRotationTarget  = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), tilt);
        }
    }

    // Obstacle response is a bounded spring target, not a per-frame impulse.
    // Repeated overlap at fraction zero therefore converges to one pushback
    // distance instead of accumulating until the item leaves the scene.
    const float effectiveStiffness = std::max(handling.sway.stiffness, 0.0f) / std::max(handling.sway.massKg, 0.1f);
    ItemDetail::SpringVector(
        handling.sway.positionOffset, handling.sway.positionVelocity, obstaclePositionTarget, dt, effectiveStiffness, handling.sway.damping
    );
    ItemDetail::SpringRotation(
        handling.sway.rotationOffset, handling.sway.angularVelocity, obstacleRotationTarget, dt, effectiveStiffness, handling.sway.damping
    );

    const bool finitePosition = std::isfinite(handling.sway.positionOffset.GetX()) && std::isfinite(handling.sway.positionOffset.GetY()) &&
                                std::isfinite(handling.sway.positionOffset.GetZ());
    const bool finiteRotation = std::isfinite(handling.sway.rotationOffset.GetX()) && std::isfinite(handling.sway.rotationOffset.GetY()) &&
                                std::isfinite(handling.sway.rotationOffset.GetZ()) && std::isfinite(handling.sway.rotationOffset.GetW());
    if (!finitePosition || !finiteRotation) {
        handling.sway.positionOffset   = obstaclePositionTarget;
        handling.sway.positionVelocity = JPH::Vec3::sZero();
        handling.sway.rotationOffset   = obstacleRotationTarget;
        handling.sway.angularVelocity  = JPH::Vec3::sZero();
    }
    const float maximumOffset = std::max(handling.avoidance.probeDistance * 1.25f, 0.25f);
    if (handling.sway.positionOffset.LengthSq() > maximumOffset * maximumOffset) {
        handling.sway.positionOffset = handling.sway.positionOffset.Normalized() * maximumOffset;
        handling.sway.positionVelocity *= 0.25f;
    }
    handling.sway.rotationOffset = handling.sway.rotationOffset.Normalized();
    handling.itemModelTransform  = handling.itemModelTransform * JPH::Mat44::sRotationTranslation(handling.sway.rotationOffset, handling.sway.positionOffset);
}

void ConstrainItemToGripReach(ItemHandlingComponent& handling, const JPH::Mat44* nodeTransforms, const RigBoneMap& map) noexcept {
    if (nodeTransforms == nullptr || handling.driverMode == ItemDriverMode::WorldAnchored || handling.driverMode == ItemDriverMode::HandAnchored) {
        return;
    }

    const size_t gripCount = std::min(handling.gripCount, handling.grips.size());
    // A common item translation keeps all active grips coherent. Iterate a few
    // times because correcting the farther hand can expose a smaller error on
    // the opposite hand, but never solve each hand by stretching its arm.
    const size_t correctionPasses = std::max<size_t>(4, gripCount * 4);
    for (size_t pass = 0; pass < correctionPasses; ++pass) {
        float     greatestExcess   = 0.0f;
        float     correctionWeight = 0.0f;
        JPH::Vec3 correction       = JPH::Vec3::sZero();
        for (size_t gripIndex = 0; gripIndex < gripCount; ++gripIndex) {
            const GripPoint& grip   = handling.grips[gripIndex];
            const float      weight = std::clamp(grip.evaluatedIKWeight, 0.0f, 1.0f);
            if (weight <= 0.001f) {
                continue;
            }
            const bool         left      = grip.assignedLimb == CharacterBone::HandL;
            const RigNodeIndex upperNode = map.nodeIndices[BoneSlot(left ? CharacterBone::UpperArmL : CharacterBone::UpperArmR)];
            const RigNodeIndex foreNode  = map.nodeIndices[BoneSlot(left ? CharacterBone::ForearmL : CharacterBone::ForearmR)];
            const RigNodeIndex handNode  = map.nodeIndices[BoneSlot(left ? CharacterBone::HandL : CharacterBone::HandR)];
            if (!IsValidRigNode(upperNode, map.nodeCount) || !IsValidRigNode(foreNode, map.nodeCount) || !IsValidRigNode(handNode, map.nodeCount)) {
                continue;
            }
            const JPH::Vec3 upperPosition = nodeTransforms[upperNode].GetTranslation();
            const float     armLength     = (nodeTransforms[foreNode].GetTranslation() - upperPosition).Length() +
                                            (nodeTransforms[handNode].GetTranslation() - nodeTransforms[foreNode].GetTranslation()).Length();
            const JPH::Vec3 gripTarget    = (handling.itemModelTransform * grip.localTransform).GetTranslation();
            const JPH::Vec3 reachVector   = gripTarget - upperPosition;
            const float     distance      = reachVector.Length();
            const float     maximumReach  = armLength * std::clamp(grip.maxArmExtension, 0.50f, 0.9999f);
            const float     excess        = std::max(distance - maximumReach, 0.0f);
            greatestExcess                = std::max(greatestExcess, excess * weight);
            if (excess > 0.0f && distance > 1.0e-6f) {
                correction += -reachVector * (excess * weight / distance);
                correctionWeight += weight;
            }
        }
        if (greatestExcess <= 1.0e-5f || correctionWeight <= 1.0e-5f) {
            break;
        }
        handling.itemModelTransform.SetTranslation(handling.itemModelTransform.GetTranslation() + correction / correctionWeight);
    }
}

void ApplyClavicleLead(JPH::Mat44* nodeTransforms, const RigBoneMap& map, CharacterBone upperArmBone, JPH::Vec3Arg targetGripPos, float weight) noexcept {
    if (nodeTransforms == nullptr || weight <= 0.001f) {
        return;
    }
    const RigNodeIndex armNode = map.nodeIndices[BoneSlot(upperArmBone)];
    if (!IsValidRigNode(armNode, map.nodeCount)) {
        return;
    }
    const RigNodeIndex clavicleNode = map.parentIndices[armNode];
    if (!IsValidRigNode(clavicleNode, map.nodeCount) || clavicleNode == map.nodeIndices[BoneSlot(CharacterBone::Chest)] ||
        clavicleNode == map.nodeIndices[BoneSlot(CharacterBone::SupSpine)] || clavicleNode == map.nodeIndices[BoneSlot(CharacterBone::Spine)]) {
        return;
    }
    const JPH::Vec3 claviclePosition = nodeTransforms[clavicleNode].GetTranslation();
    const JPH::Vec3 currentDirection = ItemDetail::SafeNormalized(nodeTransforms[armNode].GetTranslation() - claviclePosition, JPH::Vec3::sAxisX());
    const JPH::Vec3 targetDirection  = ItemDetail::SafeNormalized(targetGripPos - claviclePosition, currentDirection);
    const JPH::Quat rotation = JPH::Quat::sIdentity().SLERP(JPH::Quat::sFromTo(currentDirection, targetDirection), std::clamp(weight, 0.0f, 1.0f)).Normalized();
    ItemDetail::RotateSubtreeAroundPivot(map, nodeTransforms, clavicleNode, claviclePosition, rotation);
}

void ApplyTorsoReachCompensation(
    JPH::Mat44*       nodeTransforms,
    const RigBoneMap& map,
    CharacterBone     upperArmBone,
    CharacterBone     forearmBone,
    CharacterBone     handBone,
    JPH::Vec3Arg      targetGripPos,
    float             weight
) noexcept {
    if (nodeTransforms == nullptr || weight <= 0.001f) {
        return;
    }
    const RigNodeIndex upperNode = map.nodeIndices[BoneSlot(upperArmBone)];
    const RigNodeIndex foreNode  = map.nodeIndices[BoneSlot(forearmBone)];
    const RigNodeIndex handNode  = map.nodeIndices[BoneSlot(handBone)];
    const RigNodeIndex spineNode = map.nodeIndices[BoneSlot(CharacterBone::Spine)];
    const RigNodeIndex hipsNode  = map.nodeIndices[BoneSlot(CharacterBone::Hips)];
    if (!IsValidRigNode(upperNode, map.nodeCount) || !IsValidRigNode(foreNode, map.nodeCount) || !IsValidRigNode(handNode, map.nodeCount) ||
        !IsValidRigNode(spineNode, map.nodeCount) || !IsValidRigNode(hipsNode, map.nodeCount)) {
        return;
    }
    const JPH::Vec3 upperPosition = nodeTransforms[upperNode].GetTranslation();
    const float     armLength     = (nodeTransforms[foreNode].GetTranslation() - upperPosition).Length() +
                                    (nodeTransforms[handNode].GetTranslation() - nodeTransforms[foreNode].GetTranslation()).Length();
    const float     excess        = std::max((targetGripPos - upperPosition).Length() - armLength * 0.90f, 0.0f);
    JPH::Vec3       horizontal    = targetGripPos - upperPosition;
    horizontal.SetY(0.0f);
    if (excess <= 0.0f || horizontal.LengthSq() <= 1.0e-6f) {
        return;
    }
    const JPH::Vec3 pivot     = nodeTransforms[hipsNode].GetTranslation();
    const float     lever     = std::max((nodeTransforms[spineNode].GetTranslation() - pivot).Length(), 0.20f);
    const float     angle     = std::min(std::atan2(excess, lever) * std::clamp(weight, 0.0f, 1.0f), JPH::DegreesToRadians(12.0f));
    const JPH::Vec3 direction = horizontal.Normalized();
    const JPH::Quat pitch     = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), direction.GetZ() * angle);
    const JPH::Quat roll      = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), -direction.GetX() * angle);
    ItemDetail::RotateSubtreeAroundPivot(map, nodeTransforms, spineNode, pivot, (roll * pitch).Normalized());
}

JPH::Quat ConstrainWristRotation(
    JPH::QuatArg targetRotation,
    JPH::QuatArg authoredRotation,
    JPH::Vec3Arg twistAxis,
    float        maxTwistRadians,
    float        maxSwingRadians
) noexcept {
    const JPH::Vec3 axis  = ItemDetail::SafeNormalized(twistAxis, JPH::Vec3::sAxisZ());
    JPH::Quat       delta = (targetRotation * authoredRotation.Inversed()).Normalized();
    if (delta.GetW() < 0.0f) {
        delta = JPH::Quat(-delta.GetX(), -delta.GetY(), -delta.GetZ(), -delta.GetW());
    }
    const JPH::Vec3 vector(delta.GetX(), delta.GetY(), delta.GetZ());
    const JPH::Vec3 projected = axis * vector.Dot(axis);
    JPH::Quat       twist(projected.GetX(), projected.GetY(), projected.GetZ(), delta.GetW());
    const float     twistLengthSq = projected.LengthSq() + delta.GetW() * delta.GetW();
    twist                         = twistLengthSq > 1.0e-8f ? twist.Normalized() : JPH::Quat::sIdentity();
    JPH::Quat swing               = (delta * twist.Inversed()).Normalized();

    auto clampRotation = [](JPH::QuatArg value, JPH::Vec3Arg fallbackAxis, float maximum) {
        JPH::Vec3 rotationAxis;
        float     angle = 0.0f;
        JPH::Quat(value).GetAxisAngle(rotationAxis, angle);
        if (angle > std::numbers::pi_v<float>) {
            angle -= 2.0f * std::numbers::pi_v<float>;
        }
        const float clamped = std::clamp(angle, -std::abs(maximum), std::abs(maximum));
        return std::abs(clamped) > 1.0e-6f ? JPH::Quat::sRotation(ItemDetail::SafeNormalized(rotationAxis, fallbackAxis), clamped) : JPH::Quat::sIdentity();
    };

    const JPH::Quat limitedTwist = clampRotation(twist, axis, maxTwistRadians);
    const JPH::Quat limitedSwing = clampRotation(swing, JPH::Vec3::sAxisY(), maxSwingRadians);
    return (limitedSwing * limitedTwist * authoredRotation).Normalized();
}

void SolveLimbIK(
    JPH::Mat44*       nodeTransforms,
    const RigBoneMap& map,
    CharacterBone     upperBone,
    CharacterBone     foreBone,
    CharacterBone     handBone,
    const JPH::Mat44& targetGripTransform,
    const GripPoint&  grip
) noexcept {
    const float solveWeight = std::clamp(grip.evaluatedIKWeight, 0.0f, 1.0f);
    if (nodeTransforms == nullptr || solveWeight <= 0.001f) {
        return;
    }
    const RigNodeIndex upperNode = map.nodeIndices[BoneSlot(upperBone)];
    const RigNodeIndex foreNode  = map.nodeIndices[BoneSlot(foreBone)];
    const RigNodeIndex handNode  = map.nodeIndices[BoneSlot(handBone)];
    if (!IsValidRigNode(upperNode, map.nodeCount) || !IsValidRigNode(foreNode, map.nodeCount) || !IsValidRigNode(handNode, map.nodeCount)) {
        return;
    }

    const JPH::Vec3 upperPosition  = nodeTransforms[upperNode].GetTranslation();
    const JPH::Vec3 forePosition   = nodeTransforms[foreNode].GetTranslation();
    const JPH::Vec3 handPosition   = nodeTransforms[handNode].GetTranslation();
    const float     upperLength    = std::max((forePosition - upperPosition).Length(), 0.001f);
    const float     lowerLength    = std::max((handPosition - forePosition).Length(), 0.001f);
    const JPH::Vec3 rawTarget      = targetGripTransform.GetTranslation();
    const JPH::Vec3 targetPosition = handPosition + (rawTarget - handPosition) * ItemDetail::SmoothStep(solveWeight);

    const JPH::Vec3 aimAxis       = ItemDetail::SafeNormalized(targetPosition - upperPosition, JPH::Vec3(0.0f, -1.0f, 0.0f));
    const JPH::Vec3 authoredPlane = (forePosition - upperPosition) - aimAxis * (forePosition - upperPosition).Dot(aimAxis);
    const JPH::Vec3 authoredPole  = ItemDetail::SafeNormalized(authoredPlane, JPH::Vec3::sAxisZ());
    const JPH::Vec3 hintPole      = ItemDetail::SafeNormalized(targetGripTransform.Multiply3x3(grip.poleHintOffset), authoredPole);
    const JPH::Vec3 pole          = ItemDetail::SafeNormalized(authoredPole * 0.4f + hintPole * 0.6f, authoredPole);

    const auto ik = IK::SolveTwoBoneIK({
        .upperPosition  = upperPosition,
        .targetPosition = targetPosition,
        .poleVector     = pole,
        .upperLength    = upperLength,
        .lowerLength    = lowerLength,
        .maxExtension   = grip.maxArmExtension,
    });
    if (!ik.valid) {
        return;
    }

    const JPH::Vec3  currentUpperDirection = ItemDetail::SafeNormalized(forePosition - upperPosition, JPH::Vec3(0.0f, -1.0f, 0.0f));
    const JPH::Vec3  currentLowerDirection = ItemDetail::SafeNormalized(handPosition - forePosition, JPH::Vec3(0.0f, -1.0f, 0.0f));
    const JPH::Mat44 solvedUpper           = CorrectBoneDirection(nodeTransforms[upperNode], currentUpperDirection, ik.upperDirection, upperPosition);
    JPH::Mat44       solvedFore            = CorrectBoneDirection(nodeTransforms[foreNode], currentLowerDirection, ik.lowerDirection, ik.midPosition);

    JPH::Quat targetPalmRotation = ItemDetail::MatrixRotation(targetGripTransform);
    if (grip.orientationMode == GripOrientationMode::AutomaticHanded && handBone == CharacterBone::HandL) {
        // Preserve grip-local +Z as hand/finger forward while mirroring +X so
        // the left palm faces inward instead of copying the right-hand side.
        targetPalmRotation = (targetPalmRotation * JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), std::numbers::pi_v<float>)).Normalized();
    }
    const size_t    palmSide = handBone == CharacterBone::HandL ? 0u : 1u;
    const JPH::Quat targetHandRotation =
        map.handPalmFramesValid[palmSide] ? (targetPalmRotation * map.handBoneToPalmRotations[palmSide].Inversed()).Normalized() : targetPalmRotation;
    const JPH::Quat authoredHandRotation = ItemDetail::MatrixRotation(nodeTransforms[handNode]);

    // Share palm-facing roll with the forearm before applying the tighter wrist
    // cone. This lets a palm turn toward the item instead of remaining ground-
    // facing merely because the required roll exceeds the wrist swing limit.
    const JPH::Vec3 rollAxis          = ItemDetail::SafeNormalized(ik.lowerDirection, currentLowerDirection);
    const JPH::Quat authoredPalmFrame = map.handPalmFramesValid[palmSide] ? (authoredHandRotation * map.handBoneToPalmRotations[palmSide]).Normalized() :
                                                                            authoredHandRotation;
    JPH::Vec3       currentPalmNormal = authoredPalmFrame * JPH::Vec3::sAxisX();
    JPH::Vec3       targetPalmNormal  = targetPalmRotation * JPH::Vec3::sAxisX();
    currentPalmNormal -= rollAxis * currentPalmNormal.Dot(rollAxis);
    targetPalmNormal -= rollAxis * targetPalmNormal.Dot(rollAxis);
    JPH::Quat forearmRoll = JPH::Quat::sIdentity();
    if (currentPalmNormal.LengthSq() > 1.0e-8f && targetPalmNormal.LengthSq() > 1.0e-8f) {
        currentPalmNormal      = currentPalmNormal.Normalized();
        targetPalmNormal       = targetPalmNormal.Normalized();
        const float signedRoll = std::atan2(rollAxis.Dot(currentPalmNormal.Cross(targetPalmNormal)), currentPalmNormal.Dot(targetPalmNormal));
        const float rollLimit  = JPH::DegreesToRadians(std::abs(grip.maxForearmTwistDeg));
        const float rollAngle  = std::clamp(signedRoll * std::clamp(grip.forearmTwistWeight * solveWeight, 0.0f, 1.0f), -rollLimit, rollLimit);
        forearmRoll            = JPH::Quat::sRotation(rollAxis, rollAngle);
        solvedFore = JPH::Mat44::sRotationTranslation((forearmRoll * ItemDetail::MatrixRotation(solvedFore)).Normalized(), solvedFore.GetTranslation())
                         .PreScaled(ItemDetail::MatrixScale(solvedFore));
    }

    const JPH::Quat carriedHandRotation = (forearmRoll * authoredHandRotation).Normalized();
    const JPH::Quat constrainedRotation = ConstrainWristRotation(
        targetHandRotation, carriedHandRotation, ik.lowerDirection, JPH::DegreesToRadians(grip.maxWristTwistDeg), JPH::DegreesToRadians(grip.maxWristSwingDeg)
    );
    const float      rotationBlend = std::clamp(grip.rotationWeight * solveWeight, 0.0f, 1.0f);
    const JPH::Quat  handRotation  = carriedHandRotation.SLERP(constrainedRotation, rotationBlend).Normalized();
    const JPH::Mat44 solvedHand = JPH::Mat44::sRotationTranslation(handRotation, ik.endPosition).PreScaled(ItemDetail::MatrixScale(nodeTransforms[handNode]));

    SetModelTransformAndCarrySubtree(nodeTransforms, map, upperNode, solvedUpper);
    SetModelTransformAndCarrySubtree(nodeTransforms, map, foreNode, solvedFore);
    SetModelTransformAndCarrySubtree(nodeTransforms, map, handNode, solvedHand);
}

FingerCurlDesc EvaluateFingerCurl(const GraspDesc& grasp) noexcept {
    FingerCurlDesc curl;
    switch (grasp.shape) {
        case GraspShape::Cylinder: {
            const float value = std::clamp(1.0f - grasp.gripRadius / 0.08f, 0.25f, 1.0f) * std::clamp(grasp.tightness, 0.0f, 1.0f);
            curl              = {.thumb = value * 0.85f, .index = value, .middle = value, .ring = value, .pinky = value};
            break;
        }
        case GraspShape::TriggerGrip: {
            const float value = std::clamp(1.0f - grasp.gripRadius / 0.08f, 0.25f, 1.0f) * std::clamp(grasp.tightness, 0.0f, 1.0f);
            curl              = {.thumb = value * 0.85f, .index = std::clamp(grasp.triggerCurl, 0.0f, 1.0f), .middle = value, .ring = value, .pinky = value};
            break;
        }
        case GraspShape::FlatPalm:
            curl = {.thumb = 0.12f, .index = 0.05f, .middle = 0.05f, .ring = 0.05f, .pinky = 0.05f};
            break;
        case GraspShape::Pinch:
            curl = {.thumb = 0.82f, .index = 0.85f, .middle = 0.65f, .ring = 0.20f, .pinky = 0.10f};
            break;
        case GraspShape::RelaxedOpen:
            curl = {.thumb = 0.20f, .index = 0.25f, .middle = 0.28f, .ring = 0.30f, .pinky = 0.32f};
            break;
    }
    return curl;
}

JPH::Quat ConstrainFingerHingeRotation(
    JPH::QuatArg authoredRotation,
    JPH::QuatArg desiredRotation,
    JPH::Vec3Arg hingeAxis,
    float        flexSign,
    float        maxFlexRadians,
    float        maxExtensionRadians
) noexcept {
    const JPH::Vec3 axis  = ItemDetail::SafeNormalized(hingeAxis, JPH::Vec3::sAxisX());
    JPH::Quat       delta = (desiredRotation * authoredRotation.Inversed()).Normalized();
    if (delta.GetW() < 0.0f) {
        delta = JPH::Quat(-delta.GetX(), -delta.GetY(), -delta.GetZ(), -delta.GetW());
    }
    const JPH::Vec3 vector(delta.GetX(), delta.GetY(), delta.GetZ());
    const JPH::Vec3 projected = axis * vector.Dot(axis);
    const float     lengthSq  = projected.LengthSq() + delta.GetW() * delta.GetW();
    JPH::Quat twist = lengthSq > 1.0e-8f ? JPH::Quat(projected.GetX(), projected.GetY(), projected.GetZ(), delta.GetW()).Normalized() : JPH::Quat::sIdentity();
    JPH::Vec3 twistAxis;
    float     angle = 0.0f;
    twist.GetAxisAngle(twistAxis, angle);
    if (angle > std::numbers::pi_v<float>) {
        angle -= 2.0f * std::numbers::pi_v<float>;
    }
    if (twistAxis.Dot(axis) < 0.0f) {
        angle = -angle;
    }
    const float flex      = std::abs(maxFlexRadians);
    const float extension = std::abs(maxExtensionRadians);
    const float clamped   = flexSign >= 0.0f ? std::clamp(angle, -extension, flex) : std::clamp(angle, -flex, extension);
    return (JPH::Quat::sRotation(axis, clamped) * authoredRotation).Normalized();
}

void ApplyKinematicFingers(
    JPH::Mat44*           nodeTransforms,
    const RigBoneMap&     map,
    CharacterBone         handBone,
    const FingerCurlDesc& curl,
    float                 weight,
    FingerCurlAxisMode    axisMode
) noexcept {
    if (nodeTransforms == nullptr || weight <= 0.001f) {
        return;
    }
    const RigNodeIndex handNode = map.nodeIndices[BoneSlot(handBone)];
    if (!IsValidRigNode(handNode, map.nodeCount)) {
        return;
    }
    const size_t side = handBone == CharacterBone::HandL ? 0u : 1u;

    for (size_t index = 0; index < map.fingerJointConstraintCount; ++index) {
        const RigFingerJointConstraint& joint = map.fingerJointConstraints[index];
        if (joint.side != side || !IsValidRigNode(joint.child, map.nodeCount)) {
            continue;
        }
        const float      fingerCurl   = ItemDetail::CurlForDigit(joint.digit, curl);
        const JPH::Mat44 current      = nodeTransforms[joint.child];
        const JPH::Vec3  axis         = ItemDetail::SafeNormalized(current.Multiply3x3(JPH::Vec3::sAxisX()), JPH::Vec3::sAxisX());
        const bool       isThumb      = joint.digit == FingerDigit::Thumb;
        const float      maximumAngle = isThumb ? JPH::DegreesToRadians(50.0f) : JPH::DegreesToRadians(62.0f);
        float            curlSign     = -1.0f;
        if (axisMode == FingerCurlAxisMode::MirroredLocalX && handBone == CharacterBone::HandL) {
            curlSign = 1.0f;
        } else if (axisMode == FingerCurlAxisMode::AutomaticPalm) {
            curlSign = joint.hingeFlexSign;
        }

        const float      desiredAngle        = curlSign * maximumAngle * std::clamp(fingerCurl * weight, 0.0f, 1.0f);
        const JPH::Quat  authoredRotation    = ItemDetail::MatrixRotation(current);
        const JPH::Quat  desiredRotation     = (JPH::Quat::sRotation(axis, desiredAngle) * authoredRotation).Normalized();
        const JPH::Quat  constrainedRotation = ConstrainFingerHingeRotation(authoredRotation, desiredRotation, axis, curlSign, maximumAngle, 0.0f);
        const JPH::Mat44 target = JPH::Mat44::sRotationTranslation(constrainedRotation, current.GetTranslation()).PreScaled(ItemDetail::MatrixScale(current));
        SetModelTransformAndCarrySubtree(nodeTransforms, map, joint.child, target);
    }
}
} // namespace ZHLN::Animation
