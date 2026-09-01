// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Scripting.hpp
#pragma once

#include <Zahlen/IScriptRuntime.hpp>
#include <memory>
#include <string_view>

namespace ZHLN {

class Engine;

/// Core's handle on a scripting runtime.
///
/// Core owns no scripting implementation. It does not know what a lua_State, a
/// C ABI, a marshalling table or a managed delegate is -- those all live in
/// extras/scripting_lua, which implements IScriptRuntime and installs itself
/// here. ScriptRunner forwards to whatever is installed and every method is a
/// no-op while nothing is, so a build without a scripting extra still runs and
/// core call sites need no guards of their own.
class ScriptRunner {
  public:
    ScriptRunner()                                = default;
    ~ScriptRunner()                               = default;
    ScriptRunner(const ScriptRunner&)             = delete;
    ScriptRunner& operator=(const ScriptRunner&)  = delete;
    ScriptRunner(ScriptRunner&&) noexcept         = default;
    ScriptRunner& operator=(ScriptRunner&&) noexcept = default;

    /// Installs the runtime and takes ownership of it.
    void SetRuntime(std::unique_ptr<IScriptRuntime> runtime);

    [[nodiscard]] auto HasRuntime() const noexcept -> bool {
        return _runtime != nullptr;
    }

    [[nodiscard]] auto GetRuntime() const noexcept -> IScriptRuntime* {
        return _runtime.get();
    }

    void RunFile(std::string_view path);
    void CallUpdate(Engine* engine, float dt);
    void ExecuteString(std::string_view code);
    void ReloadFile(std::string_view path);

  private:
    std::unique_ptr<IScriptRuntime> _runtime;
};

} // namespace ZHLN
