// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <span>
#include <string_view>

namespace ZHLN {
class Engine;

class IScriptRuntime {
  public:
    virtual ~IScriptRuntime() = default;

    virtual void Initialize(Engine* engine) = 0;
    virtual void Shutdown()                 = 0;

    virtual void RunFile(std::string_view path)       = 0;
    virtual void ExecuteString(std::string_view code) = 0;
    virtual void ReloadFile(std::string_view path)    = 0;

    virtual void TickUpdate(Engine* engine, float dt) = 0;

    /// The boot entry points this runtime recognises, in priority order.
    ///
    /// Core watches these for hot reload and asks whether any of them exist to
    /// decide if a project shipped no boot script at all, so the runtime owns
    /// its own file convention and core never has to name a language. Return an
    /// empty span if the runtime has no boot-file convention.
    ///
    /// The span must stay valid for the lifetime of the runtime.
    [[nodiscard]] virtual auto BootScriptPaths() const noexcept -> std::span<const std::string_view> = 0;
};
} // namespace ZHLN
