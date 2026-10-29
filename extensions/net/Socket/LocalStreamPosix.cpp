// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Socket/LocalEndpoint.hpp>
#include <Socket/LocalStream.hpp>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace ZHLN::Net {

namespace {

auto GetCandidateDirectories(IpcScope scope) -> std::vector<std::string_view> {
    if (scope == IpcScope::SystemWide) {
        return {"/tmp", "/var/run"};
    }
    // User-session priority order
    static constexpr std::array   kEnvVars = {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"};
    std::vector<std::string_view> dirs;
    for (const char* var: kEnvVars) {
        if (const char* val = std::getenv(var); val && val[0] != '\0') {
            dirs.push_back(val);
        }
    }
    dirs.push_back("/tmp");
    return dirs;
}
} // namespace

auto EnumerateLocalEndpoints(const LocalEndpointPattern pattern) -> std::vector<std::string> {
    std::vector<std::string> endpoints;
    auto                     dirs = GetCandidateDirectories(pattern.scope);

    for (std::string_view dir: dirs) {
        std::string base(dir);
        if (base.back() != '/') {
            base.push_back('/');
        }
        base.append(pattern.serviceName);

        for (uint32_t i = 0; i < pattern.maxInstances; ++i) {
            std::string path = base;
            if (pattern.indexed) {
                path.push_back('-');
                path.append(std::to_string(i));
            }
            if (std::find(endpoints.begin(), endpoints.end(), path) == endpoints.end()) {
                endpoints.push_back(std::move(path));
            }
        }
    }
    return endpoints;
}

class LocalStreamAccess {
  public:
    [[nodiscard]] static auto Adopt(const int descriptor) noexcept -> LocalStream {
        return LocalStream(static_cast<std::uintptr_t>(descriptor));
    }

    [[nodiscard]] static auto Get(const LocalStream& stream) noexcept -> int {
        return static_cast<int>(stream._nativeHandle);
    }
};

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] auto Failure(const SocketError error, const int nativeCode = 0) noexcept -> SocketFailure {
    return SocketFailure {.error = error, .domain = SocketErrorDomain::LocalIPC, .nativeCode = nativeCode};
}

[[nodiscard]] auto RemainingMilliseconds(const Clock::time_point deadline) noexcept -> int {
    const auto now = Clock::now();
    if (now >= deadline) {
        return 0;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(std::clamp<int64_t>(remaining, 1, INT_MAX));
}

enum class WaitResult : uint8_t { Ready, TimedOut };

[[nodiscard]] auto
    WaitForEvent(const int descriptor, const short events, const std::chrono::milliseconds timeout) noexcept -> std::expected<WaitResult, SocketFailure> {
    const auto deadline = Clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
    pollfd     event {.fd = descriptor, .events = events, .revents = 0};

    for (;;) {
        event.revents    = 0;
        const int result = ::poll(&event, 1, RemainingMilliseconds(deadline));
        if (result > 0) {
            if ((event.revents & POLLNVAL) != 0) {
                return std::unexpected(Failure(SocketError::WaitFailed, EBADF));
            }
            return WaitResult::Ready;
        }
        if (result == 0) {
            return WaitResult::TimedOut;
        }
        if (errno != EINTR) {
            return std::unexpected(Failure(SocketError::WaitFailed, errno));
        }
        if (Clock::now() >= deadline) {
            return WaitResult::TimedOut;
        }
    }
}

[[nodiscard]] auto SetNonBlocking(const int descriptor) noexcept -> std::expected<void, SocketFailure> {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0) {
        return std::unexpected(Failure(SocketError::SetOptionFailed, errno));
    }
    if (::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        return std::unexpected(Failure(SocketError::SetOptionFailed, errno));
    }

    const int descriptorFlags = ::fcntl(descriptor, F_GETFD, 0);
    if (descriptorFlags < 0 || ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0) {
        return std::unexpected(Failure(SocketError::SetOptionFailed, errno));
    }
    return {};
}

[[nodiscard]] auto ConnectDeadline(const std::chrono::milliseconds timeout) noexcept -> Clock::time_point {
    return Clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
}

[[nodiscard]] auto RemainingDuration(const Clock::time_point deadline) noexcept -> std::chrono::milliseconds {
    const auto now = Clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

} // namespace

LocalStream::~LocalStream() noexcept {
    Close();
}

LocalStream::LocalStream(LocalStream&& other) noexcept:
    _nativeHandle(std::exchange(other._nativeHandle, 0)), _state(std::exchange(other._state, State::Closed)) {
}

auto LocalStream::operator=(LocalStream&& other) noexcept -> LocalStream& {
    if (this != &other) {
        Close();
        _nativeHandle = std::exchange(other._nativeHandle, 0);
        _state        = std::exchange(other._state, State::Closed);
    }
    return *this;
}

void LocalStream::Close() noexcept {
    if (_state == State::Open) {
        ::close(LocalStreamAccess::Get(*this));
        _nativeHandle = 0;
        _state        = State::Closed;
    }
}

auto LocalStream::Send(const std::span<const uint8_t> bytes, const std::chrono::milliseconds timeout) noexcept -> std::expected<SocketIOResult, SocketFailure> {
    if (_state != State::Open) {
        return std::unexpected(Failure(SocketError::InvalidSocket, EBADF));
    }
    if (bytes.empty()) {
        return SocketIOResult {};
    }

    const int  descriptor = LocalStreamAccess::Get(*this);
    const auto deadline   = ConnectDeadline(timeout);
#if defined(MSG_NOSIGNAL)
    constexpr int NoSignalFlag = MSG_NOSIGNAL;
#else
    constexpr int NoSignalFlag = 0;
#endif

    for (;;) {
        auto ready = WaitForEvent(descriptor, POLLOUT, RemainingDuration(deadline));
        if (!ready) {
            return std::unexpected(ready.error());
        }
        if (*ready == WaitResult::TimedOut) {
            return SocketIOResult {.state = SocketIOState::WouldBlock, .bytes = 0};
        }

        const auto transferred = ::send(descriptor, bytes.data(), bytes.size(), NoSignalFlag);
        if (transferred >= 0) {
            if (transferred == 0) {
                return SocketIOResult {.state = SocketIOState::WouldBlock, .bytes = 0};
            }
            return SocketIOResult {.state = SocketIOState::Complete, .bytes = static_cast<size_t>(transferred)};
        }

        const int error = errno;
        if (error == EINTR) {
            if (Clock::now() >= deadline) {
                return SocketIOResult {.state = SocketIOState::WouldBlock, .bytes = 0};
            }
            continue;
        }
        if (error == EAGAIN || error == EWOULDBLOCK) {
            if (Clock::now() >= deadline) {
                return SocketIOResult {.state = SocketIOState::WouldBlock, .bytes = 0};
            }
            continue;
        }
        return std::unexpected(Failure(SocketError::SendFailed, error));
    }
}

auto LocalStream::Receive(const std::span<uint8_t> bytes, const std::chrono::milliseconds timeout) noexcept -> std::expected<SocketIOResult, SocketFailure> {
    if (_state != State::Open) {
        return std::unexpected(Failure(SocketError::InvalidSocket, EBADF));
    }
    if (bytes.empty()) {
        return SocketIOResult {};
    }

    const int  descriptor = LocalStreamAccess::Get(*this);
    const auto deadline   = ConnectDeadline(timeout);
    for (;;) {
        auto ready = WaitForEvent(descriptor, POLLIN, RemainingDuration(deadline));
        if (!ready) {
            return std::unexpected(ready.error());
        }
        if (*ready == WaitResult::TimedOut) {
            return SocketIOResult {.state = SocketIOState::WouldBlock, .bytes = 0};
        }

        const auto transferred = ::recv(descriptor, bytes.data(), bytes.size(), 0);
        if (transferred > 0) {
            return SocketIOResult {.state = SocketIOState::Complete, .bytes = static_cast<size_t>(transferred)};
        }
        if (transferred == 0) {
            return SocketIOResult {.state = SocketIOState::PeerClosed, .bytes = 0};
        }

        const int error = errno;
        if (error == EINTR) {
            if (Clock::now() >= deadline) {
                return SocketIOResult {.state = SocketIOState::WouldBlock, .bytes = 0};
            }
            continue;
        }
        if (error == EAGAIN || error == EWOULDBLOCK) {
            if (Clock::now() >= deadline) {
                return SocketIOResult {.state = SocketIOState::WouldBlock, .bytes = 0};
            }
            continue;
        }
        return std::unexpected(Failure(SocketError::ReceiveFailed, error));
    }
}

auto ConnectLocalStream(const std::string_view endpoint, const std::chrono::milliseconds timeout) noexcept -> std::expected<LocalStream, SocketFailure> {
    if (endpoint.empty() || endpoint.find('\0') != std::string_view::npos) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint, EINVAL));
    }

    sockaddr_un address {};
    if (endpoint.size() >= sizeof(address.sun_path)) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint, ENAMETOOLONG));
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.data(), endpoint.size());

    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return std::unexpected(Failure(SocketError::CreationFailed, errno));
    }
    auto nonBlocking = SetNonBlocking(descriptor);
    if (!nonBlocking) {
        ::close(descriptor);
        return std::unexpected(nonBlocking.error());
    }

    const int result = ::connect(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (result == 0) {
        return LocalStreamAccess::Adopt(descriptor);
    }

    const int connectError = errno;
    if (connectError != EINPROGRESS && connectError != EALREADY && connectError != EINTR && connectError != EAGAIN && connectError != EWOULDBLOCK) {
        ::close(descriptor);
        return std::unexpected(Failure(SocketError::ConnectFailed, connectError));
    }

    auto ready = WaitForEvent(descriptor, POLLOUT, timeout);
    if (!ready) {
        const auto failure = ready.error();
        ::close(descriptor);
        return std::unexpected(failure);
    }
    if (*ready == WaitResult::TimedOut) {
        ::close(descriptor);
        return std::unexpected(Failure(SocketError::ConnectTimedOut, ETIMEDOUT));
    }

    int       socketError       = 0;
    socklen_t socketErrorLength = sizeof(socketError);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorLength) != 0) {
        const int error = errno;
        ::close(descriptor);
        return std::unexpected(Failure(SocketError::ConnectFailed, error));
    }
    if (socketError != 0) {
        ::close(descriptor);
        return std::unexpected(Failure(SocketError::ConnectFailed, socketError));
    }

    return LocalStreamAccess::Adopt(descriptor);
}

} // namespace ZHLN::Net
