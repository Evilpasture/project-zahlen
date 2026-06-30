// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "LuaScriptRuntime.hpp"

#include <Zahlen/Console.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <cstring>
#include <format>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace {

int LuaBridge_Log(lua_State* L) {
	lua_Debug ar;
	std::memset(&ar, 0, sizeof(lua_Debug));

	if (lua_getstack(L, 1, &ar)) {
		lua_getinfo(L, "Sl", &ar);
	} else {
		std::strncpy(ar.short_src, "unknown", sizeof(ar.short_src) - 1);
		ar.currentline = 0;
	}

	int n = lua_gettop(L);
	std::string msg;

	lua_getglobal(L, "tostring");

	for (int i = 1; i <= n; i++) {
		lua_pushvalue(L, -1);
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);

		size_t len = 0;
		const char* s = lua_tolstring(L, -1, &len);
		if (i > 1) {
			msg += "\t";
		}
		if (s != nullptr) {
			msg += std::string(s, len);
		}

		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	std::string_view file = ar.short_src;
	if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos) {
		file.remove_prefix(pos + 1);
	}

	ZHLN::LogManual(file, ar.currentline, msg, ZHLN::Color::Green);
	ZHLN::GameConsole::Log(msg, {.r = 0.4f, .g = 1.0f, .b = 0.4f, .a = 1.0f});

	return 0;
}

int LuaBridge_Warn(lua_State* L) {
	lua_Debug ar;
	std::memset(&ar, 0, sizeof(lua_Debug));

	if (lua_getstack(L, 1, &ar)) {
		lua_getinfo(L, "Sl", &ar);
	} else {
		std::strncpy(ar.short_src, "unknown", sizeof(ar.short_src) - 1);
		ar.currentline = 0;
	}

	int n = lua_gettop(L);
	std::string msg;

	lua_getglobal(L, "tostring");

	for (int i = 1; i <= n; i++) {
		lua_pushvalue(L, -1);
		lua_pushvalue(L, i);
		lua_call(L, 1, 1);

		size_t len = 0;
		const char* s = lua_tolstring(L, -1, &len);
		if (i > 1) {
			msg += "\t";
		}
		if (s != nullptr) {
			msg += std::string(s, len);
		}

		lua_pop(L, 1);
	}
	lua_pop(L, 1);

	std::string_view file = ar.short_src;
	if (auto pos = file.find_last_of("/\\"); pos != std::string_view::npos) {
		file.remove_prefix(pos + 1);
	}

	ZHLN::LogManual(file, ar.currentline, msg, ZHLN::Color::Yellow);

	return 0;
}

} // namespace

namespace ZHLN {

LuaScriptRuntime::LuaScriptRuntime() : L(luaL_newstate()) {
	if (L != nullptr) {
		luaL_openlibs(L);

		lua_newtable(L);
		lua_pushcfunction(L, LuaBridge_Log);
		lua_setfield(L, -2, "log");
		lua_pushcfunction(L, LuaBridge_Warn);
		lua_setfield(L, -2, "warn");
		lua_setglobal(L, "zahlen");

		lua_pushcfunction(L, LuaBridge_Log);
		lua_setglobal(L, "print");
	}
}

LuaScriptRuntime::~LuaScriptRuntime() {
	Shutdown();
}

void LuaScriptRuntime::Initialize(Engine* engine) {
	if (L == nullptr) {
		return;
	}

#ifdef ZHLN_COMPILED_SCRIPTS_DIR
	// Append the compiled build scripts directory to package.path
	// so the runtime can always locate compiled Fennel output files.
	std::string appendPath = std::format("package.path = package.path .. ';{}/?.lua;{}/?/init.lua'",
										 ZHLN_COMPILED_SCRIPTS_DIR, ZHLN_COMPILED_SCRIPTS_DIR);
	luaL_dostring(L, appendPath.c_str());
#endif

	// Load the C++ memoryview binding first
	lua_getglobal(L, "require");
	lua_pushstring(L, "scripts.core.memoryview");
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		Panic("Failed to load core script: scripts/core/memoryview.lua. Error: {}",
			  lua_tostring(L, -1));
	}
	lua_pop(L, 1);

	// Execute our Fennel bootstrapper
	RunFile("scripts/boot.lua");
}

void LuaScriptRuntime::Shutdown() {
	if (L != nullptr) {
		lua_close(L);
		L = nullptr;
	}
}

void LuaScriptRuntime::RunFile(std::string_view path) {
	if (L == nullptr) {
		return;
	}
	std::string p(path);
	if (luaL_dofile(L, p.c_str()) != LUA_OK) {
		Log("Lua Error in {}: {}", path, lua_tostring(L, -1));
		lua_pop(L, 1);
	}
}

void LuaScriptRuntime::ExecuteString(std::string_view code) {
	if (L == nullptr) {
		return;
	}
	if (luaL_dostring(L, code.data()) != LUA_OK) {
		std::string err = lua_tostring(L, -1);
		ZHLN::GameConsole::Log("Lua Error: " + err, {1.0f, 0.4f, 0.4f, 1.0f});
		lua_pop(L, 1);
	}
}

void LuaScriptRuntime::ReloadFile(std::string_view path) {
	if (L == nullptr) {
		return;
	}
	std::string moduleName = std::string(path);

	if (size_t pos = moduleName.find(".lua"); pos != std::string::npos) {
		moduleName.erase(pos);
	}
	std::replace(moduleName.begin(), moduleName.end(), '/', '.');

	std::string resetCode = std::format("package.loaded['{}'] = nil", moduleName);
	luaL_dostring(L, resetCode.c_str());

	RunFile(path);

	Log("Script Hot-Reloaded: {}", path);
	ZHLN::GameConsole::Log("Hot-Reloaded: " + std::string(path),
						   {.r = 0.2f, .g = 0.8f, .b = 1.0f, .a = 1.0f});
}

void LuaScriptRuntime::TickUpdate(Engine* engine, float dt) {
	if (L == nullptr) {
		return;
	}

	lua_getglobal(L, "update");
	if (lua_isfunction(L, -1)) {
		lua_pushlightuserdata(L, engine);
		lua_pushnumber(L, dt);

		if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
			Log("Lua Error during update: {}", lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	} else {
		lua_pop(L, 1);
	}

	lua_getglobal(L, "require");
	lua_pushstring(L, "scripts.core.zahlen");
	if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
		lua_getfield(L, -1, "cleanup");
		if (lua_isfunction(L, -1)) {
			if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
				Log("Lua Error during cleanup: {}", lua_tostring(L, -1));
				lua_pop(L, 1);
			}
		} else {
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
	} else {
		Log("Lua Error requiring zahlen core: {}", lua_tostring(L, -1));
		lua_pop(L, 1);
	}
}

} // namespace ZHLN
