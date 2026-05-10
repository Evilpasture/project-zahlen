#include <Zahlen/Log.hpp>
#include <Zahlen/PhysicsWorld.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <cstring>

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
	template <typename T> static void Acquire(const T* obj) {
		// Because T is now the actual type (e.g. PhysicsWorld), members are visible.
		if (obj->viewExportCount.fetch_add(1, std::memory_order_acquire) == 0) {
			obj->shadowLock.lock();
		}
	}

	template <typename T> static void Release(const T* obj) {
		if (obj->viewExportCount.fetch_sub(1, std::memory_order_release) == 1) {
			obj->shadowLock.unlock();
		}
	}
};

struct ViewComposer {
	/**
	 * @brief The "Universal Builder".
	 *
	 * @tparam TOwner The engine struct type (e.g., PhysicsWorld)
	 * @tparam TData  The primitive type (e.g., float)
	 * @tparam Dims   Variadic pack of dimensions
	 */
	template <typename TOwner, typename TData, typename... Dims>
	static ZHLN_BufferView Build(const TOwner* owner, TData* data, const char* format,
								 Dims... dims) {

		// 1. Thread Synchronization via Policy
		SyncPolicy::Acquire(owner);

		ZHLN_BufferView view = {};
		view.buf = (void*)data; // data is Real*, Real* const when world is const. Safe.
		view.obj = (void*)owner;
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
		if (world.viewExportCount.load() > 0) {
			world.viewExportCount.store(0);
			world.shadowLock.unlock();
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

void ZHLN_ReleaseBuffer(void* owner) {
	// Uses the same policy to ensure symmetric locking
	ZHLN::SyncPolicy::Release(static_cast<const ZHLN::Physics::PhysicsWorld*>(owner));
}
}