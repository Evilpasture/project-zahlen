// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN::Net {

enum class SocketAddressFamily : uint8_t {
    Any,
    IPv4,
    IPv6
};

enum class SocketType : uint8_t {
    Stream,
    Datagram
};

enum class SocketError : uint8_t {
    RuntimeInitializationFailed = 1,
    InvalidSocket,
    InvalidArgument,
    InvalidEndpoint,
    ResolutionFailed,
    CreationFailed,
    BindFailed,
    ListenFailed,
    AcceptFailed,
    ConnectFailed,
    ConnectTimedOut,
    SetOptionFailed,
    WaitFailed,
    SendFailed,
    ReceiveFailed
};

enum class SocketErrorDomain : uint8_t {
    None,
    Posix,
    Winsock,
    Resolver
};

struct SocketFailure {
    SocketError       error      = SocketError::InvalidSocket;
    SocketErrorDomain domain     = SocketErrorDomain::None;
    int               nativeCode = 0;
};

enum class SocketIOState : uint8_t {
    Complete,
    WouldBlock,
    PeerClosed
};

struct SocketIOResult {
    SocketIOState state = SocketIOState::Complete;
    size_t        bytes = 0;
};

struct SocketReadiness {
    bool readable = false;
    bool writable = false;
    bool error    = false;
    bool closed   = false;
};

struct SocketEndpoint {
    SocketAddressFamily     family  = SocketAddressFamily::Any;
    std::array<uint8_t, 16> address {}; // Network-order octets: first four for IPv4, all sixteen for IPv6.
    uint16_t                port    = 0; // Host byte order.
    uint32_t                scopeId = 0; // IPv6 interface scope; zero for IPv4/global addresses.

    [[nodiscard]] constexpr auto IsValid() const noexcept -> bool {
        return family == SocketAddressFamily::IPv4 || family == SocketAddressFamily::IPv6;
    }

    [[nodiscard]] static constexpr auto IPv4Any(const uint16_t port) noexcept -> SocketEndpoint {
        return SocketEndpoint {.family = SocketAddressFamily::IPv4, .address = {}, .port = port};
    }

    [[nodiscard]] static constexpr auto IPv4Loopback(const uint16_t port) noexcept -> SocketEndpoint {
        SocketEndpoint endpoint {.family = SocketAddressFamily::IPv4, .address = {}, .port = port};
        endpoint.address[0] = 127;
        endpoint.address[3] = 1;
        return endpoint;
    }

    [[nodiscard]] static constexpr auto IPv6Any(const uint16_t port) noexcept -> SocketEndpoint {
        return SocketEndpoint {.family = SocketAddressFamily::IPv6, .address = {}, .port = port};
    }

    [[nodiscard]] static constexpr auto IPv6Loopback(const uint16_t port) noexcept -> SocketEndpoint {
        SocketEndpoint endpoint {.family = SocketAddressFamily::IPv6, .address = {}, .port = port};
        endpoint.address[15] = 1;
        return endpoint;
    }

    [[nodiscard]] constexpr auto operator==(const SocketEndpoint&) const noexcept -> bool = default;
};

struct SocketDatagramResult {
    SocketIOResult  io {};
    SocketEndpoint peer {};
};

// Move-only owner for one native TCP or UDP socket. The integer representation
// is private and deliberately has no public native-handle accessor: callers
// operate on the same value API on POSIX and Win32.
class Socket {
  public:
    Socket() noexcept = default;
    ~Socket() noexcept;

    Socket(const Socket&)            = delete;
    auto operator=(const Socket&)    -> Socket& = delete;
    Socket(Socket&& other) noexcept;
    auto operator=(Socket&& other) noexcept -> Socket&;

    [[nodiscard]] auto IsOpen() const noexcept -> bool {
        return _open;
    }

    void Close() noexcept;

    [[nodiscard]] auto SetNonBlocking(bool enabled) noexcept -> std::expected<void, SocketFailure>;
    [[nodiscard]] auto SetNoDelay(bool enabled) noexcept -> std::expected<void, SocketFailure>;
    [[nodiscard]] auto SetReuseAddress(bool enabled) noexcept -> std::expected<void, SocketFailure>;

    [[nodiscard]] auto Bind(const SocketEndpoint& endpoint) noexcept -> std::expected<void, SocketFailure>;
    [[nodiscard]] auto Listen(int backlog) noexcept -> std::expected<void, SocketFailure>;

    // Connect always uses nonblocking mode internally and waits up to timeout;
    // the socket remains nonblocking after it succeeds. For a datagram socket,
    // this selects its default peer and normally completes immediately.
    [[nodiscard]] auto Connect(const SocketEndpoint& endpoint, std::chrono::milliseconds timeout) noexcept
        -> std::expected<void, SocketFailure>;

    // A timeout is represented by a readiness value with all flags false.
    [[nodiscard]] auto WaitFor(bool readable, bool writable, std::chrono::milliseconds timeout) const noexcept
        -> std::expected<SocketReadiness, SocketFailure>;

    // Stream sends may be partial; stream reads distinguish peer close from a
    // nonblocking socket with no data. For datagrams, Send/Receive operate on a
    // connected peer, and a zero-byte datagram is a completed read.
    [[nodiscard]] auto Send(std::span<const uint8_t> bytes) noexcept -> std::expected<SocketIOResult, SocketFailure>;
    [[nodiscard]] auto Receive(std::span<uint8_t> bytes) noexcept -> std::expected<SocketIOResult, SocketFailure>;

    [[nodiscard]] auto SendTo(std::span<const uint8_t> bytes, const SocketEndpoint& endpoint) noexcept
        -> std::expected<SocketIOResult, SocketFailure>;
    [[nodiscard]] auto ReceiveFrom(std::span<uint8_t> bytes) noexcept -> std::expected<SocketDatagramResult, SocketFailure>;

    [[nodiscard]] auto LocalEndpoint() const noexcept -> std::expected<SocketEndpoint, SocketFailure>;

  private:
    friend class SocketAccess;

    explicit Socket(
        std::uintptr_t nativeHandle,
        SocketType type,
        SocketAddressFamily family,
        bool runtimeLease
    ) noexcept:
        _nativeHandle(nativeHandle), _type(type), _family(family), _open(true), _runtimeLease(runtimeLease) {
    }

    std::uintptr_t      _nativeHandle = 0;
    SocketType          _type         = SocketType::Stream;
    SocketAddressFamily _family       = SocketAddressFamily::Any;
    bool                _open         = false;
    bool                _runtimeLease = false;
};

[[nodiscard]] auto OpenSocket(SocketType type, SocketAddressFamily family) noexcept -> std::expected<Socket, SocketFailure>;
[[nodiscard]] auto AcceptSocket(Socket& listener) noexcept -> std::expected<Socket, SocketFailure>;
[[nodiscard]] auto ResolveSocketEndpoints(
    std::string_view host,
    uint16_t port,
    SocketAddressFamily family = SocketAddressFamily::Any,
    SocketType type = SocketType::Stream
) noexcept -> std::expected<std::vector<SocketEndpoint>, SocketFailure>;

} // namespace ZHLN::Net
