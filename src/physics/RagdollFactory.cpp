// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsWorld.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <vector>

namespace ZHLN {

auto PhysicsContext::CreateSkeletalRagdoll(JPH::Ref<JPH::Skeleton> skeleton, const std::vector<Physics::RagdollPartParams>& parts) -> JPH::Ref<JPH::Ragdoll> {
    auto& joltSystem = GetInternalSystem();
    auto& world      = GetInternalWorld();

    JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings();
    settings->mSkeleton                     = skeleton;
    settings->mParts.resize(skeleton->GetJointCount());

    return ZHLN::Lock(world.sync.shadowLock, [&]() -> JPH::Ref<JPH::Ragdoll> {
        for (const auto& part: parts) {
            uint32_t jointIdx = part.jointIndex;

            settings->mParts[jointIdx].SetShape(part.shape);
            settings->mParts[jointIdx].mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
            settings->mParts[jointIdx].mMassPropertiesOverride.mMass = part.mass;
            settings->mParts[jointIdx].mMotionType                   = JPH::EMotionType::Dynamic;
            settings->mParts[jointIdx].mObjectLayer                  = 1;
            settings->mParts[jointIdx].mPosition                     = part.position;
            settings->mParts[jointIdx].mRotation                     = part.rotation;

            if (part.parentJointIndex >= 0) {
                JPH::SwingTwistConstraintSettings twistSettings;
                twistSettings.mSpace     = JPH::EConstraintSpace::LocalToBodyCOM;
                twistSettings.mPosition1 = twistSettings.mPosition2 = JPH::RVec3::sZero();
                twistSettings.mTwistAxis1 = twistSettings.mTwistAxis2 = part.twistAxis;
                twistSettings.mPlaneAxis1 = twistSettings.mPlaneAxis2 = part.planeNormal;
                twistSettings.mNormalHalfConeAngle                    = part.coneAngle;
                twistSettings.mPlaneHalfConeAngle                     = part.coneAngle;
                twistSettings.mTwistMinAngle                          = part.twistMin;
                twistSettings.mTwistMaxAngle                          = part.twistMax;

                if (part.enableMotors) {
                    twistSettings.mSwingMotorSettings.mSpringSettings.mFrequency = 8.0f;
                    twistSettings.mSwingMotorSettings.mSpringSettings.mDamping   = 1.0f;
                    twistSettings.mSwingMotorSettings.SetTorqueLimit(part.maxMotorForce);

                    twistSettings.mTwistMotorSettings.mSpringSettings.mFrequency = 8.0f;
                    twistSettings.mTwistMotorSettings.mSpringSettings.mDamping   = 1.0f;
                    twistSettings.mTwistMotorSettings.SetTorqueLimit(part.maxMotorForce);
                }

                settings->mParts[jointIdx].mToParent = new JPH::SwingTwistConstraintSettings(twistSettings);
            }
        }

        for (auto i = 1; i < skeleton->GetJointCount(); ++i) {
            int parentIdx = skeleton->GetJoint(i).mParentJointIndex;
            if (parentIdx >= 0 && settings->mParts[i].GetShape() != nullptr && settings->mParts[i].mToParent == nullptr) {
                auto* twist       = new JPH::SwingTwistConstraintSettings();
                twist->mSpace     = JPH::EConstraintSpace::LocalToBodyCOM;
                twist->mPosition1 = twist->mPosition2 = JPH::RVec3::sZero();
                twist->mNormalHalfConeAngle           = 0.0f;
                twist->mPlaneHalfConeAngle            = 0.0f;
                twist->mTwistMinAngle                 = 0.0f;
                twist->mTwistMaxAngle                 = 0.0f;

                settings->mParts[i].mToParent = twist;
            }
        }

        settings->DisableParentChildCollisions();
        settings->CalculateBodyIndexToConstraintIndex();
        settings->CalculateConstraintIndexToBodyIdxPair();
        settings->Stabilize();

        JPH::Ragdoll* ragdoll = settings->CreateRagdoll(0, 0, &joltSystem);
        return {ragdoll};
    });
}

} // namespace ZHLN
