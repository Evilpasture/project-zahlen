// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <FreeCam/FreeCam.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <new>

namespace {

using namespace ZHLN;

class FreeCamPlugin final : public COM::Implements<FreeCamPlugin, COM::IAppLayer> {
  public:
    constexpr FreeCamPlugin(FreeCam::Component config, bool customConfig) noexcept
        : _config(config), _customConfig(customConfig) {}

    auto OnStart(COM::IEngine* engine) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        if (_customConfig) {
            if (auto* entities = engine->Entities()) {
                (void)FreeCam::Attach(*entities, _config);
                _appliedCustom = true;
            }
        }
        return {};
    }

    auto OnTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnBeforeRender(COM::IEngine* engine, float dt) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        if (_customConfig && !_appliedCustom) {
            if (auto* entities = engine->Entities()) {
                _appliedCustom = FreeCam::Attach(*entities, _config);
            }
        }
        FreeCam::Step(*engine, dt);
        return {};
    }

    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    void OnStop(COM::IEngine* engine) noexcept override {
        if (engine != nullptr) {
            if (auto* entities = engine->Entities()) {
                FreeCam::Detach(*entities);
            }
        }
    }

  private:
    FreeCam::Component _config {};
    bool               _customConfig  = false;
    bool               _appliedCustom = false;
};

} // namespace

namespace ZHLN::COM {

auto CreateAppLayer(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    FreeCam::Component knobs {};
    bool               hasKnobs = false;
    const auto         checked  = DispatchInChain<FreeCam::Component>(
        config, [&](const FreeCam::Component& value) noexcept {
            knobs    = value;
            hasKnobs = true;
        });
    if (!checked.HasValue()) {
        return checked.error;
    }
    auto* instance = new (std::nothrow) FreeCamPlugin(knobs, hasKnobs);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM
