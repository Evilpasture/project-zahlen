// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include <Zahlen/Scripting.hpp>
#include <utility>

namespace ZHLN {

void ScriptRunner::SetRuntime(std::unique_ptr<IScriptRuntime> runtime) {
    _runtime = std::move(runtime);
    if (_onRuntimeChanged) {
        _onRuntimeChanged();
    }
}

void ScriptRunner::SetRuntimeChanged(RuntimeChanged callback) {
    _onRuntimeChanged = std::move(callback);
}

auto ScriptRunner::BootScriptPaths() const noexcept -> std::span<const std::string_view> {
    return _runtime != nullptr ? _runtime->BootScriptPaths() : std::span<const std::string_view> {};
}

void ScriptRunner::RunFile(std::string_view path) {
    if (_runtime) {
        _runtime->RunFile(path);
    }
}

void ScriptRunner::CallUpdate(Engine* engine, float dt) {
    if (_runtime && engine != nullptr) {
        _runtime->Initialize(engine);
        _runtime->TickUpdate(engine, dt);
    }
}

void ScriptRunner::ExecuteString(std::string_view code) {
    if (_runtime) {
        _runtime->ExecuteString(code);
    }
}

void ScriptRunner::ReloadFile(std::string_view path) {
    if (_runtime) {
        _runtime->ReloadFile(path);
    }
}

}
