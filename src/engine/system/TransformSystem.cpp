// src/engine/system/TransformSystem.cpp
#include "TransformSystem.hpp"

#include <Zahlen/Components.hpp>
#include <ecs/ECS.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN {

// Helper to calculate the logical world-space transform of an entity
static JPH::Mat44 GetLogicalWorldTransform(const ECS::Registry& reg, Entity e) noexcept {
	const auto* trans = reg.Get<TransformComponent>(e);
	JPH::Mat44 localMatrix = (trans != nullptr) ? trans->GetMatrix() : JPH::Mat44::sIdentity();

	const auto* hierarchy = reg.Get<HierarchyComponent>(e);
	if ((hierarchy != nullptr) && hierarchy->parent != NullEntity &&
		reg.IsAlive(hierarchy->parent)) {
		static thread_local int recursionDepth = 0;
		if (recursionDepth > 16) {
			return localMatrix;
		}
		recursionDepth++;
		JPH::Mat44 parentLogical = GetLogicalWorldTransform(reg, hierarchy->parent);
		recursionDepth--;
		return parentLogical * localMatrix;
	}
	return localMatrix;
}

JPH::Mat44 TransformSystem::GetWorldTransform(const ECS::Registry& reg, Entity e) const noexcept {
	const auto* mesh = reg.Get<MeshComponent>(e);
	JPH::Mat44 meshLocal = (mesh != nullptr) ? mesh->localTransform : JPH::Mat44::sIdentity();

	const auto* trans = reg.Get<TransformComponent>(e);
	JPH::Mat44 localMatrix = (trans != nullptr) ? trans->GetMatrix() : JPH::Mat44::sIdentity();

	const auto* hierarchy = reg.Get<HierarchyComponent>(e);
	if ((hierarchy != nullptr) && hierarchy->parent != NullEntity &&
		reg.IsAlive(hierarchy->parent)) {
		// Retrieve only the logical parent matrix (bypassing the parent's visual offset)
		JPH::Mat44 parentLogical = GetLogicalWorldTransform(reg, hierarchy->parent);

		// If the node is animated, meshLocal is already computed relative to the glTF root
		// by the AnimationSystem. We skip multiplying by the redundant static localMatrix.
		if ((mesh != nullptr) && mesh->gltfNode != nullptr && !mesh->isSkinned) {
			return parentLogical * meshLocal;
		}

		return parentLogical * localMatrix * meshLocal;
	}

	return localMatrix * meshLocal;
}

void TransformSystem::ResolveTransforms(ECS::Registry& reg) const noexcept {
	auto entities = reg.GetEntitiesWith<MeshComponent>();
	auto meshes = reg.GetRawArray<MeshComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		MeshComponent& mesh = meshes[i];
		Entity e = entities[i];
		mesh.worldTransform = GetWorldTransform(reg, e);
	}
}

void TransformSystem::UpdateTransformHistory(ECS::Registry& reg) noexcept {
	auto entities = reg.GetEntitiesWith<MeshComponent>();
	auto meshes = reg.GetRawArray<MeshComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		MeshComponent& mesh = meshes[i];
		mesh.prevTransform = mesh.worldTransform;
	}
}

} // namespace ZHLN
