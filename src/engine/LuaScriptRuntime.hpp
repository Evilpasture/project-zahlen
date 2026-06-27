// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "IScriptRuntime.hpp"

struct lua_State;

namespace ZHLN {

class LuaScriptRuntime : public IScriptRuntime {
  public:
	LuaScriptRuntime();
	~LuaScriptRuntime() override;

	void Initialize(Engine* engine) override;
	void Shutdown() override;

	void RunFile(std::string_view path) override;
	void ExecuteString(std::string_view code) override;
	void ReloadFile(std::string_view path) override;

	void TickUpdate(Engine* engine, float dt) override;

  private:
	lua_State* L = nullptr;
};

} // namespace ZHLN
