// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Discord {

enum class RpcOpcode : uint32_t {
    Handshake = 0,
    Frame = 1,
    Close = 2,
    Ping = 3,
    Pong = 4
};

struct RpcFrame {
    RpcOpcode   opcode = RpcOpcode::Frame;
    std::string payload;
};

enum class RpcProtocolError : uint8_t {
    PayloadTooLarge = 1,
    InvalidFrame,
    UnsupportedOpcode,
    InvalidJsonPayload
};

// Discord RPC frames use two little-endian uint32 fields followed by JSON.
[[nodiscard]] auto EncodeRpcFrame(RpcOpcode opcode, std::string_view payload) noexcept
    -> std::expected<std::vector<uint8_t>, RpcProtocolError>;
[[nodiscard]] auto DecodeRpcFrame(std::span<const uint8_t> bytes) noexcept
    -> std::expected<RpcFrame, RpcProtocolError>;

struct RichPresence {
    std::string                 details;
    std::string                 state;
    std::optional<int64_t>      startTimestamp;
    std::optional<std::string>  largeImage;
    std::optional<std::string>  largeText;
};

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

[[nodiscard]] auto ValidateApplicationId(std::string_view applicationId) noexcept -> std::expected<void, PresenceError>;

// A reconnecting Discord IPC client. Start queues a TaskSystem background job
// and fails rather than running it inline when scheduler capacity is exhausted.
// SetActivity and ClearActivity only replace the pending value and never do
// socket I/O on the caller's thread. Keep the client within the TaskSystem
// lifetime. Application IDs and presence are supplied by the embedding
// application rather than compiled into this library.
class DiscordPresenceClient {
  public:
    explicit DiscordPresenceClient(std::string applicationId);
    ~DiscordPresenceClient() noexcept;

    DiscordPresenceClient(const DiscordPresenceClient&) = delete;
    auto operator=(const DiscordPresenceClient&) -> DiscordPresenceClient& = delete;
    DiscordPresenceClient(DiscordPresenceClient&&) = delete;
    auto operator=(DiscordPresenceClient&&) -> DiscordPresenceClient& = delete;

    [[nodiscard]] auto Start() noexcept -> std::expected<void, PresenceError>;
    void SetActivity(RichPresence activity) noexcept;
    void ClearActivity() noexcept;
    // Requests cancellation without waiting on the caller's thread. Destruction
    // waits for the background job to finish before releasing its state.
    void Stop() noexcept;

    [[nodiscard]] auto State() const noexcept -> PresenceState;

  private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN::Discord
