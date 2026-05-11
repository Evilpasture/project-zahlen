#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyType.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <cstdint>
#include <detail/Atomic.hpp>
#include <detail/Platform.hpp>
namespace ZHLN::Physics::Sync {
struct alignas(32) PosStride {
	JPH::Real x, y, z, w;
};
struct alignas(16) AuxStride {
	float x, y, z, w;
};

static_assert(sizeof(PosStride) == sizeof(JPH::Real) * 4);
static_assert(sizeof(AuxStride) == sizeof(float) * 4);
static_assert(sizeof(PosStride) % 32 == 0);

struct WorldDataCreateInfo {
	PosStride* const ZHLN_RESTRICT shadow_pos;
	PosStride* const ZHLN_RESTRICT shadow_ppos;
	AuxStride* const ZHLN_RESTRICT shadow_rot;
	AuxStride* const ZHLN_RESTRICT shadow_prot;
	AuxStride* const ZHLN_RESTRICT shadow_lvel;
	AuxStride* const ZHLN_RESTRICT shadow_avel;
};

struct MappingDataCreateInfo {
	const void* ZHLN_RESTRICT* const ZHLN_RESTRICT body_ptrs;
	const ZHLN::Atomic<uint32_t>* const ZHLN_RESTRICT generations;
	const size_t slot_capacity;
	const uint32_t* const ZHLN_RESTRICT slot_to_dense;
};

template <JPH::EBodyType TType>
void ExecuteSyncPass(const uint32_t active_count,
					 const JPH::PhysicsSystem* const ZHLN_RESTRICT system,
					 MappingDataCreateInfo map, const WorldDataCreateInfo world) noexcept;

void SyncCharacters(const JPH::Array<JPH::CharacterVirtual*>& characters,
					const MappingDataCreateInfo& map, const WorldDataCreateInfo& world) noexcept;
} // namespace ZHLN::Physics::Sync