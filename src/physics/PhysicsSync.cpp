#include "PhysicsSync.hpp"

#include "detail/Span.hpp"
#include "physics/Physics.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyType.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <detail/Loop.hpp>
#include <detail/Prefetch.hpp>

// =================================================================================================
// UNIFIED SYNC PASS IMPLEMENTATION
// =================================================================================================
namespace {

constexpr uint32_t BATCH_SIZE = 128;

struct SyncWorkItem {
	const JPH::Body* body;
	uint32_t dense_idx;
};

#ifdef JPH_DOUBLE_PRECISION
inline constexpr bool IS_DOUBLE = true;
using PosPointerType = JPH::Double3* const ZHLN_RESTRICT;
#else
inline constexpr bool IS_DOUBLE = false;
#endif

using AuxPointerType = JPH::Float4* const ZHLN_RESTRICT;

template <JPH::EBodyType TType>
[[gnu::always_inline, gnu::nonnull(2)]]
inline void ProcessItem(const uint32_t D, const JPH::Body* const ZHLN_RESTRICT b,
						const ZHLN::Physics::Sync::WorldDataCreateInfo world) noexcept {

	// 1. Snapshot previous state (Center of Mass & Rotation)
	world.shadow_ppos[D] = world.shadow_pos[D];
	world.shadow_prot[D] = world.shadow_rot[D];

	// 2. Write Current COM Position
	auto* const targetPos = &world.shadow_pos[D];
	const auto& translation = b->GetCenterOfMassPosition();

	if constexpr (IS_DOUBLE) {
		[[clang::always_inline]] translation.StoreDouble3(
			reinterpret_cast<PosPointerType>(targetPos));
		targetPos->w = 0.0;
	} else {
		[[clang::always_inline]] JPH::Vec4(JPH::Vec3(translation), 0.0f)
			.StoreFloat4(reinterpret_cast<JPH::Float4* const ZHLN_RESTRICT>(targetPos));
	}

	// 3. Write Current Rotation
	const auto& rotation = b->GetRotation();
	[[clang::always_inline]] rotation.GetXYZW().StoreFloat4(
		reinterpret_cast<AuxPointerType>(&world.shadow_rot[D]));

	// 4. Write Velocities (Rigid Body Only)
	if constexpr (TType == JPH::EBodyType::RigidBody) {
		const auto& linVel = b->GetLinearVelocity();
		const auto& angVel = b->GetAngularVelocity();

		[[clang::always_inline]] JPH::Vec4(linVel, 0.0f)
			.StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_lvel[D]));

		[[clang::always_inline]] JPH::Vec4(angVel, 0.0f)
			.StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_avel[D]));
	}
}

template <JPH::EBodyType TType>
[[gnu::always_inline, gnu::hot, gnu::flatten]]
inline void ProcessBatch(const ZHLN::Physics::Sync::WorldDataCreateInfo world,
						 ZHLN::RestrictSpan<const SyncWorkItem> items) noexcept {

	const size_t count = items.size();
	[[assume(count > 0)]];
	[[assume(count <= BATCH_SIZE)]];

	ZHLN::UnrollLoop<4>(count, [&](auto j) {
		if (j + 2 < count) {
			const uint32_t next_idx = items[j + 2].dense_idx;
			ZHLN::Prefetch<ZHLN::AccessType::Write>(&world.shadow_pos[next_idx]);
			ZHLN::Prefetch<ZHLN::AccessType::Write>(&world.shadow_rot[next_idx]);
		}
		ProcessItem<TType>(items[j].dense_idx, items[j].body, world);
	});
}
} // namespace
namespace ZHLN::Physics::Sync {
template <JPH::EBodyType TType>
[[gnu::always_inline, gnu::flatten, gnu::nonnull(2)]]
void ExecuteSyncPass(const uint32_t active_count,
					 const JPH::PhysicsSystem* const ZHLN_RESTRICT system,
					 MappingDataCreateInfo map, const WorldDataCreateInfo world) noexcept {
	if (active_count == 0)
		return;

	const JPH::BodyID* const ZHLN_RESTRICT active_ids = system->GetActiveBodiesUnsafe(TType);
	if (active_ids == nullptr) [[unlikely]]
		return;

	const auto* const ZHLN_RESTRICT lock_iface = &system->GetBodyLockInterfaceNoLock();

	alignas(64) std::array<SyncWorkItem, BATCH_SIZE> worklist;
	uint32_t work_ptr = 0;

	for (uint32_t i = 0; i < active_count; i++) {
		const uint32_t raw_jolt_id = active_ids[i].GetIndexAndSequenceNumber();
		const uint32_t j_idx = raw_jolt_id & JPH::BodyID::cMaxBodyIndex;

		const auto* ZHLN_RESTRICT b =
			static_cast<const JPH::Body * ZHLN_RESTRICT>(map.body_ptrs[j_idx]);

		[[clang::always_inline]]
		if (b == nullptr || b->GetID().GetIndexAndSequenceNumber() != raw_jolt_id) [[unlikely]] {
			b = lock_iface->TryGetBody(JPH::BodyID(raw_jolt_id));
			map.body_ptrs[j_idx] = b;
		}
		[[assume(b != nullptr)]];

		// ECS Handle decoding
		const uint64_t handle = b->GetUserData();
		const uint32_t slot = static_cast<uint32_t>(handle & 0xFFFFFFFF);
		const uint32_t gen = static_cast<uint32_t>(handle >> 32);

		const uint32_t safe_slot = (slot < map.slot_capacity) ? slot : 0;
		const uint32_t current_gen = map.generations[safe_slot].load(std::memory_order_relaxed);

		// Branchless Validation
		const uint32_t bad = static_cast<uint32_t>(slot >= map.slot_capacity) | (current_gen ^ gen);
		const uint32_t d_idx = map.slot_to_dense[safe_slot];
		const uint32_t is_valid = static_cast<uint32_t>(bad == 0);

		[[assume(work_ptr < BATCH_SIZE)]];
		worklist[work_ptr].body = b;
		worklist[work_ptr].dense_idx = d_idx;
		work_ptr += is_valid;

		if (work_ptr == BATCH_SIZE) {
			ProcessBatch<TType>(world, RestrictSpan(worklist));
			work_ptr = 0;
		}
	}

	if (work_ptr > 0) {
		ProcessBatch<TType>(world, RestrictSpan(worklist).first(work_ptr));
	}
}

/**
 * @brief Optimized SIMD sync for CharacterVirtual objects.
 * Handles Position, Rotation, and Linear Velocity.
 */
[[gnu::always_inline]]
void SyncCharacters(const JPH::Array<JPH::CharacterVirtual*>& characters,
					const MappingDataCreateInfo& map, const WorldDataCreateInfo& world) noexcept {
	for (auto* character : characters) {
		const EntityHandle h = EntityHandle::Unpack(character->GetUserData());

		// In-loop validation (similar to rigid body pass)
		const uint32_t slot = h.index;
		if (slot >= map.slot_capacity) [[unlikely]]
			continue;

		const uint32_t D = map.slot_to_dense[slot];

		// 1. Snapshot previous state
		world.shadow_ppos[D] = world.shadow_pos[D];
		world.shadow_prot[D] = world.shadow_rot[D];

		// 2. Optimized Position Store
		const JPH::RVec3 pos = character->GetPosition();
		auto* const targetPos = &world.shadow_pos[D];
		if constexpr (IS_DOUBLE) {
			pos.StoreDouble3(reinterpret_cast<PosPointerType>(targetPos));
			targetPos->w = 0.0;
		} else {
			JPH::Vec4(JPH::Vec3(pos), 0.0f)
				.StoreFloat4(reinterpret_cast<AuxPointerType>(targetPos));
		}

		// 3. Optimized Rotation Store
		const JPH::Quat rot = character->GetRotation();
		rot.GetXYZW().StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_rot[D]));

		// 4. Optimized Velocity Store
		const JPH::Vec3 vel = character->GetLinearVelocity();
		JPH::Vec4(vel, 0.0f).StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_lvel[D]));
	}
}

} // namespace ZHLN::Physics::Sync