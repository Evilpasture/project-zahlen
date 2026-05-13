#include "Zahlen/Engine.hpp"
#include "Zahlen/Sync.hpp"

#include <Zahlen/Buffer.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Scripting.h>
#include <physics/PhysicsWorld.hpp>
namespace ZHLN {
/**
 * @brief Policy that defines how to "lock" an engine object for script access.
 */
struct SyncPolicy {
	// The "Universal" entry point
	static void Acquire(BufferSync* sync) {
		if (sync->viewExportCount.fetch_add(1, std::memory_order_acquire) == 0) {
			sync->shadowLock.lock();
		}
	}

	static void Release(BufferSync* sync) {
		if (sync->viewExportCount.fetch_sub(1, std::memory_order_release) == 1) {
			sync->shadowLock.unlock();
		}
	}
};

struct ViewComposer {
	template <typename TOwner, typename TData, typename... Dims>
	static ZHLN_BufferView Build(const TOwner* owner, TData* data, const char* format,
								 Dims... dims) {
		// 1. Thread Synchronization via Policy
		// Since sync is the first member, we can safely cast the owner to BufferSync
		auto* sync = reinterpret_cast<BufferSync*>(const_cast<TOwner*>(owner));
		SyncPolicy::Acquire(sync);

		ZHLN_BufferView view = {};
		view.buf = (void*)data;
		view.obj = (void*)sync; // Store the SYNC pointer, not the object pointer
		view.itemsize = sizeof(TData);
		std::strncpy(view.format, format, 7);
		view.readonly = 0;

		// 2. Setup Dimensions
		view.ndim = sizeof...(dims);
		size_t d_array[] = {static_cast<size_t>(dims)...};

		// 3. Recursive Stride Calculation (C-Contiguous / Row-Major)
		size_t stride = sizeof(TData);
		for (int i = (int)view.ndim - 1; i >= 0; --i) {
			view.shape[i] = d_array[i];
			view.strides[i] = stride;
			stride *= d_array[i];
		}

		view.len = stride; // Total footprint in bytes
		view.flags = ZHLN_BUFFER_CONTIGUOUS | ZHLN_BUFFER_WRITABLE;
		if (((uintptr_t)view.buf % 32) == 0)
			view.flags |= ZHLN_BUFFER_ALIGNED_32;

		return view;
	}
};
} // namespace ZHLN
extern "C" {

ZHLN_BufferView ZHLN_GetPhysicsPositions(ZHLN_Engine* engine_handle) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	const auto& world = engine->GetPhysicsContext().GetWorld();

	// No hardcoding '4' or 'ndim=2'. We just describe the logic:
	// A 2D array of [CurrentCount] by [4 components]
	return ZHLN::ViewComposer::Build(&world, world.positions, (sizeof(JPH::Real) == 8) ? "d" : "f",
									 world.count.load(), 4);
}

ZHLN_BufferView ZHLN_GetPhysicsLinearVelocities(ZHLN_Engine* engine_handle) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	const auto& world = engine->GetPhysicsContext().GetWorld();

	// If you ever change velocities to be 3-wide (x,y,z),
	// you just change '4' to '3' here. The composer handles the rest.
	return ZHLN::ViewComposer::Build(&world, world.linearVelocities, "f", world.count.load(), 4);
}

void ZHLN_ReleaseBuffer(void* sync_ptr) {
	// We know for a fact this is a BufferSync* because
	// ViewComposer put it there.
	ZHLN::SyncPolicy::Release(static_cast<ZHLN::BufferSync*>(sync_ptr));
}

ZHLN_BufferView ZHLN_GetECSBuffer(struct ZHLN_Engine* engine_handle, const char* componentName) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	auto& reg = engine->GetRegistry();
	std::string_view name(componentName);

	if (name == "PhysicsComponent") {
		auto raw = reg.GetRawArray<ZHLN::PhysicsComponent>();
		// Use the Composer to handle the Acquire() call automatically
		return ZHLN::ViewComposer::Build(&reg, raw.data(), "Q", raw.size());
	}
	return {};
}

ZHLN_BufferView ZHLN_GetECSEntities(struct ZHLN_Engine* engine_handle,
									const char* /*componentName*/) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	auto& reg = engine->GetRegistry();

	// Simplified: All components use the same entity dense array logic
	auto entities = reg.GetEntitiesWith<ZHLN::PhysicsComponent>();
	return ZHLN::ViewComposer::Build(&reg, const_cast<ZHLN::Entity*>(entities.data()), "Q",
									 entities.size());
}

ZHLN_BufferView ZHLN_GetPhysicsContactEvents(ZHLN_Engine* engine_handle) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	auto events = ZHLN::Physics::GetContactEvents(engine->GetPhysicsContext());

	// Choose format string based on Jolt precision
	const char* fmt = (sizeof(JPH::Real) == 8) ? "EvtD" : "EvtF";

	return ZHLN::ViewComposer::Build(&engine->GetPhysicsContext().GetWorld(), events.first, fmt,
									 events.second);
}
}