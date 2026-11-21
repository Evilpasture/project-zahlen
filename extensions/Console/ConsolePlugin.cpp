// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer for the Console extension. Wraps an
// application-owned ZHLN::GameConsole so a host can expose an in-game console
// through the standard plugin ABI (CreateAppLayer + the EntryPointWitness seal).
//
// The console itself is in-memory state: the host pumps input and renders
// entries through the GameConsole API; the plugin layer owns the instance's
// lifetime and gates per-frame work through PluginConfig.

#include <Console/Console.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <new>

namespace ZHLN::Console {

// Plugin-boundary configuration for the console layer. Kept out of the public
// extension headers so the extension itself knows nothing about the COM plugin
// ABI.
struct PluginConfig {
    bool enabled = true; // when false, OnTick is a no-op
};

} // namespace ZHLN::Console

namespace {

using namespace ZHLN;

class ConsolePlugin final : public COM::Implements<ConsolePlugin, COM::IAppLayer> {
  public:
    explicit ConsolePlugin(Console::PluginConfig config) noexcept
        : _config(std::move(config)) {}

    auto OnStart(COM::IEngine*) noexcept -> COM::Result<void> override {
        // GameConsole is default-constructed as a member; no engine state to
        // acquire. The host writes entries and reads history directly.
        return {};
    }

    auto OnTick(COM::IEngine* engine, float) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        if (!_config.enabled) {
            return {};
        }
        // Console input is pumped by the host through ConsoleDebugger::Execute
        // or its own UI; the layer only owns the instance here.
        (void)_console.ConsumeScroll();
        return {};
    }

    auto OnBeforeRender(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    void OnStop(COM::IEngine*) noexcept override {}

  private:
    GameConsole        _console {};
    Console::PluginConfig _config {};
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayer(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    ZHLN::Console::PluginConfig parsed {};
    const auto                checked = DispatchInChain<ZHLN::Console::PluginConfig>(
        config, [&](const ZHLN::Console::PluginConfig& value) noexcept { parsed = value; });
    if (!checked.HasValue()) {
        return checked.error;
    }
    auto* instance = new (std::nothrow) ConsolePlugin(parsed);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayer>::Seal() noexcept {}
