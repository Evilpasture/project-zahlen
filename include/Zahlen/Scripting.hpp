// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Engine.hpp>
#include <string_view>

// Forward declare lua_State to keep the header clean
struct lua_State;

namespace ZHLN {

/**
 * @brief High-performance Scripting Environment wrapper.
 * Manages the LuaJIT VM and the Buffer Protocol bridge.
 */
class ScriptRunner {
  public:
	ScriptRunner();
	~ScriptRunner();

	// Non-copyable
	ScriptRunner(const ScriptRunner&) = delete;
	ScriptRunner& operator=(const ScriptRunner&) = delete;

	/**
	 * @brief Loads and executes a Lua script.
	 */
	void RunFile(std::string_view path);

	/**
	 * @brief Calls the global 'update(engine, dt)' function in Lua.
	 */
	void CallUpdate(Engine* engine, float dt);

	void ExecuteString(std::string_view code);

	void ReloadFile(std::string_view path);

  private:
	lua_State* L;
};

} // namespace ZHLN
