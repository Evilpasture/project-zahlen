// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/IScriptRuntime.hpp>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

struct lua_State;

namespace ZHLN {

class LuaScriptRuntime: public IScriptRuntime {
  public:
    LuaScriptRuntime();
    ~LuaScriptRuntime() override;

    // Extras can add LuaJIT bindings to this runtime without creating another
    // state or changing the core IScriptRuntime contract. Initializers run
    // before any boot/user script; their captures remain alive until Shutdown.
    using BindingInitializer = std::function<void(lua_State*)>;
    void AddBindingInitializer(BindingInitializer initializer);

    void Initialize(Engine* engine) override;
    void Shutdown() override;

    void RunFile(std::string_view path) override;
    void ExecuteString(std::string_view code) override;
    void ReloadFile(std::string_view path) override;

    void TickUpdate(Engine* engine, float dt) override;

    // scripts/boot.lua, then scripts/boot.fnl. This runtime owns that
    // convention; core asks for it instead of hardcoding either name.
    [[nodiscard]] auto BootScriptPaths() const noexcept -> std::span<const std::string_view> override;

  private:
    lua_State* L            = nullptr;
    bool       _initialized = false;
    std::vector<BindingInitializer> _bindingInitializers;
};

} // namespace ZHLN
