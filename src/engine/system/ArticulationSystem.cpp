#include "ArticulationSystem.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <cgltf.h>
#include <cstring>
#include <detail/ControlFlow.hpp>
#include <ecs/ECS.hpp>
#include <physics/Physics.hpp>
#include <physics/PhysicsWorld.hpp>
#include <print>

namespace ZHLN {

void ArticulationSystem::Update(Engine& engine, float dt) {
	auto& reg = engine.GetRegistry();
	const auto& world = engine.GetPhysicsContext().GetWorld();
	auto& rc = engine.GetRenderContext();

	auto entities = reg.GetEntitiesWith<RagdollComponent>();
	auto ragdolls = reg.GetRawArray<RagdollComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		RagdollComponent& ragComp = ragdolls[i];
		auto* phys = reg.Get<PhysicsComponent>(e);

		std::println("[C++] ArticSystem - Entity: {}, State: {}, AddedToPhys: {}, Joints: {}",
					 e.index, (uint32_t)ragComp.state, (int)ragComp.isAddedToPhysics,
					 ragComp.jointCount);

		if ((ragComp.ragdollInstance == nullptr) || (ragComp.gltfSkin == nullptr)) {
			continue;
		}

		JPH::Ragdoll* ragdoll = ragComp.ragdollInstance;
		const JPH::Skeleton* skel = ragdoll->GetRagdollSettings()->GetSkeleton();
		const auto* skin = static_cast<const cgltf_skin*>(ragComp.gltfSkin);

		// ====================================================================
		// 1. QUERY CAPSULE WORLD POSITION & SET ROOT OFFSET
		// ====================================================================
		JPH::RVec3 capsuleWorldPos = JPH::RVec3::sZero();
		if (phys != nullptr) {
			uint32_t dense = world.slotToDense[phys->physicsHandle.index];
			const size_t base = static_cast<size_t>(dense) * 4;
			capsuleWorldPos = JPH::RVec3(world.positions[base], world.positions[base + 1],
										 world.positions[base + 2]);
		}

		JPH::SkeletonPose animPose;
		animPose.SetSkeleton(skel);
		animPose.SetRootOffset(capsuleWorldPos); // <-- Set root to player's actual position!

		std::vector<JPH::Mat44> localJoints(ragComp.jointCount, JPH::Mat44::sIdentity());
		for (uint32_t j = 0; j < ragComp.jointCount; ++j) {
			const cgltf_node* jointNode = nullptr;
			for (size_t k = 0; k < skin->joints_count; ++k) {
				if ((skin->joints[k]->name != nullptr) &&
					std::string_view(skin->joints[k]->name) == skel->GetJoint(j).mName) {
					jointNode = skin->joints[k];
					break;
				}
			}
			if (jointNode != nullptr) {
				float m[16];
				cgltf_node_transform_local(jointNode, m);
				localJoints[j] = JPH::Mat44(
					JPH::Vec4(m[0], m[1], m[2], m[3]), JPH::Vec4(m[4], m[5], m[6], m[7]),
					JPH::Vec4(m[8], m[9], m[10], m[11]), JPH::Vec4(m[12], m[13], m[14], m[15]));
			}
		}
		std::memcpy(animPose.GetJointMatrices().data(), localJoints.data(),
					ragComp.jointCount * sizeof(JPH::Mat44));
		animPose.CalculateJointStates();

		// ====================================================================
		// 2. STATE TRANSITIONS & MOMENTUM INHERITANCE
		// ====================================================================
		if (ragComp.state != ragComp.prevState) {
			if (ragComp.state == RagdollState::Limp ||
				ragComp.state == RagdollState::KeyframeMotor) {
				if (ragComp.isAddedToPhysics == 0) { // <-- UPDATED (Explicit comparison)
					ZHLN_LOCK(world.sync.shadowLock) {
						ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);

						if (phys != nullptr) {
							JPH::Vec3 charVel = Physics::GetCharacterVelocity(
								engine.GetPhysicsContext(), phys->physicsHandle);
							ragdoll->SetPose(
								animPose); // Correctly aligns visual/physical poses on transition
							ragdoll->SetLinearAndAngularVelocity(charVel, JPH::Vec3::sZero());
						}
						ragComp.isAddedToPhysics = 1; // <-- UPDATED (Clean cast)
					}
				}
			} else if (ragComp.state == RagdollState::Inactive &&
					   ragComp.isAddedToPhysics != 0) { // <-- UPDATED (Explicit comparison)
				ZHLN_LOCK(world.sync.shadowLock) {
					ragdoll->RemoveFromPhysicsSystem();
					ragComp.isAddedToPhysics = 0; // <-- UPDATED (Clean cast)
				}
			}
			ragComp.prevState = ragComp.state;
		}

		// ====================================================================
		// 3. EXECUTE ACTIVE PHYSICS
		// ====================================================================
		if (ragComp.state == RagdollState::KeyframeMotor) {
			ZHLN_LOCK(world.sync.shadowLock) {
				ragdoll->Activate();
				ragdoll->DriveToPoseUsingMotors(animPose);
			}
		}

		// ====================================================================
		// 4. READ-BACK & GPU INVERSE-BIND SKINNING
		// ====================================================================
		if (ragComp.state == RagdollState::Limp || ragComp.state == RagdollState::KeyframeMotor) {
			std::vector<JPH::Mat44> physicalWorldJoints(ragComp.jointCount,
														JPH::Mat44::sIdentity());
			JPH::RVec3 actualRootOffset = JPH::RVec3::sZero();

			ZHLN_LOCK(world.sync.shadowLock) {
				ragdoll->GetPose(actualRootOffset, physicalWorldJoints.data());
			}

			// Calculate final skinning matrices: Final = PhysicalWorld * InverseBindMatrix
			std::vector<JPH::Mat44> finalSkinningMatrices(ragComp.jointCount);
			for (uint32_t j = 0; j < ragComp.jointCount; ++j) {
				JPH::Mat44 ibm = JPH::Mat44::sIdentity();
				if (skin->inverse_bind_matrices != nullptr) {
					float ibmRaw[16];
					cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibmRaw, 16);
					ibm = JPH::Mat44(JPH::Vec4(ibmRaw[0], ibmRaw[1], ibmRaw[2], ibmRaw[3]),
									 JPH::Vec4(ibmRaw[4], ibmRaw[5], ibmRaw[6], ibmRaw[7]),
									 JPH::Vec4(ibmRaw[8], ibmRaw[9], ibmRaw[10], ibmRaw[11]),
									 JPH::Vec4(ibmRaw[12], ibmRaw[13], ibmRaw[14], ibmRaw[15]));
				}
				finalSkinningMatrices[j] = physicalWorldJoints[j] * ibm;
			}

			// Update localTransform of visual meshes to match the physical pelvis root offset
			// (actualRootOffset)
			auto allEntities = reg.GetEntitiesWith<MeshComponent>();
			auto allMeshes = reg.GetRawArray<MeshComponent>();
			for (size_t k = 0; k < allEntities.size(); ++k) {
				if (allMeshes[k].gltfSkin == skin) {
					allMeshes[k].localTransform = JPH::Mat44::sTranslation(
						JPH::Vec3(actualRootOffset)); // <-- UPDATED (Translation)
				}
			}

			rc.UpdateJointMatrices(ragComp.jointOffset, finalSkinningMatrices.data(),
								   ragComp.jointCount);

			// Sync the CharacterVirtual capsule so the camera continues to follow the falling body
			if (phys != nullptr) {
				Physics::SetCharacterVelocity(engine.GetPhysicsContext(), phys->physicsHandle,
											  JPH::Vec3::sZero());
				Physics::SetCharacterPosition(engine.GetPhysicsContext(), phys->physicsHandle,
											  actualRootOffset);
			}
		}
	}
}

} // namespace ZHLN
