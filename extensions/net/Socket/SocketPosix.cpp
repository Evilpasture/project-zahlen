// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Socket/Socket.hpp>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace ZHLN::Net {
namespace {

[[nodiscard]] auto Failure(const SocketError error, const int nativeCode = 0, const SocketErrorDomain domain = SocketErrorDomain::Posix) -> SocketFailure {
    return SocketFailure {.error = error, .domain = domain, .nativeCode = nativeCode};
}

[[nodiscard]] auto NativeHandle(const std::uintptr_t nativeHandle) noexcept -> int {
    return static_cast<int>(nativeHandle);
}

[[nodiscard]] auto ToNativeEndpoint(const SocketEndpoint& endpoint, sockaddr_storage& storage, socklen_t& length) noexcept -> bool {
    storage = {};
    switch (endpoint.family) {
        case SocketAddressFamily::IPv4: {
            auto* address       = reinterpret_cast<sockaddr_in*>(&storage);
            address->sin_family = AF_INET;
            address->sin_port   = htons(endpoint.port);
            std::memcpy(&address->sin_addr, endpoint.address.data(), sizeof(address->sin_addr));
            length = sizeof(sockaddr_in);
            return true;
        }
        case SocketAddressFamily::IPv6: {
            auto* address          = reinterpret_cast<sockaddr_in6*>(&storage);
            address->sin6_family   = AF_INET6;
            address->sin6_port     = htons(endpoint.port);
            address->sin6_scope_id = endpoint.scopeId;
            std::memcpy(&address->sin6_addr, endpoint.address.data(), sizeof(address->sin6_addr));
            length = sizeof(sockaddr_in6);
            return true;
        }
        case SocketAddressFamily::Any:
            return false;
    }
    return false;
}

[[nodiscard]] auto FromNativeEndpoint(const sockaddr* address, const socklen_t length) noexcept -> std::expected<SocketEndpoint, SocketFailure> {
    if (address == nullptr) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }

    if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
        const auto*    native = reinterpret_cast<const sockaddr_in*>(address);
        SocketEndpoint endpoint {.family = SocketAddressFamily::IPv4, .address = {}, .port = ntohs(native->sin_port)};
        std::memcpy(endpoint.address.data(), &native->sin_addr, sizeof(native->sin_addr));
        return endpoint;
    }

    if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        const auto*    native = reinterpret_cast<const sockaddr_in6*>(address);
        SocketEndpoint endpoint {.family = SocketAddressFamily::IPv6, .address = {}, .port = ntohs(native->sin6_port), .scopeId = native->sin6_scope_id};
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

[[nodiscard]] auto MaxTransferSize() noexcept -> size_t {
    return static_cast<size_t>(std::numeric_limits<ssize_t>::max());
}

} // namespace

class SocketAccess {
  public:
    [[nodiscard]] static auto Adopt(const int descriptor, const SocketType type, const SocketAddressFamily family) noexcept -> Socket {
        return Socket(static_cast<std::uintptr_t>(descriptor), type, family, false);
    }

    [[nodiscard]] static auto Accept(Socket& listener) noexcept -> std::expected<Socket, SocketFailure> {
        if (!listener._open) {
            return std::unexpected(Failure(SocketError::InvalidSocket));
        }
        if (listener._type != SocketType::Stream) {
            return std::unexpected(Failure(SocketError::InvalidArgument));
        }

        sockaddr_storage peer {};
        socklen_t        peerLength = sizeof(peer);
        int              descriptor = 0;
        do {
            descriptor = accept(NativeHandle(listener._nativeHandle), reinterpret_cast<sockaddr*>(&peer), &peerLength);
        } while (descriptor < 0 && errno == EINTR);

        if (descriptor < 0) {
            return std::unexpected(Failure(SocketError::AcceptFailed, errno));
        }

        const int descriptorFlags = fcntl(descriptor, F_GETFD, 0);
        if (descriptorFlags >= 0) {
            fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC);
        }
#ifdef SO_NOSIGPIPE
        const int noSigPipe = 1;
        if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe)) < 0) {
            const int error = errno;
            ::close(descriptor);
            return std::unexpected(Failure(SocketError::SetOptionFailed, error));
        }
#endif
        return Adopt(descriptor, SocketType::Stream, listener._family);
    }
};

auto AcceptSocket(Socket& listener) noexcept -> std::expected<Socket, SocketFailure> {
    return SocketAccess::Accept(listener);
}

Socket::~Socket() noexcept {
    Close();
}

Socket::Socket(Socket&& other) noexcept:
    _nativeHandle(other._nativeHandle), _type(other._type), _family(other._family), _open(other._open), _runtimeLease(other._runtimeLease) {
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
        ::close(NativeHandle(_nativeHandle));
        _nativeHandle = 0;
        _family       = SocketAddressFamily::Any;
        _open         = false;
        _runtimeLease = false;
    }
}

auto Socket::SetNonBlocking(const bool enabled) noexcept -> std::expected<void, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }

    const int descriptor = NativeHandle(_nativeHandle);
    const int flags      = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0) {
        return std::unexpected(Failure(SocketError::SetOptionFailed, errno));
    }

    const int updated = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(descriptor, F_SETFL, updated) < 0) {
        return std::unexpected(Failure(SocketError::SetOptionFailed, errno));
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
    if (setsockopt(NativeHandle(_nativeHandle), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) < 0) {
        return std::unexpected(Failure(SocketError::SetOptionFailed, errno));
    }
    return {};
}

auto Socket::SetReuseAddress(const bool enabled) noexcept -> std::expected<void, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }

    const int value = enabled ? 1 : 0;
    if (setsockopt(NativeHandle(_nativeHandle), SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) < 0) {
        return std::unexpected(Failure(SocketError::SetOptionFailed, errno));
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
    socklen_t        length = 0;
    if (!ToNativeEndpoint(endpoint, storage, length)) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }
    if (::bind(NativeHandle(_nativeHandle), reinterpret_cast<const sockaddr*>(&storage), length) < 0) {
        return std::unexpected(Failure(SocketError::BindFailed, errno));
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
    if (::listen(NativeHandle(_nativeHandle), backlog) < 0) {
        return std::unexpected(Failure(SocketError::ListenFailed, errno));
    }
    return {};
}

auto Socket::Connect(const SocketEndpoint& endpoint, const std::chrono::milliseconds timeout) noexcept -> std::expected<void, SocketFailure> {
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
    socklen_t        length = 0;
    if (!ToNativeEndpoint(endpoint, storage, length)) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }

    const int result = ::connect(NativeHandle(_nativeHandle), reinterpret_cast<const sockaddr*>(&storage), length);
    if (result == 0) {
        return {};
    }
    if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EALREADY && errno != EINTR) {
        return std::unexpected(Failure(SocketError::ConnectFailed, errno));
    }

    auto readiness = WaitFor(false, true, timeout);
    if (!readiness) {
        return std::unexpected(readiness.error());
    }
    if (!readiness->writable && !readiness->error && !readiness->closed) {
        return std::unexpected(Failure(SocketError::ConnectTimedOut));
    }

    int       socketError       = 0;
    socklen_t socketErrorLength = sizeof(socketError);
    if (getsockopt(NativeHandle(_nativeHandle), SOL_SOCKET, SO_ERROR, &socketError, &socketErrorLength) < 0) {
        return std::unexpected(Failure(SocketError::ConnectFailed, errno));
    }
    if (socketError != 0) {
        return std::unexpected(Failure(SocketError::ConnectFailed, socketError));
    }
    return {};
}

auto Socket::WaitFor(const bool readable, const bool writable, const std::chrono::milliseconds timeout) const noexcept
    -> std::expected<SocketReadiness, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }

    pollfd descriptor {};
    descriptor.fd     = NativeHandle(_nativeHandle);
    descriptor.events = 0;
    if (readable) {
        descriptor.events = static_cast<short>(descriptor.events | POLLIN);
    }
    if (writable) {
        descriptor.events = static_cast<short>(descriptor.events | POLLOUT);
    }

    const auto waitDuration = std::chrono::milliseconds(ToPollTimeout(timeout));
    const auto deadline     = std::chrono::steady_clock::now() + waitDuration;
    int        result       = 0;
    for (;;) {
        descriptor.revents   = 0;
        const auto now       = std::chrono::steady_clock::now();
        const auto remaining = (now >= deadline) ? std::chrono::milliseconds::zero() : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        result               = poll(&descriptor, 1, ToPollTimeout(remaining));
        if (result >= 0 || errno != EINTR) {
            break;
        }
    }

    if (result < 0) {
        return std::unexpected(Failure(SocketError::WaitFailed, errno));
    }

    SocketReadiness readiness {};
    readiness.readable = (descriptor.revents & (POLLIN | POLLPRI)) != 0;
    readiness.writable = (descriptor.revents & POLLOUT) != 0;
    readiness.error    = (descriptor.revents & (POLLERR | POLLNVAL)) != 0;
    readiness.closed   = (descriptor.revents & POLLHUP) != 0;
#ifdef POLLRDHUP
    readiness.closed = readiness.closed || (descriptor.revents & POLLRDHUP) != 0;
#endif
    return readiness;
}

auto Socket::Send(const std::span<const uint8_t> bytes) noexcept -> std::expected<SocketIOResult, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }
    if (bytes.empty() && _type == SocketType::Stream) {
        return SocketIOResult {};
    }

    const auto transferSize = std::min(bytes.size(), MaxTransferSize());
    ssize_t    result       = 0;
    do {
#ifdef MSG_NOSIGNAL
        result = send(NativeHandle(_nativeHandle), bytes.data(), transferSize, MSG_NOSIGNAL);
#else
        result = send(NativeHandle(_nativeHandle), bytes.data(), transferSize, 0);
#endif
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return SocketIOResult {.state = SocketIOState::WouldBlock};
        }
        return std::unexpected(Failure(SocketError::SendFailed, errno));
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

    const auto transferSize = std::min(bytes.size(), MaxTransferSize());
    ssize_t    result       = 0;
    do {
        result = recv(NativeHandle(_nativeHandle), bytes.data(), transferSize, 0);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return SocketIOResult {.state = SocketIOState::WouldBlock};
        }
        return std::unexpected(Failure(SocketError::ReceiveFailed, errno));
    }
    if (result == 0 && _type == SocketType::Stream) {
        return SocketIOResult {.state = SocketIOState::PeerClosed};
    }
    return SocketIOResult {.state = SocketIOState::Complete, .bytes = static_cast<size_t>(result)};
}

auto Socket::SendTo(const std::span<const uint8_t> bytes, const SocketEndpoint& endpoint) noexcept -> std::expected<SocketIOResult, SocketFailure> {
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
    socklen_t        length = 0;
    if (!ToNativeEndpoint(endpoint, storage, length)) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }

    const auto transferSize = std::min(bytes.size(), MaxTransferSize());
    ssize_t    result       = 0;
    do {
#ifdef MSG_NOSIGNAL
        result = sendto(NativeHandle(_nativeHandle), bytes.data(), transferSize, MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&storage), length);
#else
        result = sendto(NativeHandle(_nativeHandle), bytes.data(), transferSize, 0, reinterpret_cast<const sockaddr*>(&storage), length);
#endif
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return SocketIOResult {.state = SocketIOState::WouldBlock};
        }
        return std::unexpected(Failure(SocketError::SendFailed, errno));
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
    socklen_t        peerLength   = sizeof(peerStorage);
    const auto       transferSize = std::min(bytes.size(), MaxTransferSize());
    ssize_t          result       = 0;
    do {
        result = recvfrom(NativeHandle(_nativeHandle), bytes.data(), transferSize, 0, reinterpret_cast<sockaddr*>(&peerStorage), &peerLength);
    } while (result < 0 && errno == EINTR);

    if (result < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return SocketDatagramResult {.io = SocketIOResult {.state = SocketIOState::WouldBlock}};
        }
        return std::unexpected(Failure(SocketError::ReceiveFailed, errno));
    }

    auto peer = FromNativeEndpoint(reinterpret_cast<const sockaddr*>(&peerStorage), peerLength);
    if (!peer) {
        return std::unexpected(peer.error());
    }
    return SocketDatagramResult {.io = SocketIOResult {.state = SocketIOState::Complete, .bytes = static_cast<size_t>(result)}, .peer = *peer};
}

auto Socket::LocalEndpoint() const noexcept -> std::expected<SocketEndpoint, SocketFailure> {
    if (!_open) {
        return std::unexpected(Failure(SocketError::InvalidSocket));
    }

    sockaddr_storage storage {};
    socklen_t        length = sizeof(storage);
    if (getsockname(NativeHandle(_nativeHandle), reinterpret_cast<sockaddr*>(&storage), &length) < 0) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint, errno));
    }
    return FromNativeEndpoint(reinterpret_cast<const sockaddr*>(&storage), length);
}

auto OpenSocket(const SocketType type, const SocketAddressFamily family) noexcept -> std::expected<Socket, SocketFailure> {
    const int nativeFamily = family == SocketAddressFamily::IPv4 ? AF_INET : family == SocketAddressFamily::IPv6 ? AF_INET6 : AF_UNSPEC;
    if (nativeFamily == AF_UNSPEC) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint));
    }

    const int nativeType = type == SocketType::Stream ? SOCK_STREAM : SOCK_DGRAM;
    int       descriptor = socket(nativeFamily, nativeType, 0);
    if (descriptor < 0) {
        return std::unexpected(Failure(SocketError::CreationFailed, errno));
    }

    const int descriptorFlags = fcntl(descriptor, F_GETFD, 0);
    if (descriptorFlags >= 0) {
        fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC);
    }
#ifdef SO_NOSIGPIPE
    const int noSigPipe = 1;
    if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe)) < 0) {
        const int error = errno;
        ::close(descriptor);
        return std::unexpected(Failure(SocketError::SetOptionFailed, error));
    }
#endif
    return SocketAccess::Adopt(descriptor, type, family);
}

auto ResolveSocketEndpoints(const std::string_view host, const uint16_t port, const SocketAddressFamily family, const SocketType type) noexcept
    -> std::expected<std::vector<SocketEndpoint>, SocketFailure> {
    addrinfo hints {};
    hints.ai_family   = family == SocketAddressFamily::IPv4 ? AF_INET : family == SocketAddressFamily::IPv6 ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = type == SocketType::Stream ? SOCK_STREAM : SOCK_DGRAM;
    hints.ai_protocol = type == SocketType::Stream ? IPPROTO_TCP : IPPROTO_UDP;

    const std::string hostName(host);
    const std::string service = std::to_string(port);
    addrinfo*         results = nullptr;
    const int         status  = getaddrinfo(hostName.c_str(), service.c_str(), &hints, &results);
    if (status != 0) {
        if (results != nullptr) {
            freeaddrinfo(results);
        }
        return std::unexpected(Failure(SocketError::ResolutionFailed, status, SocketErrorDomain::Resolver));
    }

    std::vector<SocketEndpoint> endpoints;
    for (const addrinfo* current = results; current != nullptr; current = current->ai_next) {
        auto endpoint = FromNativeEndpoint(current->ai_addr, static_cast<socklen_t>(current->ai_addrlen));
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
