// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Discord/EngineIntegration.hpp>

#include <utility>

namespace ZHLN::Discord {

auto StartPresence(Engine& engine, std::string applicationId) noexcept -> std::expected<void, PresenceError> {
    auto validApplicationId = ValidateApplicationId(applicationId);
    if (!validApplicationId) {
        return std::unexpected(validApplicationId.error());
    }
    if (auto client = engine.Service<DiscordPresenceClient>();
        client && client->State() != PresenceState::Stopped) {
        return std::unexpected(PresenceError::AlreadyStarted);
    }

    auto& client = engine.EmplaceService<DiscordPresenceClient>(std::move(applicationId));
    return client.Start();
}

auto SetPresence(Engine& engine, RichPresence activity) noexcept -> std::expected<void, PresenceError> {
    auto client = engine.Service<DiscordPresenceClient>();
    if (!client) {
        return std::unexpected(PresenceError::NotStarted);
    }
    client->SetActivity(std::move(activity));
    return {};
}

auto ClearPresence(Engine& engine) noexcept -> std::expected<void, PresenceError> {
    auto client = engine.Service<DiscordPresenceClient>();
    if (!client) {
        return std::unexpected(PresenceError::NotStarted);
    }
    client->ClearActivity();
    return {};
}

void StopPresence(Engine& engine) noexcept {
    if (auto client = engine.Service<DiscordPresenceClient>(); client) {
        client->Stop();
    }
}

auto GetPresenceState(const Engine& engine) noexcept -> PresenceState {
    if (auto client = engine.Service<DiscordPresenceClient>(); client) {
        return client->State();
    }
    return PresenceState::Stopped;
}

} // namespace ZHLN::Discord
