// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsWorld.hpp"
#include <Jolt/Physics/Constraints/ConeConstraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>

namespace ZHLN::Physics {

JPH::Constraint* CreateNativeConstraint(const ConstraintType type, JPH::Body* b1, JPH::Body* b2, const ConstraintParams& p) {
    switch (type) {
        case ConstraintType::Fixed: {
            JPH::FixedConstraintSettings settings;
            settings.mAutoDetectPoint = true;
            return settings.Create(*b1, *b2);
        }
        case ConstraintType::Point: {
            JPH::PointConstraintSettings settings;
            settings.mSpace  = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = settings.mPoint2 = JPH::RVec3(p.pivot);
            return settings.Create(*b1, *b2);
        }
        case ConstraintType::Hinge: {
            JPH::HingeConstraintSettings settings;
            settings.mSpace  = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = settings.mPoint2 = JPH::RVec3(p.pivot);
            settings.mHingeAxis1 = settings.mHingeAxis2 = p.axis;

            // Calculate a normal perpendicular to the axis for the reference frame
            JPH::Vec3 normal      = p.axis.GetNormalizedPerpendicular();
            settings.mNormalAxis1 = settings.mNormalAxis2 = normal;

            settings.mLimitsMin = p.limitMin;
            settings.mLimitsMax = p.limitMax;

            if (p.hasMotor) {
                settings.mMotorSettings.mSpringSettings.mFrequency = p.frequency;
                settings.mMotorSettings.mSpringSettings.mDamping   = p.damping;
                settings.mMotorSettings.mMaxTorqueLimit            = p.maxForce;
            }

            return settings.Create(*b1, *b2);
        }
        case ConstraintType::Slider: {
            JPH::SliderConstraintSettings settings;
            settings.mSpace  = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = settings.mPoint2 = JPH::RVec3(p.pivot);
            settings.mSliderAxis1 = settings.mSliderAxis2 = p.axis;
            settings.mLimitsMin                           = p.limitMin;
            settings.mLimitsMax                           = p.limitMax;
            return settings.Create(*b1, *b2);
        }
        case ConstraintType::Cone: {
            JPH::ConeConstraintSettings settings;
            settings.mSpace  = JPH::EConstraintSpace::WorldSpace;
            settings.mPoint1 = settings.mPoint2 = JPH::RVec3(p.pivot);

            // Normalize axis and handle zero-length axis safety
            JPH::Vec3 axis  = p.axis;
            float     lenSq = axis.LengthSq();
            if (lenSq < 1e-6f) {
                axis = JPH::Vec3::sAxisY();
            } else {
                axis /= JPH::Sqrt(lenSq);
            }

            settings.mTwistAxis1 = settings.mTwistAxis2 = axis;

            // Typically for a cone, we use the limitMax as the half-cone angle
            settings.mHalfConeAngle = p.limitMax;
            return settings.Create(*b1, *b2);
        }
        case ConstraintType::Distance: {
            JPH::DistanceConstraintSettings settings;
            settings.mSpace = JPH::EConstraintSpace::WorldSpace;

            // If the pivot is near zero, Culverin defaults to the current distance between bodies.
            // Otherwise, it uses the pivot for both anchor points.
            if (p.pivot.LengthSq() > 1e-6f) {
                settings.mPoint1 = settings.mPoint2 = JPH::RVec3(p.pivot);
            } else {
                settings.mPoint1 = b1->GetPosition();
                settings.mPoint2 = b2->GetPosition();
            }

            settings.mMinDistance = p.limitMin;
            settings.mMaxDistance = p.limitMax;

            return settings.Create(*b1, *b2);
        }
        default:
            break;
    }
    return nullptr;
}

} // namespace ZHLN::Physics