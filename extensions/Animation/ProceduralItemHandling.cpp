// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// clang-format off
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Animation/ProceduralMath.hpp>
// clang-format on
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Animation/IK.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

module ZHLN.ProceduralAnimation;

namespace ZHLN::Animation {

// Anonymous, not `namespace ItemDetail`: this is an implementation unit of
// ZHLN.ProceduralAnimation, so nothing here is reachable from outside it, and
// six of these helpers -- SafeNormalized, SmoothStep, MatrixRotation,
// MatrixScale, IsDescendant and RotateSubtreeAroundPivot -- are defined again in
// the module's ProceduralGait unit. Internal linkage, not a namespace, is what
// keeps two definitions of one name from meeting at link time.
namespace {

[[nodiscard]] float SmoothStep(float value) noexcept {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}

[[nodiscard]] FVec3 SafeNormalized(FVec3 value, FVec3 fallback) noexcept {
    return value.LengthSquared() > 1.0e-8f ? value.Normalized() : FVec3(fallback);
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

void RotateSubtreeAroundPivot(const RigBoneMap& map, Mat4x4* transforms, RigNodeIndex rootNode, FVec3 pivot, Quat rotation) noexcept {
    if (transforms == nullptr || !IsValidRigNode(rootNode, map.nodeCount)) {
        return;
    }
    for (RigNodeIndex node = 0; node < map.nodeCount; ++node) {
        if (!IsDescendant(map, node, rootNode)) {
            continue;
        }
        const FVec3 position    = pivot + rotation * (ProceduralMath::Translation(transforms[node]) - pivot);
        const Quat orientation = (rotation * MatrixRotation(transforms[node])).Normalized();
        transforms[node]            = ProceduralMath::Transform(orientation, position, MatrixScale(transforms[node]));
    }
}

void SpringVector(FVec3& value, FVec3& velocity, FVec3 target, float dt, float stiffness, float damping) noexcept {
    const float     safeDt             = std::clamp(dt, 0.0f, 0.05f);
    const float     safeStiffness      = std::max(stiffness, 0.0f);
    const float     dampingCoefficient = 2.0f * std::sqrt(safeStiffness) * std::max(damping, 0.0f);
    const float     f                  = 1.0f + safeDt * dampingCoefficient;
    const float     hoo                = safeDt * safeStiffness;
    const float     hhoo               = safeDt * hoo;
    const float     inverse            = 1.0f / (f + hhoo);
    const FVec3 old                = value;
    value                              = (value * f + velocity * safeDt + FVec3(target) * hhoo) * inverse;
    velocity                           = (velocity + (FVec3(target) - old) * hoo) * inverse;
}

void SpringRotation(Quat& value, FVec3& velocity, Quat target, float dt, float stiffness, float damping) noexcept {
    const float safeDt             = std::clamp(dt, 0.0f, 0.05f);
    const float safeStiffness      = std::max(stiffness, 0.0f);
    const float dampingCoefficient = 2.0f * std::sqrt(safeStiffness) * std::max(damping, 0.0f);
    Quat   error              = (target * value.Inversed()).Normalized();
    if (error.w < 0.0f) {
        error = -error;
    }
    FVec3 axis;
    float     angle = 0.0f;
    error.ToAxisAngle(axis, angle);
    const float denominator   = 1.0f + dampingCoefficient * safeDt + safeStiffness * safeDt * safeDt;
    velocity                  = (velocity + axis * (angle * safeStiffness * safeDt)) / denominator;
    const FVec3 step      = velocity * safeDt;
    const float     stepAngle = step.Length();
    if (stepAngle > 1.0e-7f) {
        value = (Quat::FromAxisAngle(step / stepAngle, std::min(stepAngle, std::numbers::pi_v<float>)) * value).Normalized();
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

} // namespace

Mat4x4 SolveItemBasePose(
    const ItemHandlingComponent& handling,
    const Mat4x4&            primaryHandModel,
    const Mat4x4&            chestModel,
    const Mat4x4&            worldToModel,
    FVec3                 headPosModel,
    FVec3                 aimDirModel,
    const Mat4x4&            worldAnchor
) noexcept {
    switch (handling.driverMode) {
        case ItemDriverMode::HandAnchored:
            return primaryHandModel;
        case ItemDriverMode::AimGuided: {
            const Quat aimRotation = FromTo(FVec3::UnitZ(), SafeNormalized(aimDirModel, FVec3::UnitZ()));
            const FVec3 hipPosition = FVec3(headPosModel) + aimRotation * ProceduralMath::Translation(Mat4x4(handling.hipLocalOffset));
            const FVec3 aimPosition = FVec3(headPosModel) + aimRotation * ProceduralMath::Translation(Mat4x4(handling.aimLocalOffset));
            const float     blend       = SmoothStep(handling.aimProgress);
            return ProceduralMath::Transform(aimRotation, hipPosition + (aimPosition - hipPosition) * blend);
        }
        case ItemDriverMode::BodyMounted:
            return chestModel * handling.hipLocalOffset;
        case ItemDriverMode::WorldAnchored:
            return worldToModel * worldAnchor;
    }
    return Mat4x4::Identity();
}

float UpdateGripWeight(GripPoint& grip, float dt) noexcept {
    FVec3 scalar(grip.evaluatedIKWeight, 0.0f, 0.0f);
    FVec3 velocity(grip.ikWeightVelocity, 0.0f, 0.0f);
    SpringVector(scalar, velocity, FVec3(std::clamp(grip.ikWeight, 0.0f, 1.0f), 0.0f, 0.0f), dt, 180.0f, 1.0f);
    grip.evaluatedIKWeight = std::clamp(scalar.x, 0.0f, 1.0f);
    grip.ikWeightVelocity  = velocity.x;
    return grip.evaluatedIKWeight;
}

void UpdateItemDynamics(
    const ECS::Registry&   registry,
    const PhysicsContext&  physics,
    Entity                 characterEntity,
    ItemHandlingComponent& handling,
    FVec3           rootPosition,
    Quat           rootRotation,
    float                  dt
) noexcept {
    const Mat4x4 rootWorld      = ProceduralMath::Transform(rootRotation, rootPosition);
    const Mat4x4 driverWorld    = rootWorld * handling.itemModelTransform;
    const FVec3  driverPosition = ProceduralMath::Translation(driverWorld);
    const Quat  driverRotation = MatrixRotation(driverWorld);
    const float      inertia        = std::clamp(handling.sway.massKg * 0.035f, 0.01f, 0.35f);
    if (handling.sway.driverInitialized) {
        const FVec3 localDriverDelta = driverRotation.Inversed() * (driverPosition - handling.sway.previousDriverPosition);
        handling.sway.positionOffset           = FVec3(handling.sway.positionOffset) - localDriverDelta * inertia;
        const Quat previousDriverRotation = handling.sway.previousDriverRotation;
        Quat       delta                  = (driverRotation * previousDriverRotation.Inversed()).Normalized();
        if (delta.w < 0.0f) {
            delta = -delta;
        }
        FVec3 axis;
        float     angle = 0.0f;
        delta.ToAxisAngle(axis, angle);
        if (angle > 1.0e-6f) {
            const FVec3 localAxis = driverRotation.Inversed() * SafeNormalized(axis, FVec3::UnitY());
            handling.sway.rotationOffset =
                (Quat::FromAxisAngle(SafeNormalized(localAxis, FVec3::UnitY()), -angle * inertia) * handling.sway.rotationOffset).Normalized();
        }
    }
    handling.sway.previousDriverPosition = driverPosition;
    handling.sway.previousDriverRotation = driverRotation;
    handling.sway.driverInitialized      = true;

    FVec3 obstaclePositionTarget = FVec3::Zero();
    Quat obstacleRotationTarget = Quat::Identity();
    if (handling.avoidance.probeDistance > 0.01f) {
        const Mat4x4 worldItem = rootWorld * handling.itemModelTransform;
        const FVec3  origin    = ProceduralMath::Translation(worldItem);
        const FVec3  forward   = SafeNormalized(ProceduralMath::Direction(worldItem, FVec3::UnitZ()), rootRotation * FVec3::UnitZ());
        Physics::BodyID ignoredPhysics {};
        if (const auto physicsComponent = registry.Get<Components::PhysicsComponent>(characterEntity)) {
            ignoredPhysics = physicsComponent->physicsID;
        }
        const auto hit = physics.Raycast(origin.Cast<double>(), forward, handling.avoidance.probeDistance, ignoredPhysics);
        if (hit.hasHit && std::isfinite(hit.fraction)) {
            const float penetration = handling.avoidance.probeDistance * (1.0f - std::clamp(hit.fraction, 0.0f, 1.0f));
            const float pushback    = std::clamp(penetration * std::max(handling.avoidance.pushbackScale, 0.0f), 0.0f, handling.avoidance.probeDistance);
            const float tilt        = std::clamp(penetration * handling.avoidance.tiltScale, -ZHLN::DegF(35.0f).ToRadians().value, ZHLN::DegF(35.0f).ToRadians().value);
            obstaclePositionTarget  = FVec3(0.0f, 0.0f, -pushback);
            obstacleRotationTarget  = Quat::FromAxisAngle(FVec3::UnitX(), tilt);
        }
    }

    // Obstacle response is a bounded spring target, not a per-frame impulse.
    // Repeated overlap at fraction zero therefore converges to one pushback
    // distance instead of accumulating until the item leaves the scene.
    const float effectiveStiffness = std::max(handling.sway.stiffness, 0.0f) / std::max(handling.sway.massKg, 0.1f);
    // The springs write through their addresses, so the state is loaded, stepped
    // and stored: a member converts on assignment, never to a FVec3&.
    FVec3 swayOffset          = handling.sway.positionOffset;
    FVec3 swayVelocity        = handling.sway.positionVelocity;
    Quat swayRotation        = handling.sway.rotationOffset;
    FVec3 swayAngularVelocity = handling.sway.angularVelocity;
    SpringVector(swayOffset, swayVelocity, obstaclePositionTarget, dt, effectiveStiffness, handling.sway.damping);
    SpringRotation(swayRotation, swayAngularVelocity, obstacleRotationTarget, dt, effectiveStiffness, handling.sway.damping);
    handling.sway.positionOffset   = swayOffset;
    handling.sway.positionVelocity = swayVelocity;
    handling.sway.rotationOffset   = swayRotation;
    handling.sway.angularVelocity  = swayAngularVelocity;

    const bool finitePosition = std::isfinite(handling.sway.positionOffset.x) && std::isfinite(handling.sway.positionOffset.y) &&
                                std::isfinite(handling.sway.positionOffset.z);
    const bool finiteRotation = std::isfinite(handling.sway.rotationOffset.x) && std::isfinite(handling.sway.rotationOffset.y) &&
                                std::isfinite(handling.sway.rotationOffset.z) && std::isfinite(handling.sway.rotationOffset.w);
    if (!finitePosition || !finiteRotation) {
        handling.sway.positionOffset   = obstaclePositionTarget;
        handling.sway.positionVelocity = FVec3::Zero();
        handling.sway.rotationOffset   = obstacleRotationTarget;
        handling.sway.angularVelocity  = FVec3::Zero();
    }
    // Clamp against the stored values: the NaN guard above may have replaced them.
    const float maximumOffset = std::max(handling.avoidance.probeDistance * 1.25f, 0.25f);
    swayOffset                = handling.sway.positionOffset;
    if (swayOffset.LengthSquared() > maximumOffset * maximumOffset) {
        swayOffset                     = swayOffset.Normalized() * maximumOffset;
        handling.sway.positionOffset   = swayOffset;
        handling.sway.positionVelocity = FVec3(handling.sway.positionVelocity) * 0.25f;
    }
    swayRotation                 = handling.sway.rotationOffset.Normalized();
    const Mat4x4 itemModel   = handling.itemModelTransform;
    handling.sway.rotationOffset = swayRotation;
    handling.itemModelTransform  = itemModel * ProceduralMath::Transform(swayRotation, handling.sway.positionOffset);
}

void ConstrainItemToGripReach(ItemHandlingComponent& handling, const Mat4x4* nodeTransforms, const RigBoneMap& map) noexcept {
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
        FVec3 correction       = FVec3::Zero();
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
            const FVec3 upperPosition = ProceduralMath::Translation(nodeTransforms[upperNode]);
            const float     armLength     = (ProceduralMath::Translation(nodeTransforms[foreNode]) - upperPosition).Length() +
                                            (ProceduralMath::Translation(nodeTransforms[handNode]) - ProceduralMath::Translation(nodeTransforms[foreNode])).Length();
            const Mat4x4 itemModel     = handling.itemModelTransform;
            const FVec3  gripTarget    = ProceduralMath::Translation((itemModel * grip.localTransform));
            const FVec3 reachVector   = gripTarget - upperPosition;
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
        Mat4x4 itemModel = handling.itemModelTransform;
        ProceduralMath::SetTranslation(itemModel, ProceduralMath::Translation(itemModel) + correction / correctionWeight);
        handling.itemModelTransform = itemModel;
    }
}

void ApplyClavicleLead(Mat4x4* nodeTransforms, const RigBoneMap& map, CharacterBone upperArmBone, FVec3 targetGripPos, float weight) noexcept {
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
    const FVec3 claviclePosition = ProceduralMath::Translation(nodeTransforms[clavicleNode]);
    const FVec3 currentDirection = SafeNormalized(ProceduralMath::Translation(nodeTransforms[armNode]) - claviclePosition, FVec3::UnitX());
    const FVec3 targetDirection  = SafeNormalized(targetGripPos - claviclePosition, currentDirection);
    const Quat rotation = Slerp(Quat::Identity(), FromTo(currentDirection, targetDirection), std::clamp(weight, 0.0f, 1.0f)).Normalized();
    RotateSubtreeAroundPivot(map, nodeTransforms, clavicleNode, claviclePosition, rotation);
}

void ApplyTorsoReachCompensation(
    Mat4x4*       nodeTransforms,
    const RigBoneMap& map,
    CharacterBone     upperArmBone,
    CharacterBone     forearmBone,
    CharacterBone     handBone,
    FVec3      targetGripPos,
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
    const FVec3 upperPosition = ProceduralMath::Translation(nodeTransforms[upperNode]);
    const float     armLength     = (ProceduralMath::Translation(nodeTransforms[foreNode]) - upperPosition).Length() +
                                    (ProceduralMath::Translation(nodeTransforms[handNode]) - ProceduralMath::Translation(nodeTransforms[foreNode])).Length();
    const float     excess        = std::max((targetGripPos - upperPosition).Length() - armLength * 0.90f, 0.0f);
    FVec3       horizontal    = targetGripPos - upperPosition;
    horizontal.y = 0.0f;
    if (excess <= 0.0f || horizontal.LengthSquared() <= 1.0e-6f) {
        return;
    }
    const FVec3 pivot     = ProceduralMath::Translation(nodeTransforms[hipsNode]);
    const float     lever     = std::max((ProceduralMath::Translation(nodeTransforms[spineNode]) - pivot).Length(), 0.20f);
    const float     angle     = std::min(std::atan2(excess, lever) * std::clamp(weight, 0.0f, 1.0f), ZHLN::DegF(12.0f).ToRadians().value);
    const FVec3 direction = horizontal.Normalized();
    const Quat pitch     = Quat::FromAxisAngle(FVec3::UnitX(), direction.z * angle);
    const Quat roll      = Quat::FromAxisAngle(FVec3::UnitZ(), -direction.x * angle);
    RotateSubtreeAroundPivot(map, nodeTransforms, spineNode, pivot, (roll * pitch).Normalized());
}

Quat ConstrainWristRotation(
    Quat targetRotation,
    Quat authoredRotation,
    FVec3 twistAxis,
    float        maxTwistRadians,
    float        maxSwingRadians
) noexcept {
    const FVec3 axis  = SafeNormalized(twistAxis, FVec3::UnitZ());
    Quat       delta = (targetRotation * authoredRotation.Inversed()).Normalized();
    if (delta.w < 0.0f) {
        delta = -delta;
    }
    const FVec3 vector(delta.x, delta.y, delta.z);
    const FVec3 projected = axis * Dot(vector, axis);
    Quat       twist(projected.x, projected.y, projected.z, delta.w);
    const float     twistLengthSq = projected.LengthSquared() + delta.w * delta.w;
    twist                         = twistLengthSq > 1.0e-8f ? twist.Normalized() : Quat::Identity();
    Quat swing               = (delta * twist.Inversed()).Normalized();

    auto clampRotation = [](Quat value, FVec3 fallbackAxis, float maximum) {
        FVec3 rotationAxis;
        float     angle = 0.0f;
        value.ToAxisAngle(rotationAxis, angle);
        if (angle > std::numbers::pi_v<float>) {
            angle -= 2.0f * std::numbers::pi_v<float>;
        }
        const float clamped = std::clamp(angle, -std::abs(maximum), std::abs(maximum));
        return std::abs(clamped) > 1.0e-6f ? Quat::FromAxisAngle(SafeNormalized(rotationAxis, fallbackAxis), clamped) : Quat::Identity();
    };

    const Quat limitedTwist = clampRotation(twist, axis, maxTwistRadians);
    const Quat limitedSwing = clampRotation(swing, FVec3::UnitY(), maxSwingRadians);
    return (limitedSwing * limitedTwist * authoredRotation).Normalized();
}

void SolveLimbIK(
    Mat4x4*       nodeTransforms,
    const RigBoneMap& map,
    CharacterBone     upperBone,
    CharacterBone     foreBone,
    CharacterBone     handBone,
    const Mat4x4& targetGripTransform,
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

    const FVec3 upperPosition  = ProceduralMath::Translation(nodeTransforms[upperNode]);
    const FVec3 forePosition   = ProceduralMath::Translation(nodeTransforms[foreNode]);
    const FVec3 handPosition   = ProceduralMath::Translation(nodeTransforms[handNode]);
    const float     upperLength    = std::max((forePosition - upperPosition).Length(), 0.001f);
    const float     lowerLength    = std::max((handPosition - forePosition).Length(), 0.001f);
    const FVec3 rawTarget      = ProceduralMath::Translation(targetGripTransform);
    const FVec3 targetPosition = handPosition + (rawTarget - handPosition) * SmoothStep(solveWeight);

    const FVec3 aimAxis       = SafeNormalized(targetPosition - upperPosition, FVec3(0.0f, -1.0f, 0.0f));
    const FVec3 authoredPlane = (forePosition - upperPosition) - aimAxis * Dot((forePosition - upperPosition), aimAxis);
    const FVec3 authoredPole  = SafeNormalized(authoredPlane, FVec3::UnitZ());
    const FVec3 hintPole      = SafeNormalized(ProceduralMath::Direction(targetGripTransform, grip.poleHintOffset), authoredPole);
    const FVec3 pole          = SafeNormalized(authoredPole * 0.4f + hintPole * 0.6f, authoredPole);

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

    const FVec3  currentUpperDirection = SafeNormalized(forePosition - upperPosition, FVec3(0.0f, -1.0f, 0.0f));
    const FVec3  currentLowerDirection = SafeNormalized(handPosition - forePosition, FVec3(0.0f, -1.0f, 0.0f));
    const Mat4x4 solvedUpper           = CorrectBoneDirection(nodeTransforms[upperNode], currentUpperDirection, ik.upperDirection, upperPosition);
    Mat4x4       solvedFore            = CorrectBoneDirection(nodeTransforms[foreNode], currentLowerDirection, ik.lowerDirection, ik.midPosition);

    Quat targetPalmRotation = MatrixRotation(targetGripTransform);
    if (grip.orientationMode == GripOrientationMode::AutomaticHanded && handBone == CharacterBone::HandL) {
        // Preserve grip-local +Z as hand/finger forward while mirroring +X so
        // the left palm faces inward instead of copying the right-hand side.
        targetPalmRotation = (targetPalmRotation * Quat::FromAxisAngle(FVec3::UnitZ(), std::numbers::pi_v<float>)).Normalized();
    }
    const size_t    palmSide = handBone == CharacterBone::HandL ? 0u : 1u;
    const Quat targetHandRotation =
        map.handPalmFramesValid[palmSide] ? (targetPalmRotation * map.handBoneToPalmRotations[palmSide].Inversed()).Normalized() : targetPalmRotation;
    const Quat authoredHandRotation = MatrixRotation(nodeTransforms[handNode]);

    // Share palm-facing roll with the forearm before applying the tighter wrist
    // cone. This lets a palm turn toward the item instead of remaining ground-
    // facing merely because the required roll exceeds the wrist swing limit.
    const FVec3 rollAxis          = SafeNormalized(ik.lowerDirection, currentLowerDirection);
    const Quat authoredPalmFrame = map.handPalmFramesValid[palmSide] ? (authoredHandRotation * map.handBoneToPalmRotations[palmSide]).Normalized() :
                                                                            authoredHandRotation;
    FVec3       currentPalmNormal = authoredPalmFrame * FVec3::UnitX();
    FVec3       targetPalmNormal  = targetPalmRotation * FVec3::UnitX();
    currentPalmNormal -= rollAxis * Dot(currentPalmNormal, rollAxis);
    targetPalmNormal -= rollAxis * Dot(targetPalmNormal, rollAxis);
    Quat forearmRoll = Quat::Identity();
    if (currentPalmNormal.LengthSquared() > 1.0e-8f && targetPalmNormal.LengthSquared() > 1.0e-8f) {
        currentPalmNormal      = currentPalmNormal.Normalized();
        targetPalmNormal       = targetPalmNormal.Normalized();
        const float signedRoll = std::atan2(Dot(rollAxis, Cross(currentPalmNormal, targetPalmNormal)), Dot(currentPalmNormal, targetPalmNormal));
        const float rollLimit  = ZHLN::DegF(std::abs(grip.maxForearmTwistDeg)).ToRadians().value;
        const float rollAngle  = std::clamp(signedRoll * std::clamp(grip.forearmTwistWeight * solveWeight, 0.0f, 1.0f), -rollLimit, rollLimit);
        forearmRoll            = Quat::FromAxisAngle(rollAxis, rollAngle);
        solvedFore             = ProceduralMath::Transform((forearmRoll * MatrixRotation(solvedFore)).Normalized(), ProceduralMath::Translation(solvedFore), MatrixScale(solvedFore));
    }

    const Quat carriedHandRotation = (forearmRoll * authoredHandRotation).Normalized();
    const Quat constrainedRotation = ConstrainWristRotation(
        targetHandRotation, carriedHandRotation, ik.lowerDirection, ZHLN::DegF(grip.maxWristTwistDeg).ToRadians().value, ZHLN::DegF(grip.maxWristSwingDeg).ToRadians().value
    );
    const float      rotationBlend = std::clamp(grip.rotationWeight * solveWeight, 0.0f, 1.0f);
    const Quat  handRotation  = Slerp(carriedHandRotation, constrainedRotation, rotationBlend).Normalized();
    const Mat4x4 solvedHand    = ProceduralMath::Transform(handRotation, ik.endPosition, MatrixScale(nodeTransforms[handNode]));

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

Quat ConstrainFingerHingeRotation(
    Quat authoredRotation,
    Quat desiredRotation,
    FVec3 hingeAxis,
    float        flexSign,
    float        maxFlexRadians,
    float        maxExtensionRadians
) noexcept {
    const FVec3 axis  = SafeNormalized(hingeAxis, FVec3::UnitX());
    Quat       delta = (desiredRotation * authoredRotation.Inversed()).Normalized();
    if (delta.w < 0.0f) {
        delta = -delta;
    }
    const FVec3 vector(delta.x, delta.y, delta.z);
    const FVec3 projected = axis * Dot(vector, axis);
    const float     lengthSq  = projected.LengthSquared() + delta.w * delta.w;
    Quat twist = lengthSq > 1.0e-8f ? Quat(projected.x, projected.y, projected.z, delta.w).Normalized() : Quat::Identity();
    FVec3 twistAxis;
    float     angle = 0.0f;
    twist.ToAxisAngle(twistAxis, angle);
    if (angle > std::numbers::pi_v<float>) {
        angle -= 2.0f * std::numbers::pi_v<float>;
    }
    if (Dot(twistAxis, axis) < 0.0f) {
        angle = -angle;
    }
    const float flex      = std::abs(maxFlexRadians);
    const float extension = std::abs(maxExtensionRadians);
    const float clamped   = flexSign >= 0.0f ? std::clamp(angle, -extension, flex) : std::clamp(angle, -flex, extension);
    return (Quat::FromAxisAngle(axis, clamped) * authoredRotation).Normalized();
}

void ApplyKinematicFingers(
    Mat4x4*           nodeTransforms,
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
        const float      fingerCurl   = CurlForDigit(joint.digit, curl);
        const Mat4x4 current      = nodeTransforms[joint.child];
        const FVec3  axis         = SafeNormalized(ProceduralMath::Direction(current, FVec3::UnitX()), FVec3::UnitX());
        const bool       isThumb      = joint.digit == FingerDigit::Thumb;
        const float      maximumAngle = isThumb ? ZHLN::DegF(50.0f).ToRadians().value : ZHLN::DegF(62.0f).ToRadians().value;
        float            curlSign     = -1.0f;
        if (axisMode == FingerCurlAxisMode::MirroredLocalX && handBone == CharacterBone::HandL) {
            curlSign = 1.0f;
        } else if (axisMode == FingerCurlAxisMode::AutomaticPalm) {
            curlSign = joint.hingeFlexSign;
        }

        const float      desiredAngle        = curlSign * maximumAngle * std::clamp(fingerCurl * weight, 0.0f, 1.0f);
        const Quat  authoredRotation    = MatrixRotation(current);
        const Quat  desiredRotation     = (Quat::FromAxisAngle(axis, desiredAngle) * authoredRotation).Normalized();
        const Quat  constrainedRotation = ConstrainFingerHingeRotation(authoredRotation, desiredRotation, axis, curlSign, maximumAngle, 0.0f);
        const Mat4x4 target              = ProceduralMath::Transform(constrainedRotation, ProceduralMath::Translation(current), MatrixScale(current));
        SetModelTransformAndCarrySubtree(nodeTransforms, map, joint.child, target);
    }
}
} // namespace ZHLN::Animation
