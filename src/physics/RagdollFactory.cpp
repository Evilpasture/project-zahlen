// src/physics/RagdollFactory.cpp
#include "Physics.hpp"
#include "detail/ControlFlow.hpp"

#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <vector>

namespace ZHLN::Physics {

JPH::Ref<JPH::Ragdoll> CreateSkeletalRagdoll(PhysicsContext& ctx, const JPH::Skeleton* skeleton,
											 const std::vector<RagdollPartParams>& parts) {
	// Retrieve Jolt objects cleanly over the PIMPL barrier using helpers
	auto& joltSystem = GetInternalSystem(ctx);
	auto& world = GetInternalWorld(ctx);

	JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings();

	// 1. Assign visual skeleton directly to public Ref field
	settings->mSkeleton = const_cast<JPH::Skeleton*>(skeleton);

	// 2. Resize public dynamic parts vector
	settings->mParts.resize(skeleton->GetJointCount());

	ZHLN_LOCK(world.sync.shadowLock) {
		for (const auto& part : parts) {
			uint32_t jointIdx = part.jointIndex;

			// 3. Configure JPH::BodyCreationSettings fields inherited by mParts[jointIdx]
			settings->mParts[jointIdx].SetShape(part.shape);

			// Scale and configure mass calculations using native overrides
			settings->mParts[jointIdx].mOverrideMassProperties =
				JPH::EOverrideMassProperties::CalculateInertia;
			settings->mParts[jointIdx].mMassPropertiesOverride.mMass = part.mass;

			settings->mParts[jointIdx].mMotionType = JPH::EMotionType::Dynamic;
			settings->mParts[jointIdx].mObjectLayer = 1; // Dynamic Layer
			settings->mParts[jointIdx].mPosition = part.position;
			settings->mParts[jointIdx].mRotation = part.rotation;

			// 4. Configure Parent constraint parameters
			if (part.parentJointIndex >= 0) {
				JPH::SwingTwistConstraintSettings twistSettings;

				// Align space to body center of mass as defined by compiler hint
				twistSettings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
				twistSettings.mPosition1 = twistSettings.mPosition2 = JPH::RVec3::sZero();

				twistSettings.mTwistAxis1 = twistSettings.mTwistAxis2 = part.twistAxis;
				twistSettings.mPlaneAxis1 = twistSettings.mPlaneAxis2 = part.planeNormal;

				twistSettings.mNormalHalfConeAngle = part.coneAngle;
				twistSettings.mPlaneHalfConeAngle = part.coneAngle;
				twistSettings.mTwistMinAngle = part.twistMin;
				twistSettings.mTwistMaxAngle = part.twistMax;

				// 5. Configure split swing and twist motor settings
				if (part.enableMotors) {
					twistSettings.mSwingMotorSettings.mSpringSettings.mFrequency = 8.0f;
					twistSettings.mSwingMotorSettings.mSpringSettings.mDamping = 1.0f;
					twistSettings.mSwingMotorSettings.SetTorqueLimit(part.maxMotorForce);

					twistSettings.mTwistMotorSettings.mSpringSettings.mFrequency = 8.0f;
					twistSettings.mTwistMotorSettings.mSpringSettings.mDamping = 1.0f;
					twistSettings.mTwistMotorSettings.SetTorqueLimit(part.maxMotorForce);
				}

				// 6. Bind the constraint settings pointer to parent Ref target
				settings->mParts[jointIdx].mToParent =
					new JPH::SwingTwistConstraintSettings(twistSettings);
			}
		}

		// --- CONSTRAINT SAFETY INITIALIZATION ---
		// Jolt's Stabilize() assumes every joint with a parent in the skeleton has a valid
		// constraint. We auto-generate default constraints for unconfigured joints to prevent null
		// dereferences.
		for (size_t i = 1; i < skeleton->GetJointCount(); ++i) {
			int parentIdx = skeleton->GetJoint(i).mParentJointIndex;
			if (parentIdx >= 0 && settings->mParts[i].mToParent == nullptr) {
				auto* twist = new JPH::SwingTwistConstraintSettings();
				twist->mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
				twist->mPosition1 = twist->mPosition2 = JPH::RVec3::sZero();

				twist->mTwistAxis1 = twist->mTwistAxis2 = JPH::Vec3::sAxisX();
				twist->mPlaneAxis1 = twist->mPlaneAxis2 = JPH::Vec3::sAxisY();

				// Setup comfortable default rotation limits
				twist->mNormalHalfConeAngle = JPH::DegreesToRadians(45.0f);
				twist->mPlaneHalfConeAngle = JPH::DegreesToRadians(45.0f);
				twist->mTwistMinAngle = JPH::DegreesToRadians(-45.0f);
				twist->mTwistMaxAngle = JPH::DegreesToRadians(45.0f);

				settings->mParts[i].mToParent = twist;
			}
		} // ----------------------------------------

		// 7. Complete final joint and collision topology mapping
		settings->DisableParentChildCollisions();
		settings->CalculateBodyIndexToConstraintIndex();
		settings->CalculateConstraintIndexToBodyIdxPair();
		settings->Stabilize();

		JPH::Ragdoll* ragdoll = settings->CreateRagdoll(0, 0, &joltSystem);
		return {ragdoll};
	}
}
} // namespace ZHLN::Physics
