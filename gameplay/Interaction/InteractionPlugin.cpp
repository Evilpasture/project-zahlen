// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer for the Interaction gameplay subsystem.
// Wraps ZHLN::Interaction::Layer so hosts can load trigger volumes, pickups,
// containers, and usables across the standard plugin ABI without linking
// zahlen_interaction.

#include <Interaction/InteractionSystem.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <new>
#include <type_traits>

namespace ZHLN::Interaction {

struct PluginConfig {
    bool enabled = true;
};

static_assert(std::is_standard_layout_v<PluginConfig> && std::is_trivially_copyable_v<PluginConfig>);

} // namespace ZHLN::Interaction

namespace {

using namespace ZHLN;

class InteractionPlugin final : public COM::Implements<InteractionPlugin, COM::IAppLayer> {
  public:
    explicit InteractionPlugin(Interaction::PluginConfig config) noexcept
        : _config(config) {}

    auto OnStart(COM::IEngine*) noexcept -> COM::Result<void> override {
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
        if (engine != nullptr) {
            _layer.Stop(*engine);
        }
    }

  private:
    Interaction::Layer        _layer {};
    Interaction::PluginConfig _config {};
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayerRaw(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    ZHLN::Interaction::PluginConfig parsed {};
    const auto                      checked = DispatchInChain<ZHLN::Interaction::PluginConfig>(
        config, [&](const ZHLN::Interaction::PluginConfig& value) noexcept { parsed = value; });
    if (!checked.HasValue()) {
        return checked.error;
    }
    auto* instance = new (std::nothrow) InteractionPlugin(parsed);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayerRaw>::Seal() noexcept {}
