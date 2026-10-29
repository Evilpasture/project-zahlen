// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RagdollManager.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <utility>

namespace ZHLN::Physics {

auto RagdollManager::FindSlotUnlocked(RagdollHandle handle) noexcept -> RagdollSlot* {
    if (handle == RagdollHandle::Invalid) {
        return nullptr;
    }
    const auto     raw   = static_cast<uint64_t>(handle);
    const auto     index = static_cast<uint32_t>(raw);
    const uint32_t gen   = static_cast<uint32_t>(raw >> 32);
    if (index >= slots.size()) {
        return nullptr;
    }
    auto& slot = slots[index];
    return (slot.generation == gen && slot.instance != nullptr) ? &slot : nullptr;
}

auto RagdollManager::FindSlotUnlocked(RagdollHandle handle) const noexcept -> const RagdollSlot* {
    if (handle == RagdollHandle::Invalid) {
        return nullptr;
    }
    const auto     raw   = static_cast<uint64_t>(handle);
    const auto     index = static_cast<uint32_t>(raw);
    const uint32_t gen   = static_cast<uint32_t>(raw >> 32);
    if (index >= slots.size()) {
        return nullptr;
    }
    const auto& slot = slots[index];
    return (slot.generation == gen && slot.instance != nullptr) ? &slot : nullptr;
}

void RagdollManager::Shutdown() noexcept {
    ZHLN::MutexGuard guard(mutex);
    for (auto& slot: slots) {
        if (slot.instance != nullptr) {
            if (slot.active) {
                slot.instance->RemoveFromPhysicsSystem();
                slot.active = false;
            }
            slot.instance = nullptr;
        }
    }
    slots.clear();
    freeSlots.clear();
}

auto RagdollManager::CreateSkeletalRagdoll(
    JPH::PhysicsSystem&               joltSystem,
    JPH::Ref<JPH::Skeleton>           skeleton,
    std::span<const RagdollPartParams> parts
) -> RagdollHandle {
    JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings();
    settings->mSkeleton                     = skeleton;
    settings->mParts.resize(static_cast<size_t>(skeleton->GetJointCount()));

    for (const auto& part: parts) {
        const uint32_t jointIdx = part.jointIndex;

        settings->mParts[jointIdx].SetShape(part.shape);
        settings->mParts[jointIdx].mOverrideMassProperties       = JPH::EOverrideMassProperties::CalculateInertia;
        settings->mParts[jointIdx].mMassPropertiesOverride.mMass = part.mass;
        settings->mParts[jointIdx].mMotionType                   = JPH::EMotionType::Dynamic;
        settings->mParts[jointIdx].mObjectLayer                  = static_cast<JPH::ObjectLayer>(Layers::ID::MOVING);
        settings->mParts[jointIdx].mPosition                     = part.position;
        settings->mParts[jointIdx].mRotation                     = part.rotation;

        if (part.parentJointIndex >= 0) {
            JPH::SwingTwistConstraintSettings twistSettings;
            twistSettings.mSpace               = JPH::EConstraintSpace::LocalToBodyCOM;
            twistSettings.mPosition1           = twistSettings.mPosition2 = JPH::RVec3::sZero();
            twistSettings.mTwistAxis1          = twistSettings.mTwistAxis2 = part.twistAxis;
            twistSettings.mPlaneAxis1          = twistSettings.mPlaneAxis2 = part.planeNormal;
            twistSettings.mNormalHalfConeAngle = part.coneAngle;
            twistSettings.mPlaneHalfConeAngle  = part.coneAngle;
            twistSettings.mTwistMinAngle       = part.twistMin;
            twistSettings.mTwistMaxAngle       = part.twistMax;

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

    for (int i = 1; i < skeleton->GetJointCount(); ++i) {
        const int parentIdx = skeleton->GetJoint(i).mParentJointIndex;
        if (parentIdx >= 0 && settings->mParts[static_cast<size_t>(i)].GetShape() != nullptr &&
            settings->mParts[static_cast<size_t>(i)].mToParent == nullptr) {
            auto* twist                = new JPH::SwingTwistConstraintSettings();
            twist->mSpace              = JPH::EConstraintSpace::LocalToBodyCOM;
            twist->mPosition1          = twist->mPosition2 = JPH::RVec3::sZero();
            twist->mNormalHalfConeAngle = 0.0f;
            twist->mPlaneHalfConeAngle  = 0.0f;
            twist->mTwistMinAngle       = 0.0f;
            twist->mTwistMaxAngle       = 0.0f;

            settings->mParts[static_cast<size_t>(i)].mToParent = twist;
        }
    }

    settings->DisableParentChildCollisions();
    settings->CalculateBodyIndexToConstraintIndex();
    settings->CalculateConstraintIndexToBodyIdxPair();
    settings->Stabilize();

    ZHLN::MutexGuard       guard(mutex);
    JPH::Ref<JPH::Ragdoll> createdRagdoll {settings->CreateRagdoll(0, 0, &joltSystem)};
    if (createdRagdoll == nullptr) {
        return RagdollHandle::Invalid;
    }

    uint32_t index = 0;
    if (freeSlots.empty()) {
        freeSlots.reserve(slots.size() + 1);
        index = static_cast<uint32_t>(slots.size());
        slots.emplace_back();
    } else {
        index = freeSlots.back();
        freeSlots.pop_back();
    }

    auto& slot    = slots[index];
    slot.instance = std::move(createdRagdoll);
    slot.active   = false;
    return static_cast<RagdollHandle>((static_cast<uint64_t>(slot.generation) << 32) | index);
}

void RagdollManager::DestroyRagdolls(std::span<const RagdollHandle> handles) noexcept {
    if (handles.empty()) {
        return;
    }
    ZHLN::MutexGuard guard(mutex);
    for (const auto handle: handles) {
        auto* slot = FindSlotUnlocked(handle);
        if (slot == nullptr) {
            continue;
        }
        if (slot->active) {
            slot->instance->RemoveFromPhysicsSystem();
            slot->active = false;
        }
        slot->instance = nullptr;
        ++slot->generation;
        freeSlots.push_back(static_cast<uint32_t>(static_cast<uint64_t>(handle)));
    }
}

auto RagdollManager::GetRagdoll(RagdollHandle handle) const noexcept -> ZHLN::Optional<JPH::Ragdoll&> {
    ZHLN::MutexGuard guard(mutex);
    const auto*      slot = FindSlotUnlocked(handle);
    if (slot != nullptr && slot->instance != nullptr) {
        return *slot->instance.GetPtr();
    }
    return std::nullopt;
}

void RagdollManager::ActivateRagdoll(RagdollHandle handle, const JPH::SkeletonPose& pose, JPH::Vec3Arg initialVelocity) noexcept {
    ZHLN::MutexGuard guard(mutex);
    auto*            slot = FindSlotUnlocked(handle);
    if (slot == nullptr) {
        return;
    }
    if (!slot->active) {
        slot->instance->AddToPhysicsSystem(JPH::EActivation::Activate);
        slot->active = true;
    }
    slot->instance->SetPose(pose);
    slot->instance->SetLinearAndAngularVelocity(initialVelocity, JPH::Vec3::sZero());
}

void RagdollManager::RemoveRagdoll(RagdollHandle handle) noexcept {
    ZHLN::MutexGuard guard(mutex);
    auto*            slot = FindSlotUnlocked(handle);
    if (slot != nullptr && slot->active) {
        slot->instance->RemoveFromPhysicsSystem();
        slot->active = false;
    }
}

void RagdollManager::DriveRagdollPose(RagdollHandle handle, const JPH::SkeletonPose& pose) noexcept {
    ZHLN::MutexGuard guard(mutex);
    auto*            slot = FindSlotUnlocked(handle);
    if (slot == nullptr || !slot->active) {
        return;
    }
    slot->instance->Activate();
    slot->instance->DriveToPoseUsingMotors(pose);
}

void RagdollManager::AddRagdollImpulse(RagdollHandle handle, uint32_t jointIndex, JPH::Vec3Arg impulse, JPH::BodyInterface& bodyInterface) noexcept {
    ZHLN::MutexGuard guard(mutex);
    auto*            slot = FindSlotUnlocked(handle);
    if (slot == nullptr || !slot->active || jointIndex >= slot->instance->GetBodyCount()) {
        return;
    }
    const JPH::BodyID bodyID = slot->instance->GetBodyID(static_cast<int>(jointIndex));
    if (!bodyID.IsInvalid()) {
        bodyInterface.AddImpulse(bodyID, impulse);
        bodyInterface.ActivateBody(bodyID);
    }
}

auto RagdollManager::GetRagdollPose(RagdollHandle handle, JPH::RVec3& outRootOffset, JPH::Mat44* outWorldJoints) const noexcept -> bool {
    if (outWorldJoints == nullptr) {
        return false;
    }
    ZHLN::MutexGuard guard(mutex);
    const auto*      slot = FindSlotUnlocked(handle);
    if (slot == nullptr || !slot->active) {
        return false;
    }
    slot->instance->GetPose(outRootOffset, outWorldJoints);
    return true;
}

} // namespace ZHLN::Physics
