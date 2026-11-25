// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer + Discord::IPresenceClient plugin. Seals
// Discord IPC sockets and JSON RPC framing behind a runtime .so boundary.

#include <Discord/IPresenceClient.hpp>
#include <Discord/RichPresence.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <new>
#include <string>
#include <utility>

namespace {

using namespace ZHLN;

class DiscordPlugin final : public COM::Implements<DiscordPlugin, COM::IAppLayer, Discord::IPresenceClient> {
  public:
    DiscordPlugin(std::string applicationId, bool autoStart)
        : _client(std::move(applicationId)), _autoStart(autoStart) {}

    // --- COM::IAppLayer ------------------------------------------------------

    auto OnStart(COM::IEngine*) noexcept -> COM::Result<void> override {
        if (_autoStart) {
            return Start();
        }
        return {};
    }

    auto OnTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnBeforeRender(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    void OnStop(COM::IEngine*) noexcept override {
        _client.Stop();
    }

    // --- Discord::IPresenceClient --------------------------------------------

    auto Start() noexcept -> COM::Result<void> override {
        const auto started = _client.Start();
        if (!started) {
            return ErrorCode(started.error());
        }
        return {};
    }

    void SetActivity(const Discord::ComRichPresence& activity) noexcept override {
        Discord::RichPresence native {};
        if (activity.details.data != nullptr && activity.details.size > 0) {
            native.details.assign(activity.details.data, activity.details.size);
        }
        if (activity.state.data != nullptr && activity.state.size > 0) {
            native.state.assign(activity.state.data, activity.state.size);
        }
        if (activity.startTimestamp.HasValue()) {
            native.startTimestamp = activity.startTimestamp.value;
        }
        if (activity.hasLargeImage && activity.largeImage.data != nullptr) {
            native.largeImage = std::string(activity.largeImage.data, activity.largeImage.size);
        }
        if (activity.hasLargeText && activity.largeText.data != nullptr) {
            native.largeText = std::string(activity.largeText.data, activity.largeText.size);
        }
        _client.SetActivity(std::move(native));
    }

    void ClearActivity() noexcept override {
        _client.ClearActivity();
    }

    void Stop() noexcept override {
        _client.Stop();
    }

    auto State() const noexcept -> Discord::PresenceState override {
        return _client.State();
    }

  private:
    Discord::PresenceClient _client;
    bool                    _autoStart = false;
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayerRaw(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    ZHLN::Discord::PluginConfig parsed {};
    const auto                  checked = DispatchInChain<ZHLN::Discord::PluginConfig>(
        config, [&](const ZHLN::Discord::PluginConfig& value) noexcept { parsed = value; });
    if (!checked.HasValue()) {
        return checked.error;
    }
    if (parsed.applicationId.size > 0 && parsed.applicationId.data == nullptr) {
        return Error::NullArgument;
    }
    std::string appId;
    if (parsed.applicationId.data != nullptr && parsed.applicationId.size > 0) {
        appId.assign(parsed.applicationId.data, parsed.applicationId.size);
    }
    auto* instance = new (std::nothrow) DiscordPlugin(std::move(appId), parsed.autoStart);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayerRaw>::Seal() noexcept {}
