// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer + VFX::IVfxLayer plugin. Wraps the
// Explosion and Lightning systems so a host can start, spawn, render, and stop
// combat FX across the standard plugin ABI (CreateAppLayerRaw +
// EntryPointWitness::Seal) without linking the static zahlen_vfx library.
//
// Configuration arrives through a ChainInNode payload (VFX::PluginConfig); an
// absent or empty chain means default configuration.

#include <VFX/Explosion.hpp>
#include <VFX/IVfxLayer.hpp>
#include <VFX/Lightning.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <new>

namespace {

using namespace ZHLN;

class VFXPlugin final : public COM::Implements<VFXPlugin, COM::IAppLayer, VFX::IVfxLayer> {
  public:
    explicit VFXPlugin(VFX::PluginConfig config) noexcept : _config(config) {}

    // --- COM::IAppLayer ------------------------------------------------------

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

    // --- VFX::IVfxLayer ------------------------------------------------------

    auto SpawnExplosion(const Explosion::SpawnInfo& info) noexcept -> COM::Result<Entity> override {
        const auto spawned = _explosion.Spawn(info);
        if (!spawned) {
            return spawned.error();
        }
        return *spawned;
    }

    void RemoveExplosion(Entity entity) noexcept override {
        _explosion.Remove(entity);
    }

    void ClearExplosions() noexcept override {
        _explosion.Clear();
    }

    auto ExplosionCount() const noexcept -> uint64_t override {
        return static_cast<uint64_t>(_explosion.Size());
    }

    auto CraterCount() const noexcept -> uint64_t override {
        return static_cast<uint64_t>(_explosion.CraterCount());
    }

    auto SpawnLightning(
        FVec3                        cloud,
        FVec3                        ground,
        FVec3                        camera,
        const Lightning::ConfigInfo& config
    ) noexcept -> COM::Result<Entity> override {
        const auto spawned = _lightning.Spawn(cloud, ground, camera, config);
        if (!spawned) {
            return spawned.error();
        }
        return *spawned;
    }

    void RemoveLightning(Entity entity) noexcept override {
        _lightning.Remove(entity);
    }

    void ClearLightning() noexcept override {
        _lightning.Clear();
    }

    auto LightningCount() const noexcept -> uint64_t override {
        return static_cast<uint64_t>(_lightning.Size());
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
    const auto              checked = DispatchInChain<ZHLN::VFX::PluginConfig>(
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
