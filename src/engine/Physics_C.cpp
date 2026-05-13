#include <Zahlen/Common.h>
#include <Zahlen/Engine.hpp>
#include <Zahlen/physics/Physics_C.h>

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
}