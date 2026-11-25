// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace ZHLN::Discord {

enum class PresenceState : uint8_t {
    Stopped,
    Connecting,
    Disconnected,
    Connected,
    Stopping,
    RpcRejected // Discord replied with an RPC ERROR, including an authorization rejection.
};

enum class PresenceError : uint8_t {
    InvalidApplicationId = 1,
    AlreadyStarted,
    NotStarted,
    TaskSystemUnavailable,
    TaskCapacityExhausted
};

} // namespace ZHLN::Discord
