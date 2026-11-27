// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsWorld.hpp"
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <physics/Physics.hpp>
#include <memory>
#include <utility>

namespace ZHLN {

namespace Physics {

namespace {

auto ToWorldPoint(ZHLN::FVec3 vec) {
    return JPH::RVec3(static_cast<JPH::Real>(vec.x), static_cast<JPH::Real>(vec.y), static_cast<JPH::Real>(vec.z));
}

auto NormalizeAxisOr(ZHLN::FVec3 axis, JPH::Vec3 fallback) -> JPH::Vec3 {
    const float lenSq = axis.LengthSquared();
    if (!std::isfinite(lenSq) || lenSq < 1e-12f) {
        return fallback;
    }
    const ZHLN::FVec3 normalized = axis / std::sqrt(lenSq);
    return JPH::Vec3(normalized.x, normalized.y, normalized.z);
}

} // namespace

auto CreateNativeConstraint(JPH::Body* b1, JPH::Body* b2, const FixedConstraint& p) -> JPH::Constraint* {
    JPH::FixedConstraintSettings settings;
    if (const auto pivot = p.pivot; pivot.HasValue()) {
        settings.mAutoDetectPoint = false;
        settings.mPoint1 = settings.mPoint2 = ToWorldPoint(pivot.value);
    } else {
        // The old behaviour: weld in whatever relative pose the bodies hold.
        settings.mAutoDetectPoint = true;
    }
    return settings.Create(*b1, *b2);
}

auto CreateNativeConstraint(JPH::Body* b1, JPH::Body* b2, const PointConstraint& p) -> JPH::Constraint* {
    JPH::PointConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    const JPH::RVec3 anchor = p.pivot.HasValue() ? ToWorldPoint(p.pivot.value) : (b1->GetPosition() + b2->GetPosition()) * 0.5;
    settings.mPoint1 = settings.mPoint2 = anchor;
    return settings.Create(*b1, *b2);
}

auto CreateNativeConstraint(JPH::Body* b1, JPH::Body* b2, const DistanceConstraint& p) -> JPH::Constraint* {
    JPH::DistanceConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    // Anchors ride the body origins, matching the old zero-pivot path.
    settings.mPoint1 = b1->GetPosition();
    settings.mPoint2 = b2->GetPosition();
    settings.mMinDistance = p.minDistance;
    settings.mMaxDistance = p.maxDistance;
    return settings.Create(*b1, *b2);
}

auto CreateNativeConstraint(JPH::Body* b1, JPH::Body* b2, const HingeConstraint& p) -> JPH::Constraint* {
    JPH::HingeConstraintSettings settings;
    settings.mSpace       = JPH::EConstraintSpace::WorldSpace;
    settings.mPoint1      = settings.mPoint2 = ToWorldPoint(p.pivot);
    const JPH::Vec3 axis  = NormalizeAxisOr(p.axis, JPH::Vec3::sAxisY());
    const JPH::Vec3 norm  = axis.GetNormalizedPerpendicular();
    settings.mHingeAxis1  = settings.mHingeAxis2  = axis;
    settings.mNormalAxis1 = settings.mNormalAxis2 = norm;

    if (p.limitEnabled) {
        settings.mLimitsMin = p.minAngle.value;
        settings.mLimitsMax = p.maxAngle.value;
    }

    settings.mMaxFrictionTorque = p.maxFrictionTorque;
    if (p.motor.maxImpulse > 0.0f) {
        settings.mMotorSettings.mMaxTorqueLimit = p.motor.maxImpulse;
    }

    JPH::Constraint* constraint = settings.Create(*b1, *b2);
    if (constraint != nullptr && p.targetAngle.HasValue()) {
        auto* hinge = static_cast<JPH::HingeConstraint*>(constraint);
        hinge->SetMotorState(JPH::EMotorState::Position);
        hinge->SetTargetAngle(p.targetAngle.value.value);
    }
    return constraint;
}

auto CreateNativeConstraint(JPH::Body* b1, JPH::Body* b2, const SliderConstraint& p) -> JPH::Constraint* {
    JPH::SliderConstraintSettings settings;
    settings.mSpace = JPH::EConstraintSpace::WorldSpace;
    if (p.pivot.HasValue()) {
        settings.mAutoDetectPoint = false;
        settings.mPoint1          = settings.mPoint2 = ToWorldPoint(p.pivot.value);
    } else {
        settings.mAutoDetectPoint = true;
    }
    settings.SetSliderAxis(NormalizeAxisOr(p.axis, JPH::Vec3::sAxisX()));

    if (p.limitEnabled) {
        settings.mLimitsMin = p.minPosition;
        settings.mLimitsMax = p.maxPosition;
    }

    settings.mMaxFrictionForce = p.maxFrictionForce;
    if (p.motor.maxImpulse > 0.0f) {
        settings.mMotorSettings.mMaxForceLimit = p.motor.maxImpulse;
    }

    JPH::Constraint* constraint = settings.Create(*b1, *b2);
    if (constraint != nullptr && p.targetPosition.HasValue()) {
        auto* slider = static_cast<JPH::SliderConstraint*>(constraint);
        slider->SetMotorState(JPH::EMotorState::Position);
        slider->SetTargetPosition(p.targetPosition.value);
    }
    return constraint;
}

auto CreateNativeConstraint(JPH::Body* b1, JPH::Body* b2, const ConeConstraint& p) -> JPH::Constraint* {
    JPH::ConeConstraintSettings settings;
    settings.mSpace         = JPH::EConstraintSpace::WorldSpace;
    settings.mPoint1        = settings.mPoint2 = ToWorldPoint(p.pivot);
    settings.mTwistAxis1    = settings.mTwistAxis2 = NormalizeAxisOr(p.axis, JPH::Vec3::sAxisY());
    settings.mHalfConeAngle = p.halfAngle.value;
    return settings.Create(*b1, *b2);
}

} // namespace Physics

auto PhysicsContext::CreateConstraintVariant(Physics::BodyID b1, Physics::BodyID b2, const Physics::ConstraintVariant& params)
    -> Physics::ConstraintID {
    if (b1 == Physics::BodyID::Null() || b2 == Physics::BodyID::Null() || b1 == b2) {
        return Physics::ConstraintID::Null();
    }
    auto&                       world  = GetInternalWorld();
    const Physics::ConstraintID handle = world.AllocateConstraintID();

    Physics::Command cmd {};
    cmd.type                     = Physics::CommandType::CreateConstraint;
    // The payload holds a std::variant, whose default constructor is not
    // trivial: start its lifetime in place instead of assigning into raw
    // union storage.
    std::construct_at(&cmd.payload.createConstraint, Physics::CreateConstraintPayload {
        .constraintID = handle,
        .b1           = b1,
        .b2           = b2,
        .params       = params,
    });
    world.mailbox.Enqueue(std::move(cmd));
    return handle;
}

void PhysicsContext::SetTargetAngle(Physics::TypedConstraintID<Physics::HingeConstraint> handle, ZHLN::RadF angle) {
    auto&            world = GetInternalWorld();
    Physics::Command cmd {};
    cmd.type                        = Physics::CommandType::SetConstraintTarget;
    cmd.payload.setConstraintTarget = Physics::SetConstraintTargetPayload {
        .targetConstraintID = handle,
        .targetValue        = angle.value,
    };
    world.mailbox.Enqueue(std::move(cmd));
}

void PhysicsContext::SetTargetPosition(Physics::TypedConstraintID<Physics::SliderConstraint> handle, float position) {
    auto&            world = GetInternalWorld();
    Physics::Command cmd {};
    cmd.type                        = Physics::CommandType::SetConstraintTarget;
    cmd.payload.setConstraintTarget = Physics::SetConstraintTargetPayload {
        .targetConstraintID = handle,
        .targetValue        = position,
    };
    world.mailbox.Enqueue(std::move(cmd));
}

} // namespace ZHLN
