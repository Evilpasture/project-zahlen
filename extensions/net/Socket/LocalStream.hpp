// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Socket/Socket.hpp>
#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace ZHLN::Net {

// Move-only stream over a local IPC endpoint. POSIX uses AF_UNIX stream
// sockets; Win32 uses named pipes. Endpoint strings are POSIX socket paths or
// Win32 pipe names, but no OS handle or socket type crosses this interface.
class LocalStream {
  public:
    LocalStream() noexcept = default;
    ~LocalStream() noexcept;

    LocalStream(const LocalStream&)            = delete;
    auto operator=(const LocalStream&)          -> LocalStream& = delete;
    LocalStream(LocalStream&& other) noexcept;
    auto operator=(LocalStream&& other) noexcept -> LocalStream&;

    void Close() noexcept;

    // A timeout returns WouldBlock. A stream operation may transfer fewer
    // bytes than requested; callers retain the remainder and retry.
    [[nodiscard]] auto Send(std::span<const uint8_t> bytes, std::chrono::milliseconds timeout) noexcept
        -> std::expected<SocketIOResult, SocketFailure>;
    [[nodiscard]] auto Receive(std::span<uint8_t> bytes, std::chrono::milliseconds timeout) noexcept
        -> std::expected<SocketIOResult, SocketFailure>;

  private:
    friend class LocalStreamAccess;

    enum class State : uint8_t {
        Closed,
        Open
    };

    explicit LocalStream(std::uintptr_t nativeHandle) noexcept: _nativeHandle(nativeHandle), _state(State::Open) {
    }

    std::uintptr_t _nativeHandle = 0;
    State          _state = State::Closed;
};

// Connect to one explicit local IPC endpoint. Discord path discovery belongs
// to the Discord RPC layer; this primitive also works for other local clients.
[[nodiscard]] auto ConnectLocalStream(std::string_view endpoint, std::chrono::milliseconds timeout) noexcept
    -> std::expected<LocalStream, SocketFailure>;

} // namespace ZHLN::Net
