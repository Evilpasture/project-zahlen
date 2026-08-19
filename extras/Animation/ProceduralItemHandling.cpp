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
#include <array>
#include <cmath>
#include <numbers>
#include <string_view>

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

[[nodiscard]] std::array<char, 96> Canonicalize(std::string_view name) noexcept {
    std::array<char, 96> output {};
    size_t               write = 0;
    for (char c: name) {
        if (write + 1 >= output.size()) {
            break;
        }
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            output[write++] = c;
        }
    }
    output[write] = '\0';
    return output;
}

[[nodiscard]] float CurlForName(std::string_view name, const FingerCurlDesc& curl) noexcept {
    const auto             canonicalStorage = Canonicalize(name);
    const std::string_view canonical(canonicalStorage.data());
    if (canonical.find("thumb") != std::string_view::npos) {
        return curl.thumb;
    }
    if (canonical.find("index") != std::string_view::npos) {
        return curl.index;
    }
    if (canonical.find("middle") != std::string_view::npos) {
        return curl.middle;
    }
    if (canonical.find("ring") != std::string_view::npos) {
        return curl.ring;
    }
    if (canonical.find("pinky") != std::string_view::npos || canonical.find("little") != std::string_view::npos) {
        return curl.pinky;
    }
    return -1.0f;
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
    const JPH::Mat44 solvedFore            = CorrectBoneDirection(nodeTransforms[foreNode], currentLowerDirection, ik.lowerDirection, ik.midPosition);

    const JPH::Quat targetHandRotation   = ItemDetail::MatrixRotation(targetGripTransform);
    const JPH::Quat authoredHandRotation = ItemDetail::MatrixRotation(nodeTransforms[handNode]);
    const JPH::Quat constrainedRotation  = ConstrainWristRotation(
        targetHandRotation, authoredHandRotation, ik.lowerDirection, JPH::DegreesToRadians(grip.maxWristTwistDeg), JPH::DegreesToRadians(grip.maxWristSwingDeg)
    );
    const float      rotationBlend = std::clamp(grip.rotationWeight * solveWeight, 0.0f, 1.0f);
    const JPH::Quat  handRotation  = authoredHandRotation.SLERP(constrainedRotation, rotationBlend).Normalized();
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

void ApplyKinematicFingers(
    JPH::Mat44*           nodeTransforms,
    const RigBoneMap&     map,
    CharacterBone         handBone,
    const FingerCurlDesc& curl,
    float                 weight,
    FingerCurlAxisMode    axisMode
) noexcept {
    if (nodeTransforms == nullptr || map.sourcePrefab == nullptr || weight <= 0.001f) {
        return;
    }
    const RigNodeIndex handNode = map.nodeIndices[BoneSlot(handBone)];
    if (!IsValidRigNode(handNode, map.nodeCount)) {
        return;
    }
    const float curlSign = axisMode == FingerCurlAxisMode::MirroredLocalX && handBone == CharacterBone::HandL ? 1.0f : -1.0f;

    for (size_t desiredDepth = 1; desiredDepth <= 5; ++desiredDepth) {
        for (RigNodeIndex node = 0; node < map.nodeCount && node < map.sourcePrefab->nodes.size(); ++node) {
            if (node == handNode || !ItemDetail::IsDescendant(map, node, handNode)) {
                continue;
            }
            size_t       depth  = 0;
            RigNodeIndex cursor = node;
            while (depth <= desiredDepth && IsValidRigNode(cursor, map.nodeCount) && cursor != handNode) {
                cursor = map.parentIndices[cursor];
                ++depth;
            }
            if (cursor != handNode || depth != desiredDepth) {
                continue;
            }
            const float fingerCurl = ItemDetail::CurlForName(std::string_view(map.sourcePrefab->nodes[node].name), curl);
            if (fingerCurl < 0.0f) {
                continue;
            }
            const JPH::Mat44 current       = nodeTransforms[node];
            const JPH::Vec3  axis          = ItemDetail::SafeNormalized(current.Multiply3x3(JPH::Vec3::sAxisX()), JPH::Vec3::sAxisX());
            const auto       canonicalName = ItemDetail::Canonicalize(std::string_view(map.sourcePrefab->nodes[node].name));
            const bool       isThumb       = std::string_view(canonicalName.data()).find("thumb") != std::string_view::npos;
            const float      maximumAngle  = isThumb ? JPH::DegreesToRadians(50.0f) : JPH::DegreesToRadians(62.0f);
            const JPH::Quat  rotation      = JPH::Quat::sRotation(axis, curlSign * maximumAngle * std::clamp(fingerCurl * weight, 0.0f, 1.0f));
            const JPH::Mat44 target = JPH::Mat44::sRotationTranslation((rotation * ItemDetail::MatrixRotation(current)).Normalized(), current.GetTranslation())
                                          .PreScaled(ItemDetail::MatrixScale(current));
            SetModelTransformAndCarrySubtree(nodeTransforms, map, node, target);
        }
    }
}

} // namespace ZHLN::Animation
