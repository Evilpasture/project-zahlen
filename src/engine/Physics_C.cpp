#include <Zahlen/Common.h>
#include <Zahlen/Engine.hpp>
#include <Zahlen/physics/Physics_C.h>
#include <physics/Physics.hpp>

extern "C" {

void ZHLN_SetCharacterVelocity(ZHLN_Engine* engine_handle, uint64_t physicsHandleRaw, float x,
							   float y, float z) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	ZHLN::Entity handle = ZHLN::Entity::Unpack(physicsHandleRaw);
	ZHLN::Physics::SetCharacterVelocity(engine->GetPhysicsContext(), handle, {x, y, z});
}

int ZHLN_IsCharacterOnGround(ZHLN_Engine* engine_handle, uint64_t physicsHandleRaw) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	ZHLN::Entity handle = ZHLN::Entity::Unpack(physicsHandleRaw);
	return ZHLN::Physics::IsCharacterOnGround(engine->GetPhysicsContext(), handle) ? 1 : 0;
}

void ZHLN_SetLinearVelocity(ZHLN_Engine* engine_handle, uint64_t physicsHandleRaw, float x, float y,
							float z) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	ZHLN::Entity handle = ZHLN::Entity::Unpack(physicsHandleRaw);
	ZHLN::Physics::SetLinearVelocity(engine->GetPhysicsContext(), handle, {x, y, z});
}

void ZHLN_AddImpulse(ZHLN_Engine* engine_handle, uint64_t physicsHandleRaw, float x, float y,
					 float z) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	ZHLN::Entity handle = ZHLN::Entity::Unpack(physicsHandleRaw);

	// Call the implementation we just added to Physics.cpp
	ZHLN::Physics::AddImpulse(engine->GetPhysicsContext(), handle, JPH::Vec3(x, y, z));
}

ZHLN_RaycastResult ZHLN_Raycast(ZHLN_Engine* engine_handle, double ox, double oy, double oz,
								float dx, float dy, float dz, float maxDist,
								uint64_t ignoreEntity) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);

	// Unpack the ignore entity if one was provided
	ZHLN::Entity ignore = ignoreEntity != 0 ? ZHLN::Entity::Unpack(ignoreEntity) : ZHLN::Entity{};

	// Call the internal C++ Raycast
	auto res = ZHLN::Physics::Raycast(engine->GetPhysicsContext(), JPH::RVec3(ox, oy, oz),
									  JPH::Vec3(dx, dy, dz), maxDist, ignore);

	ZHLN_RaycastResult out{};
	out.hasHit = res.hasHit ? 1 : 0;

	if (res.hasHit) {
		out.entity = res.handle.Pack();
		out.px = res.position.GetX();
		out.py = res.position.GetY();
		out.pz = res.position.GetZ();
		out.nx = res.normal.GetX();
		out.ny = res.normal.GetY();
		out.nz = res.normal.GetZ();
		out.fraction = res.fraction;
	}
	return out;
}
}