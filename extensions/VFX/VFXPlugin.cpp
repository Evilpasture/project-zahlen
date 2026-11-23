// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer for the VFX extension. Wraps the
// Explosion and Lightning systems so a host can start, render and stop combat
// FX through the standard plugin ABI (CreateAppLayerRaw + the EntryPointWitness
// seal). Both systems are started and stopped together; a future plugin can
// split them if independent lifetimes are needed.
//
// Configuration arrives through a ChainInNode payload (PluginConfig); an
// absent or empty chain means default configuration.

#include <VFX/Explosion.hpp>
#include <VFX/Lightning.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <new>

namespace ZHLN::VFX {

// Plugin-boundary configuration for the VFX layer. Kept out of the public
// extension headers so the extensions know nothing about the COM plugin ABI.
struct PluginConfig {
    bool enabled = true; // when false, OnBeforeRender is a no-op
};

} // namespace ZHLN::VFX

namespace {

using namespace ZHLN;

class VFXPlugin final : public COM::Implements<VFXPlugin, COM::IAppLayer> {
  public:
    explicit VFXPlugin(VFX::PluginConfig config) noexcept : _config(std::move(config)) {}

    auto OnStart(COM::IEngine* engine) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        if (const auto result = _explosion.Start(*engine); !result) {
            return result.error();
        }
        _lightning.Start(*engine);
        return {};
    }

    auto OnTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnBeforeRender(COM::IEngine* engine, float dt) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        if (!_config.enabled) {
            return {};
        }
        if (const auto result = _explosion.BeforeRender(*engine, dt); !result) {
            return result.error();
        }
        if (const auto result = _lightning.BeforeRender(*engine, dt); !result) {
            return result.error();
        }
        return {};
    }

    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    void OnStop(COM::IEngine* engine) noexcept override {
        if (engine == nullptr) {
            return;
        }
        _explosion.Stop(*engine);
        _lightning.Stop(*engine);
    }

  private:
    Explosion::System _explosion {};
    Lightning::System _lightning {};
    VFX::PluginConfig _config {};
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayerRaw(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    ZHLN::VFX::PluginConfig parsed {};
    const auto            checked = DispatchInChain<ZHLN::VFX::PluginConfig>(
        config, [&](const ZHLN::VFX::PluginConfig& value) noexcept { parsed = value; });
    if (!checked.HasValue()) {
        return checked.error;
    }
    auto* instance = new (std::nothrow) VFXPlugin(parsed);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayerRaw>::Seal() noexcept {}
