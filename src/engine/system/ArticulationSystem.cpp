// src/engine/system/ArticulationSystem.cpp
#include "ArticulationSystem.hpp" // <-- Local relative include for the moved header

#include <Jolt/Jolt.h> // <--- Defensive first Jolt include
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <cstring>
#include <detail/ControlFlow.hpp>
#include <ecs/ECS.hpp>
#include <physics/Physics.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN {

void ArticulationSystem::Update(Engine& engine, [[maybe_unused]] float dt) {
	auto& reg = engine.GetRegistry();
	const auto& world = engine.GetPhysicsContext().GetWorld();
	auto& rc = engine.GetRenderContext();

	auto entities = reg.GetEntitiesWith<RagdollComponent>();
	auto ragdolls = reg.GetRawArray<RagdollComponent>();

	if (entities.empty()) {
		return;
	}

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		RagdollComponent& ragComp = ragdolls[i];
		auto* mesh = reg.Get<MeshComponent>(e);

		if ((mesh == nullptr) || (ragComp.ragdollInstance == nullptr)) {
			continue;
		}

		JPH::Ragdoll* ragdoll = ragComp.ragdollInstance.GetPtr();
		const JPH::RagdollSettings* settings = ragdoll->GetRagdollSettings();
		const JPH::Skeleton* skel = settings->GetSkeleton();

		if (ragComp.state == RagdollState::KeyframeMotor) {
			if (!ragComp.isAddedToPhysics) {
				ZHLN_LOCK(world.sync.shadowLock) {
					ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
					ragComp.isAddedToPhysics = true;
				}
			}

			JPH::SkeletonPose pose;
			pose.SetSkeleton(skel);

			auto rootOffset = JPH::RVec3(mesh->localTransform.GetTranslation());
			pose.SetRootOffset(rootOffset);

			std::vector<JPH::Mat44> localJoints(ragComp.jointCount, JPH::Mat44::sIdentity());
			// (Visual joints are populated here from your animation keyframes)

			std::memcpy(pose.GetJointMatrices().data(), localJoints.data(),
						ragComp.jointCount * sizeof(JPH::Mat44));
			pose.CalculateJointStates();

			ZHLN_LOCK(world.sync.shadowLock) {
				ragdoll->Activate();
				ragdoll->DriveToPoseUsingMotors(pose);
			}
		} else if (ragComp.state == RagdollState::Limp) {
			if (!ragComp.isAddedToPhysics) {
				ZHLN_LOCK(world.sync.shadowLock) {
					ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
					ragComp.isAddedToPhysics = true;
				}
			}

			std::vector<JPH::Mat44> physicalJointMatrices(ragComp.jointCount,
														  JPH::Mat44::sIdentity());
			JPH::RVec3 rootOffset = JPH::RVec3::sZero();

			ZHLN_LOCK(world.sync.shadowLock) {
				ragdoll->GetPose(rootOffset, physicalJointMatrices.data());
			}

			mesh->localTransform = JPH::Mat44::sTranslation(JPH::Vec3(rootOffset));

			rc.UpdateJointMatrices(ragComp.jointOffset, physicalJointMatrices.data(),
								   ragComp.jointCount);

			JPH::AABox bounds = ragdoll->GetWorldSpaceBounds();
			mesh->cullRadius = bounds.GetExtent().Length() * 1.5f;
		} else if (ragComp.state == RagdollState::Inactive) {
			if (ragComp.isAddedToPhysics) {
				ZHLN_LOCK(world.sync.shadowLock) {
					ragdoll->RemoveFromPhysicsSystem();
					ragComp.isAddedToPhysics = false;
				}
			}
		}
	}
}

} // namespace ZHLN
