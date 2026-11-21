// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer for the Terrain extension. Wraps
// ZHLN::Terrain::System so a host can start, render and stop tile-based terrain
// through the standard plugin ABI (CreateAppLayer + the EntryPointWitness seal).
//
// Configuration arrives through a ChainInNode payload (PluginConfig); an
// absent or empty chain means default configuration. Terrain geometry itself is
// created by the host (System::Create); the plugin layer owns the system's
// lifetime.

#include <Terrain/Terrain.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <new>

namespace ZHLN::Terrain {

// Plugin-boundary configuration for the terrain layer. Kept out of the public
// extension headers so the extension itself knows nothing about the COM plugin
// ABI.
struct PluginConfig {
    bool enabled = true; // when false, OnBeforeRender is a no-op
};

} // namespace ZHLN::Terrain

namespace {

using namespace ZHLN;

class TerrainPlugin final : public COM::Implements<TerrainPlugin, COM::IAppLayer> {
  public:
    explicit TerrainPlugin(Terrain::PluginConfig config) noexcept
        : _config(std::move(config)) {}

    auto OnStart(COM::IEngine* engine) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        _terrain.Start(*engine);
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
        const auto result = _terrain.BeforeRender(*engine, dt);
        if (!result) {
            return result.error();
        }
        return {};
    }

    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    void OnStop(COM::IEngine* engine) noexcept override {
        if (engine != nullptr) {
            _terrain.Stop(*engine);
        }
    }

  private:
    Terrain::System       _terrain {};
    Terrain::PluginConfig _config {};
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayer(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    ZHLN::Terrain::PluginConfig parsed {};
    const auto                checked = DispatchInChain<ZHLN::Terrain::PluginConfig>(
        config, [&](const ZHLN::Terrain::PluginConfig& value) noexcept { parsed = value; });
    if (!checked.HasValue()) {
        return checked.error;
    }
    auto* instance = new (std::nothrow) TerrainPlugin(parsed);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayer>::Seal() noexcept {}
