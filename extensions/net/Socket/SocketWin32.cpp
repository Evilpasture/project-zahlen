// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Socket/Socket.hpp>

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 // WSAPoll is available from Windows Vista onward.
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <mutex>
#include <string>

namespace ZHLN::Net {
namespace {

std::mutex WinsockMutex;
size_t     WinsockReferences = 0;

[[nodiscard]] auto Failure(
    const SocketError error,
    const int nativeCode = 0,
    const SocketErrorDomain domain = SocketErrorDomain::Winsock
) -> SocketFailure {
    return SocketFailure {.error = error, .domain = domain, .nativeCode = nativeCode};
}

[[nodiscard]] auto AcquireWinsock() noexcept -> std::expected<void, SocketFailure> {
    std::lock_guard lock(WinsockMutex);
    if (WinsockReferences == 0) {
        WSADATA data {};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            return std::unexpected(Failure(SocketError::RuntimeInitializationFailed, result));
        }
    }
    ++WinsockReferences;
    return {};
}

void ReleaseWinsock() noexcept {
    std::lock_guard lock(WinsockMutex);
    if (WinsockReferences > 0) {
        --WinsockReferences;
        if (WinsockReferences == 0) {
            WSACleanup();
        }
    }
}

struct WinsockScope {
    ~WinsockScope() {
        ReleaseWinsock();
    }
};

[[nodiscard]] auto NativeHandle(const std::uintptr_t nativeHandle) noexcept -> SOCKET {
    return static_cast<SOCKET>(nativeHandle);
}

[[nodiscard]] auto ToNativeEndpoint(const SocketEndpoint& endpoint, sockaddr_storage& storage, int& length) noexcept
    -> bool {
    storage = {};
    switch (endpoint.family) {
        case SocketAddressFamily::IPv4: {
            auto* address       = reinterpret_cast<sockaddr_in*>(&storage);
            address->sin_family = AF_INET;
            address->sin_port   = htons(endpoint.port);
            std::memcpy(&address->sin_addr, endpoint.address.data(), sizeof(address->sin_addr));
            length = static_cast<int>(sizeof(sockaddr_in));
            return true;
        }
        case SocketAddressFamily::IPv6: {
            auto* address        = reinterpret_cast<sockaddr_in6*>(&storage);
            address->sin6_family = AF_INET6;
            address->sin6_port   = htons(endpoint.port);
            address->sin6_scope_id = endpoint.scopeId;
            std::memcpy(&address->sin6_addr, endpoint.address.data(), sizeof(address->sin6_addr));
            length = static_cast<int>(sizeof(sockaddr_in6));
            return true;
        }
        case SocketAddressFamily::Any:
            return false;
    }
    return false;
}

[[nodiscard]] auto FromNativeEndpoint(const sockaddr* address, const std::size_t length) noexcept
    -> std::expected<SocketEndpoint, SocketFailure> {
    if (address == nullptr) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }

    if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
        const auto* native = reinterpret_cast<const sockaddr_in*>(address);
        SocketEndpoint endpoint {.family = SocketAddressFamily::IPv4, .address = {}, .port = ntohs(native->sin_port)};
        std::memcpy(endpoint.address.data(), &native->sin_addr, sizeof(native->sin_addr));
        return endpoint;
    }

    if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        const auto* native = reinterpret_cast<const sockaddr_in6*>(address);
        SocketEndpoint endpoint {
            .family  = SocketAddressFamily::IPv6,
            .address = {},
            .port    = ntohs(native->sin6_port),
            .scopeId = native->sin6_scope_id
        };
        std::memcpy(endpoint.address.data(), &native->sin6_addr, sizeof(native->sin6_addr));
        return endpoint;
    }

    return std::unexpected(Failure(SocketError::InvalidEndpoint));
}

[[nodiscard]] auto ToPollTimeout(const std::chrono::milliseconds timeout) noexcept -> int {
    const auto count = timeout.count();
    if (count <= 0) {
        return 0;
    }
    if (count >= INT_MAX) {
        return INT_MAX;
    }
    return static_cast<int>(count);
}

} // namespace

class SocketAccess {
  public:
    [[nodiscard]] static auto Adopt(
        const SOCKET nativeHandle,
        const SocketType type,
        const SocketAddressFamily family,
        const bool runtimeLease
    ) noexcept -> Socket {
        return Socket(static_cast<std::uintptr_t>(nativeHandle), type, family, runtimeLease);
    }

    [[nodiscard]] static auto Accept(Socket& listener) noexcept -> std::expected<Socket, SocketFailure> {
        if (!listener._open) {
            return std::unexpected(Failure(SocketError::InvalidSocket));
        }
        if (listener._type != SocketType::Stream) {
            return std::unexpected(Failure(SocketError::InvalidArgument));
        }

        sockaddr_storage peer {};
        int              peerLength = static_cast<int>(sizeof(peer));
        const SOCKET     accepted = ::accept(
            NativeHandle(listener._nativeHandle), reinterpret_cast<sockaddr*>(&peer), &peerLength
        );
        if (accepted == INVALID_SOCKET) {
            return std::unexpected(Failure(SocketError::AcceptFailed, WSAGetLastError()));
        }

        auto runtime = AcquireWinsock();
        if (!runtime) {
            closesocket(accepted);
            return std::unexpected(runtime.error());
        }
        return Adopt(accepted, SocketType::Stream, listener._family, true);
    }
};

auto AcceptSocket(Socket& listener) noexcept -> std::expected<Socket, SocketFailure> {
    return SocketAccess::Accept(listener);
}

Socket::~Socket() noexcept {
    Close();
}

Socket::Socket(Socket&& other) noexcept:
    _nativeHandle(other._nativeHandle), _type(other._type), _family(other._family), _open(other._open),
    _runtimeLease(other._runtimeLease) {
    other._nativeHandle = 0;
    other._family       = SocketAddressFamily::Any;
    other._open         = false;
    other._runtimeLease = false;
}

auto Socket::operator=(Socket&& other) noexcept -> Socket& {
    if (this != &other) {
        Close();
        _nativeHandle       = other._nativeHandle;
        _type               = other._type;
        _family             = other._family;
        _open               = other._open;
        _runtimeLease       = other._runtimeLease;
        other._nativeHandle = 0;
        other._family       = SocketAddressFamily::Any;
        other._open         = false;
        other._runtimeLease = false;
    }
    return *this;
}

void Socket::Close() noexcept {
    if (_open) {
        closesocket(NativeHandle(_nativeHandle));
        _nativeHandle = 0;
        _family       = SocketAddressFamily::Any;
        _open         = false;
        if (_runtimeLease) {
            ReleaseWinsock();
            _runtimeLease = false;
        }
    }
}

auto Socket::SetNonBlocking(const bool enabled) noexcept -> std::expected<void, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }

    u_long mode = enabled ? 1UL : 0UL;
    if (ioctlsocket(NativeHandle(_nativeHandle), FIONBIO, &mode) == SOCKET_ERROR) {
        return std::unexpected(Failure(SocketError::SetOptionFailed, WSAGetLastError()));
    }
    return {};
}

auto Socket::SetNoDelay(const bool enabled) noexcept -> std::expected<void, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }
    if (_type != SocketType::Stream) {
        return std::unexpected(Failure(SocketError::InvalidArgument));
    }

    const int value = enabled ? 1 : 0;
    if (setsockopt(NativeHandle(_nativeHandle), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value)))
        == SOCKET_ERROR) {
        return std::unexpected(Failure(SocketError::SetOptionFailed, WSAGetLastError()));
    }
    return {};
}

auto Socket::SetReuseAddress(const bool enabled) noexcept -> std::expected<void, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }

    const int value = enabled ? 1 : 0;
    if (setsockopt(NativeHandle(_nativeHandle), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&value), static_cast<int>(sizeof(value)))
        == SOCKET_ERROR) {
        return std::unexpected(Failure(SocketError::SetOptionFailed, WSAGetLastError()));
    }
    return {};
}

auto Socket::Bind(const SocketEndpoint& endpoint) noexcept -> std::expected<void, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }
    if (!endpoint.IsValid() || endpoint.family != _family) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }

    sockaddr_storage storage {};
    int              length = 0;
    if (!ToNativeEndpoint(endpoint, storage, length)) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }
    if (::bind(NativeHandle(_nativeHandle), reinterpret_cast<const sockaddr*>(&storage), length) == SOCKET_ERROR) {
        return std::unexpected(Failure(SocketError::BindFailed, WSAGetLastError()));
    }
    return {};
}

auto Socket::Listen(const int backlog) noexcept -> std::expected<void, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }
    if (_type != SocketType::Stream || backlog <= 0) {
        return std::unexpected(Failure(SocketError::InvalidArgument));
    }
    if (::listen(NativeHandle(_nativeHandle), backlog) == SOCKET_ERROR) {
        return std::unexpected(Failure(SocketError::ListenFailed, WSAGetLastError()));
    }
    return {};
}

auto Socket::Connect(const SocketEndpoint& endpoint, const std::chrono::milliseconds timeout) noexcept
    -> std::expected<void, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }
    if (!endpoint.IsValid() || endpoint.family != _family) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }

    auto nonblocking = SetNonBlocking(true);
    if (!nonblocking) {
        return std::unexpected(nonblocking.error());
    }

    sockaddr_storage storage {};
    int              length = 0;
    if (!ToNativeEndpoint(endpoint, storage, length)) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }

    const int result = connect(
        NativeHandle(_nativeHandle), reinterpret_cast<const sockaddr*>(&storage), length
    );
    if (result == 0) {
        return {};
    }

    const int connectError = WSAGetLastError();
    if (connectError != WSAEWOULDBLOCK && connectError != WSAEINPROGRESS && connectError != WSAEALREADY) {
        return std::unexpected(Failure(SocketError::ConnectFailed, connectError));
    }

    auto readiness = WaitFor(false, true, timeout);
    if (!readiness) {
        return std::unexpected(readiness.error());
    }
    if (!readiness->writable && !readiness->error && !readiness->closed) {
        return std::unexpected(Failure(SocketError::ConnectTimedOut));
    }

    int socketError = 0;
    int socketErrorLength = static_cast<int>(sizeof(socketError));
    if (getsockopt(
            NativeHandle(_nativeHandle), SOL_SOCKET, SO_ERROR,
            reinterpret_cast<char*>(&socketError), &socketErrorLength
        ) == SOCKET_ERROR) {
        return std::unexpected(Failure(SocketError::ConnectFailed, WSAGetLastError()));
    }
    if (socketError != 0) {
        return std::unexpected(Failure(SocketError::ConnectFailed, socketError));
    }
    return {};
}

auto Socket::WaitFor(
    const bool readable,
    const bool writable,
    const std::chrono::milliseconds timeout
) const noexcept -> std::expected<SocketReadiness, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }

    WSAPOLLFD descriptor {};
    descriptor.fd     = NativeHandle(_nativeHandle);
    descriptor.events = 0;
    if (readable) {
        descriptor.events = static_cast<SHORT>(descriptor.events | POLLRDNORM);
    }
    if (writable) {
        descriptor.events = static_cast<SHORT>(descriptor.events | POLLWRNORM);
    }

    const int result = WSAPoll(&descriptor, 1, ToPollTimeout(timeout));
    if (result == SOCKET_ERROR) {
        return std::unexpected(Failure(SocketError::WaitFailed, WSAGetLastError()));
    }

    SocketReadiness readiness {};
    readiness.readable = (descriptor.revents & POLLRDNORM) != 0;
    readiness.writable = (descriptor.revents & POLLWRNORM) != 0;
    readiness.error    = (descriptor.revents & (POLLERR | POLLNVAL)) != 0;
    readiness.closed   = (descriptor.revents & POLLHUP) != 0;
    return readiness;
}

auto Socket::Send(const std::span<const uint8_t> bytes) noexcept -> std::expected<SocketIOResult, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }
    if (bytes.empty() && _type == SocketType::Stream) {
        return SocketIOResult {};
    }

    const int transferSize = static_cast<int>(std::min(bytes.size(), static_cast<size_t>(INT_MAX)));
    const int result = send(
        NativeHandle(_nativeHandle), reinterpret_cast<const char*>(bytes.data()), transferSize, 0
    );
    if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            return SocketIOResult {.state = SocketIOState::WouldBlock};
        }
        return std::unexpected(Failure(SocketError::SendFailed, error));
    }
    return SocketIOResult {.state = SocketIOState::Complete, .bytes = static_cast<size_t>(result)};
}

auto Socket::Receive(const std::span<uint8_t> bytes) noexcept -> std::expected<SocketIOResult, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }
    if (bytes.empty()) {
        return SocketIOResult {};
    }

    const int transferSize = static_cast<int>(std::min(bytes.size(), static_cast<size_t>(INT_MAX)));
    const int result = recv(
        NativeHandle(_nativeHandle), reinterpret_cast<char*>(bytes.data()), transferSize, 0
    );
    if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            return SocketIOResult {.state = SocketIOState::WouldBlock};
        }
        return std::unexpected(Failure(SocketError::ReceiveFailed, error));
    }
    if (result == 0 && _type == SocketType::Stream) {
        return SocketIOResult {.state = SocketIOState::PeerClosed};
    }
    return SocketIOResult {.state = SocketIOState::Complete, .bytes = static_cast<size_t>(result)};
}

auto Socket::SendTo(
    const std::span<const uint8_t> bytes,
    const SocketEndpoint& endpoint
) noexcept -> std::expected<SocketIOResult, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }
    if (_type != SocketType::Datagram) {
        return std::unexpected(Failure(SocketError::InvalidArgument));
    }
    if (!endpoint.IsValid() || endpoint.family != _family) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }

    sockaddr_storage storage {};
    int              addressLength = 0;
    if (!ToNativeEndpoint(endpoint, storage, addressLength)) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }

    const int transferSize = static_cast<int>(std::min(bytes.size(), static_cast<size_t>(INT_MAX)));
    const int result = sendto(
        NativeHandle(_nativeHandle), reinterpret_cast<const char*>(bytes.data()), transferSize, 0,
        reinterpret_cast<const sockaddr*>(&storage), addressLength
    );
    if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            return SocketIOResult {.state = SocketIOState::WouldBlock};
        }
        return std::unexpected(Failure(SocketError::SendFailed, error));
    }
    return SocketIOResult {.state = SocketIOState::Complete, .bytes = static_cast<size_t>(result)};
}

auto Socket::ReceiveFrom(const std::span<uint8_t> bytes) noexcept -> std::expected<SocketDatagramResult, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }
    if (_type != SocketType::Datagram) {
        return std::unexpected(Failure(SocketError::InvalidArgument));
    }

    sockaddr_storage peerStorage {};
    int              peerLength = static_cast<int>(sizeof(peerStorage));
    const int        transferSize = static_cast<int>(std::min(bytes.size(), static_cast<size_t>(INT_MAX)));
    const int result = recvfrom(
        NativeHandle(_nativeHandle), reinterpret_cast<char*>(bytes.data()), transferSize, 0,
        reinterpret_cast<sockaddr*>(&peerStorage), &peerLength
    );
    if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            return SocketDatagramResult {.io = SocketIOResult {.state = SocketIOState::WouldBlock}};
        }
        return std::unexpected(Failure(SocketError::ReceiveFailed, error));
    }

    auto peer = FromNativeEndpoint(reinterpret_cast<const sockaddr*>(&peerStorage), static_cast<size_t>(peerLength));
    if (!peer) {
        return std::unexpected(peer.error());
    }
    return SocketDatagramResult {
        .io   = SocketIOResult {.state = SocketIOState::Complete, .bytes = static_cast<size_t>(result)},
        .peer = *peer
    };
}

auto Socket::LocalEndpoint() const noexcept -> std::expected<SocketEndpoint, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }

    sockaddr_storage storage {};
    int              length = static_cast<int>(sizeof(storage));
    if (getsockname(NativeHandle(_nativeHandle), reinterpret_cast<sockaddr*>(&storage), &length) == SOCKET_ERROR) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint, WSAGetLastError()));
    }
    return FromNativeEndpoint(reinterpret_cast<const sockaddr*>(&storage), static_cast<size_t>(length));
}

auto OpenSocket(const SocketType type, const SocketAddressFamily family) noexcept -> std::expected<Socket, SocketFailure> {
    const int nativeFamily = family == SocketAddressFamily::IPv4 ? AF_INET
                           : family == SocketAddressFamily::IPv6 ? AF_INET6
                                                                : AF_UNSPEC;
    if (nativeFamily == AF_UNSPEC) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }

    auto runtime = AcquireWinsock();
    if (!runtime) {
        return std::unexpected(runtime.error());
    }

    const int nativeType = type == SocketType::Stream ? SOCK_STREAM : SOCK_DGRAM;
    const SOCKET native = socket(nativeFamily, nativeType, 0);
    if (native == INVALID_SOCKET) {
        const int error = WSAGetLastError();
        ReleaseWinsock();
        return std::unexpected(Failure(SocketError::CreationFailed, error));
    }
    return SocketAccess::Adopt(native, type, family, true);
}

auto ResolveSocketEndpoints(
    const std::string_view host,
    const uint16_t port,
    const SocketAddressFamily family,
    const SocketType type
) noexcept -> std::expected<std::vector<SocketEndpoint>, SocketFailure> {
    auto runtime = AcquireWinsock();
    if (!runtime) {
        return std::unexpected(runtime.error());
    }
    const WinsockScope runtimeScope {};

    ADDRINFOA hints {};
    hints.ai_family   = family == SocketAddressFamily::IPv4 ? AF_INET
                      : family == SocketAddressFamily::IPv6 ? AF_INET6
                                                           : AF_UNSPEC;
    hints.ai_socktype = type == SocketType::Stream ? SOCK_STREAM : SOCK_DGRAM;
    hints.ai_protocol = type == SocketType::Stream ? IPPROTO_TCP : IPPROTO_UDP;

    const std::string hostName(host);
    const std::string service = std::to_string(port);
    PADDRINFOA        results = nullptr;
    const int         status = getaddrinfo(hostName.c_str(), service.c_str(), &hints, &results);
    if (status != 0) {
        if (results != nullptr) {
            freeaddrinfo(results);
        }
        return std::unexpected(Failure(SocketError::ResolutionFailed, status, SocketErrorDomain::Resolver));
    }

    std::vector<SocketEndpoint> endpoints;
    for (const ADDRINFOA* current = results; current != nullptr; current = current->ai_next) {
        auto endpoint = FromNativeEndpoint(current->ai_addr, current->ai_addrlen);
        if (endpoint) {
            endpoints.push_back(*endpoint);
        }
    }
    freeaddrinfo(results);

    if (endpoints.empty()) {
        return std::unexpected(Failure(SocketError::ResolutionFailed, EAI_NONAME, SocketErrorDomain::Resolver));
    }
    return endpoints;
}

} // namespace ZHLN::Net
