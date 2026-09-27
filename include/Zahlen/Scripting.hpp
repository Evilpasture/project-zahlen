// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/IScriptRuntime.hpp>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

namespace ZHLN {

class Engine;

class ScriptRunner {
  public:
    ScriptRunner()                                = default;
    ~ScriptRunner()                               = default;
    ScriptRunner(const ScriptRunner&)             = delete;
    ScriptRunner& operator=(const ScriptRunner&)  = delete;
    ScriptRunner(ScriptRunner&&) noexcept         = default;
    ScriptRunner& operator=(ScriptRunner&&) noexcept = default;

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

    [[nodiscard]] auto BootScriptPaths() const noexcept -> std::span<const std::string_view>;

    using RuntimeChanged = std::function<void()>;
    void                 SetRuntimeChanged(RuntimeChanged callback);

  private:
    std::unique_ptr<IScriptRuntime> _runtime;
    RuntimeChanged                  _onRuntimeChanged;
};

}
