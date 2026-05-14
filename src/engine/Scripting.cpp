#include <Zahlen/Console.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/physics/Physics_C.h>
#include <cstring>
#include <physics/PhysicsWorld.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace ZHLN {

// --- ScriptRunner Implementation ---

extern "C" {
/**
 * @brief Internal C-Function registered to Lua: zahlen.log(...)
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
	ZHLN::GameConsole::Log(msg, {0.4f, 1.0f, 0.4f, 1.0f});

	return 0;
}

/**
 * @brief Internal C-Function registered to Lua: zahlen.log(...)
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

	// Register our specialized logging into the global 'zahlen' table
	lua_newtable(L);
	lua_pushcfunction(L, LuaBridge_Log);
	lua_setfield(L, -2, "log");
	lua_pushcfunction(L, LuaBridge_Warn); // Re-use for now or make Warn
	lua_setfield(L, -2, "warn");
	lua_setglobal(L, "zahlen");

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
	// This calls zahlen.cleanup() which releases the C++ Mutexes
	// before the C++ Physics Step starts.
	lua_getglobal(L, "require");
	lua_pushstring(L, "scripts.core.zahlen");
	lua_pcall(L, 1, 1, 0);
	lua_getfield(L, -1, "cleanup");
	lua_pcall(L, 0, 0, 0);
	lua_pop(L, 1); // pop zahlen table
}

void ScriptRunner::ExecuteString(std::string_view code) {
	if (luaL_dostring(L, code.data()) != LUA_OK) {
		std::string err = lua_tostring(L, -1);
		ZHLN::GameConsole::Log("Lua Error: " + err, {1.0f, 0.4f, 0.4f, 1.0f});
		lua_pop(L, 1);
	}
}

} // namespace ZHLN

// --- C-API Exports ---
extern "C" {

int ZHLN_IsKeyDown(ZHLN_Engine* engine_handle, uint8_t key) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	return engine->GetInput().IsKeyDown(static_cast<ZHLN::KeyCode>(key)) ? 1 : 0;
}

void ZHLN_GetMouseDelta(ZHLN_Engine* engine_handle, float* outX, float* outY) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	*outX = engine->GetInput().GetMouse().deltaX;
	*outY = engine->GetInput().GetMouse().deltaY;
}

float ZHLN_GetCameraYaw(ZHLN_Engine* engine_handle) {
	auto* engine = reinterpret_cast<ZHLN::Engine*>(engine_handle);
	return engine->GetCamera().yaw;
}
}