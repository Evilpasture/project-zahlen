// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <string_view>

namespace ZHLN {
class Engine;

class IScriptRuntime {
  public:
	virtual ~IScriptRuntime() = default;

	virtual void Initialize(Engine* engine) = 0;
	virtual void Shutdown() = 0;

	virtual void RunFile(std::string_view path) = 0;
	virtual void ExecuteString(std::string_view code) = 0;
	virtual void ReloadFile(std::string_view path) = 0;

	virtual void TickUpdate(Engine* engine, float dt) = 0;
};
} // namespace ZHLN
