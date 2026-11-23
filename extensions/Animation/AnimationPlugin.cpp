// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer for the Animation extension. Wraps
// ZHLN::Animation::Layer so a host can start, tick and stop animation through
// the standard plugin ABI (CreateAppLayerRaw + the EntryPointWitness seal).
//
// Configuration arrives through a ChainInNode payload (PluginConfig); an
// absent or empty chain means default configuration. The layer ticks the
// animation context every frame and releases it on stop.

#include <Animation/AnimationSystem.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <new>

namespace ZHLN::Animation {

// Plugin-boundary configuration for the animation layer. Kept out of the
// public extension headers so the extension itself knows nothing about the
// COM plugin ABI; it is only ever parsed by this translation unit.
struct PluginConfig {
    bool enabled    = true;   // when false, OnTick is a no-op
    bool clearOnStop = true;  // when false, OnStop leaves the context intact
};

} // namespace ZHLN::Animation

namespace {

using namespace ZHLN;

class AnimationPlugin final : public COM::Implements<AnimationPlugin, COM::IAppLayer> {
  public:
    explicit AnimationPlugin(Animation::PluginConfig config) noexcept
        : _config(std::move(config)) {}

    auto OnStart(COM::IEngine*) noexcept -> COM::Result<void> override {
        // Animation::Layer is default-constructed and starts lazily on first
        // tick; there is no engine state to acquire here.
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
        if (_config.clearOnStop) {
            _layer.Stop(*engine);
        }
    }

  private:
    Animation::Layer        _layer {};
    Animation::PluginConfig _config {};
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayerRaw(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    Animation::PluginConfig parsed {};
    const auto              checked = DispatchInChain<Animation::PluginConfig>(
        config, [&](const Animation::PluginConfig& value) noexcept { parsed = value; });
    if (!checked.HasValue()) {
        return checked.error;
    }
    auto* instance = new (std::nothrow) AnimationPlugin(parsed);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayerRaw>::Seal() noexcept {}
