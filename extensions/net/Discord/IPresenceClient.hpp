// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Binary-stable COM capability interface exposed by libzahlen_plugin_discord_rpc
// alongside COM::IAppLayer. Hosts include only this header to drive Discord
// Rich Presence at runtime without linking zahlen_discord_rpc, zahlen_socket,
// or zahlen_serialization (simdjson).

#include "PresenceTypes.hpp"
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Discord {

// Optional COM::InChain payload accepted by libzahlen_plugin_discord_rpc's
// CreateAppLayerRaw entry point.
struct PluginConfig {
    COM::StringView applicationId {};
    bool            autoStart = false;
};

struct ComRichPresence {
    COM::StringView      details {};
    COM::StringView      state {};
    COM::Option<int64_t> startTimestamp {};
    COM::StringView      largeImage {};
    COM::StringView      largeText {};
    bool                 hasLargeImage = false;
    bool                 hasLargeText  = false;
};

static_assert(std::is_standard_layout_v<PluginConfig> && std::is_trivially_copyable_v<PluginConfig>);
static_assert(std::is_standard_layout_v<ComRichPresence> && std::is_trivially_copyable_v<ComRichPresence>);

struct IPresenceClient : COM::IObject {
    [[nodiscard]] virtual COM::Result<void> Start() noexcept                                    = 0;
    virtual void                            SetActivity(const ComRichPresence& activity) noexcept = 0;
    virtual void                            ClearActivity() noexcept                            = 0;
    virtual void                            Stop() noexcept                                     = 0;
    [[nodiscard]] virtual PresenceState     State() const noexcept                              = 0;

  protected:
    ~IPresenceClient() = default;
};

static_assert(COM::AuditInterfaceABI<IPresenceClient>(), "IPresenceClient violates the COM ABI rules.");

} // namespace ZHLN::Discord
