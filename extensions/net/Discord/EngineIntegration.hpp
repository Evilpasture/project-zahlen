// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Discord/RichPresence.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <expected>
#include <string>

namespace ZHLN::Discord {

// Engine-owned lifecycle helpers. The client is stored in Engine::Services(),
// not in ECS; Engine destruction joins its worker after graph teardown. A
// signaled client can be restarted after its worker has finished.
[[nodiscard]] auto StartPresence(Engine& engine, std::string applicationId) noexcept
    -> std::expected<void, PresenceError>;
[[nodiscard]] auto SetPresence(Engine& engine, RichPresence activity) noexcept
    -> std::expected<void, PresenceError>;
[[nodiscard]] auto ClearPresence(Engine& engine) noexcept -> std::expected<void, PresenceError>;
void StopPresence(Engine& engine) noexcept;
[[nodiscard]] auto GetPresenceState(const Engine& engine) noexcept -> PresenceState;

} // namespace ZHLN::Discord
