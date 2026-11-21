// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer for the CharacterController extension.
// Wraps ZHLN::Character::Layer so a host can step character movement through
// the standard plugin ABI (CreateAppLayer + the EntryPointWitness seal).
//
// Configuration arrives through a ChainInNode payload (PluginConfig); an
// absent or empty chain means default configuration.

#include <CharacterController/CharacterController.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <new>

namespace ZHLN::Character {

// Plugin-boundary configuration for the character layer. Kept out of the
// public extension headers so the extension itself knows nothing about the
// COM plugin ABI.
struct PluginConfig {
    bool enabled      = true;   // when false, OnTick is a no-op
    bool detachOnStop = true;   // when false, OnStop leaves attached controllers in place
};

} // namespace ZHLN::Character

namespace {

using namespace ZHLN;

class CharacterControllerPlugin final
    : public COM::Implements<CharacterControllerPlugin, COM::IAppLayer> {
  public:
    explicit CharacterControllerPlugin(Character::PluginConfig config) noexcept
        : _config(std::move(config)) {}

    auto OnStart(COM::IEngine*) noexcept -> COM::Result<void> override {
        // Character::Layer is default-constructed; attachment of specific
        // entities is host-driven, so there is nothing to acquire here.
        return {};
    }

    auto OnTick(COM::IEngine* engine, float dt) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        if (!_config.enabled) {
            return {};
        }
        _layer.Tick(*engine, dt);
        return {};
    }

    auto OnBeforeRender(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    void OnStop(COM::IEngine* engine) noexcept override {
        if (engine == nullptr) {
            return;
        }
        if (_config.detachOnStop) {
            _layer.Stop(*engine);
        }
    }

  private:
    Character::Layer       _layer {};
    Character::PluginConfig _config {};
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayer(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    ZHLN::Character::PluginConfig parsed {};
    const auto                  checked = DispatchInChain<ZHLN::Character::PluginConfig>(
        config,
        [&](const ZHLN::Character::PluginConfig& value) noexcept { parsed = value; });
    if (!checked.HasValue()) {
        return checked.error;
    }
    auto* instance = new (std::nothrow) CharacterControllerPlugin(parsed);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayer>::Seal() noexcept {}
