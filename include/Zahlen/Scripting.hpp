// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Engine.hpp>
#include <memory>
#include <string_view>

namespace ZHLN {

class IScriptRuntime;

/**
 * @brief High-performance Scripting Environment wrapper.
 * Acts as the bridge to the underlying abstract Scripting Runtime.
 */
class ScriptRunner {
  public:
    ScriptRunner();
    ~ScriptRunner();

    // Non-copyable
    ScriptRunner(const ScriptRunner&)            = delete;
    ScriptRunner& operator=(const ScriptRunner&) = delete;

    /**
     * @brief Loads and executes a script.
     */
    void RunFile(std::string_view path);

    /**
     * @brief Calls the script update distribution loop.
     */
    void CallUpdate(Engine* engine, float dt);

    void ExecuteString(std::string_view code);

    void ReloadFile(std::string_view path);

    [[nodiscard]] IScriptRuntime* GetRuntime() const noexcept {
        return _runtime.get();
    }

  private:
    std::unique_ptr<IScriptRuntime> _runtime;
};

} // namespace ZHLN
