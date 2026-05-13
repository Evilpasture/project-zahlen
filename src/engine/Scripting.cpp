#include "Zahlen/Components.hpp"

#include <Zahlen/Log.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <cstring>
#include <physics/PhysicsWorld.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace ZHLN {

/**
 * @brief Policy that defines how to "lock" an engine object for script access.
 * The template T allows the compiler to see the actual struct members.
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

// --- ScriptRunner Implementation ---

ScriptRunner::ScriptRunner() {
	L = luaL_newstate();
	luaL_openlibs(L);
	lua_getglobal(L, "require");
	lua_pushstring(L, "scripts.core.memoryview");
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		Panic("Failed to load core script: scripts/core/memoryview.lua. Error: {}",
			  lua_tostring(L, -1));
	}
	lua_pop(L, 1);
}

ScriptRunner::~ScriptRunner() {
	if (L)
		lua_close(L);
}

void ScriptRunner::RunFile(std::string_view path) {
	std::string p(path);
	if (luaL_dofile(L, p.c_str()) != LUA_OK) {
		Log("Lua Error in {}: {}", path, lua_tostring(L, -1));
		lua_pop(L, 1);
	}
}

void ScriptRunner::CallUpdate(Engine* engine, float dt) {
	lua_getglobal(L, "update");
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		return;
	}

	lua_pushlightuserdata(L, engine);
	lua_pushnumber(L, dt);

	if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
		Log("Lua Error in update(): {}", lua_tostring(L, -1));
		// Emergency Recovery
		const auto& world = engine->GetPhysicsContext().GetWorld();
		if (world.sync.viewExportCount.load() > 0) {
			world.sync.viewExportCount.store(0);
			world.sync.shadowLock.unlock();
		}
		lua_pop(L, 1);
	}
}

} // namespace ZHLN

// --- C-API Exports ---
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

ZHLN_BufferView ZHLN_GetECSEntities(struct ZHLN_Engine* engine_handle, const char* componentName) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	auto& reg = engine->GetRegistry();

	// Simplified: All components use the same entity dense array logic
	auto entities = reg.GetEntitiesWith<ZHLN::PhysicsComponent>();
	return ZHLN::ViewComposer::Build(&reg, const_cast<ZHLN::Entity*>(entities.data()), "Q",
									 entities.size());
}

int ZHLN_IsKeyDown(ZHLN_Engine* engine_handle, uint8_t key) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	return engine->GetInput().IsKeyDown(static_cast<ZHLN::KeyCode>(key)) ? 1 : 0;
}

void ZHLN_GetMouseDelta(ZHLN_Engine* engine_handle, float* outX, float* outY) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	*outX = engine->GetInput().GetMouse().deltaX;
	*outY = engine->GetInput().GetMouse().deltaY;
}

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
}