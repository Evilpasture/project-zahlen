// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/ScriptRunner.cpp
//
// The forwarding half of ScriptRunner. Deliberately the whole of core's
// scripting support: it holds an IScriptRuntime and calls through it, so the
// engine, the fallback preset and the console can ask for script work without
// knowing whether anything is listening.

#include <Zahlen/Scripting.hpp>
#include <utility>

namespace ZHLN {

void ScriptRunner::SetRuntime(std::unique_ptr<IScriptRuntime> runtime) {
    _runtime = std::move(runtime);
}

void ScriptRunner::RunFile(std::string_view path) {
    if (_runtime) {
        _runtime->RunFile(path);
    }
}

void ScriptRunner::CallUpdate(Engine* engine, float dt) {
    // Initialize is called on every tick, as it was before the runtime left
    // core; the implementation is expected to make it a no-op once primed.
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

} // namespace ZHLN
