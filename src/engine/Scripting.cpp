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

extern "C" {
/**
 * @brief Internal C-Function registered to Lua: zhln.log(...)
 */
static int LuaBridge_Log(lua_State* L) {
	// 1. Get Lua Source Info
	lua_Debug ar;
	std::memset(&ar, 0, sizeof(lua_Debug));

	// Level 1 is the function that called this C bridge
	if (lua_getstack(L, 1, &ar)) {
		lua_getinfo(L, "Sl", &ar);
	} else {
		// Fix: Use strncpy for fixed-size char arrays
		std::strncpy(ar.short_src, "unknown", sizeof(ar.short_src) - 1);
		ar.currentline = 0;
	}

	// 2. Concatenate all arguments into a single string
	int n = lua_gettop(L);
	std::string msg;

	// We get the global "tostring" function once to use it for all arguments
	// this ensures we respect Lua metamethods for tables/userdata.
	lua_getglobal(L, "tostring");

	for (int i = 1; i <= n; i++) {
		lua_pushvalue(L, -1); // Push the 'tostring' function
		lua_pushvalue(L, i);  // Push the argument
		lua_call(L, 1, 1);	  // Call tostring(arg)

		size_t len;
		const char* s = lua_tolstring(L, -1, &len);
		if (i > 1)
			msg += "\t";
		if (s)
			msg += std::string(s, len);

		lua_pop(L, 1); // Pop the string result
	}
	lua_pop(L, 1); // Pop the 'tostring' function

	// 3. Hand off to our manual C++ Logger
	std::string_view file = ar.short_src;
	if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos)
		file.remove_prefix(pos + 1);

	// Call your Engine's LogManual (defined in Log.hpp)
	LogManual(file, ar.currentline, msg, Color::Green);

	return 0;
}

/**
 * @brief Internal C-Function registered to Lua: zhln.log(...)
 */
static int LuaBridge_Warn(lua_State* L) {
	// 1. Get Lua Source Info
	lua_Debug ar;
	std::memset(&ar, 0, sizeof(lua_Debug));

	// Level 1 is the function that called this C bridge
	if (lua_getstack(L, 1, &ar)) {
		lua_getinfo(L, "Sl", &ar);
	} else {
		// Fix: Use strncpy for fixed-size char arrays
		std::strncpy(ar.short_src, "unknown", sizeof(ar.short_src) - 1);
		ar.currentline = 0;
	}

	// 2. Concatenate all arguments into a single string
	int n = lua_gettop(L);
	std::string msg;

	// We get the global "tostring" function once to use it for all arguments
	// this ensures we respect Lua metamethods for tables/userdata.
	lua_getglobal(L, "tostring");

	for (int i = 1; i <= n; i++) {
		lua_pushvalue(L, -1); // Push the 'tostring' function
		lua_pushvalue(L, i);  // Push the argument
		lua_call(L, 1, 1);	  // Call tostring(arg)

		size_t len;
		const char* s = lua_tolstring(L, -1, &len);
		if (i > 1)
			msg += "\t";
		if (s)
			msg += std::string(s, len);

		lua_pop(L, 1); // Pop the string result
	}
	lua_pop(L, 1); // Pop the 'tostring' function

	// 3. Hand off to our manual C++ Logger
	std::string_view file = ar.short_src;
	if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos)
		file.remove_prefix(pos + 1);

	LogManual(file, ar.currentline, msg, Color::Yellow);

	return 0;
}
}

ScriptRunner::ScriptRunner() {
	L = luaL_newstate();
	luaL_openlibs(L);

	// Register our specialized logging into the global 'zhln' table
	lua_newtable(L);
	lua_pushcfunction(L, LuaBridge_Log);
	lua_setfield(L, -2, "log");
	lua_pushcfunction(L, LuaBridge_Warn); // Re-use for now or make Warn
	lua_setfield(L, -2, "warn");
	lua_setglobal(L, "zhln");

	// Override global print to use our engine logger
	lua_pushcfunction(L, LuaBridge_Log);
	lua_setglobal(L, "print");
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
		Log("Lua Error: {}", lua_tostring(L, -1));
		lua_pop(L, 1);
	}

	// --- NEW: FORCED CLEANUP ---
	// This calls zhln.cleanup() which releases the C++ Mutexes
	// before the C++ Physics Step starts.
	lua_getglobal(L, "require");
	lua_pushstring(L, "scripts.core.zhln");
	lua_pcall(L, 1, 1, 0);
	lua_getfield(L, -1, "cleanup");
	lua_pcall(L, 0, 0, 0);
	lua_pop(L, 1); // pop zhln table
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

void ZHLN_SetLinearVelocity(ZHLN_Engine* engine_handle, uint64_t physicsHandleRaw, float x, float y,
							float z) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	ZHLN::Entity handle = ZHLN::Entity::Unpack(physicsHandleRaw);
	ZHLN::Physics::SetLinearVelocity(engine->GetPhysicsContext(), handle, {x, y, z});
}
float ZHLN_GetCameraYaw(ZHLN_Engine* engine_handle) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	return engine->GetCamera().yaw;
}

void ZHLN_AddImpulse(ZHLN_Engine* engine_handle, uint64_t physicsHandleRaw, float x, float y,
					 float z) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	ZHLN::Entity handle = ZHLN::Entity::Unpack(physicsHandleRaw);

	// Call the implementation we just added to Physics.cpp
	ZHLN::Physics::AddImpulse(engine->GetPhysicsContext(), handle, JPH::Vec3(x, y, z));
}
}