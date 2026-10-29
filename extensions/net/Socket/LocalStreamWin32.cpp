// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Socket/LocalStream.hpp>
#include <Zahlen/Core/Platform.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace ZHLN::Net {

class LocalStreamAccess {
  public:
    [[nodiscard]] static auto Adopt(const HANDLE handle) noexcept -> LocalStream {
        return LocalStream(reinterpret_cast<std::uintptr_t>(handle));
    }

    [[nodiscard]] static auto Get(const LocalStream& stream) noexcept -> HANDLE {
        return reinterpret_cast<HANDLE>(stream._nativeHandle);
    }
};

namespace {

enum class PipeError : uint8_t {
    Other,
    PeerClosed
};

enum class TransferKind : uint8_t {
    Send,
    Receive
};

[[nodiscard]] auto ClassifyPipeError(const DWORD error) noexcept -> PipeError {
    switch (error) {
        case ERROR_BROKEN_PIPE:
        case ERROR_PIPE_NOT_CONNECTED:
        case ERROR_NO_DATA:
            return PipeError::PeerClosed;
        default:
            return PipeError::Other;
    }
}

[[nodiscard]] auto Failure(const SocketError error, const DWORD nativeCode = ERROR_SUCCESS) noexcept -> SocketFailure {
    return SocketFailure {
        .error = error,
        .domain = SocketErrorDomain::LocalIPC,
        .nativeCode = static_cast<int>(nativeCode)
    };
}

[[nodiscard]] auto TimeoutValue(const std::chrono::milliseconds timeout) noexcept -> DWORD {
    if (timeout.count() <= 0) {
        return 0;
    }
    const auto count = static_cast<uint64_t>(timeout.count());
    return count >= static_cast<uint64_t>(MAXDWORD) ? MAXDWORD - 1 : static_cast<DWORD>(count);
}

template <typename Byte, typename IoOperation>
[[nodiscard]] auto Transfer(
    const HANDLE handle,
    const std::span<Byte> bytes,
    const std::chrono::milliseconds timeout,
    const TransferKind kind,
    const SocketError ioError,
    IoOperation&& transfer
) noexcept -> std::expected<SocketIOResult, SocketFailure> {
    if (bytes.empty()) {
        return SocketIOResult {};
    }

    const DWORD length = bytes.size() >= static_cast<size_t>(MAXDWORD) ? MAXDWORD : static_cast<DWORD>(bytes.size());
    HANDLE     event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        return std::unexpected(Failure(SocketError::CreationFailed, GetLastError()));
    }

    OVERLAPPED operation {};
    operation.hEvent = event;
    DWORD transferred = 0;
    const BOOL completed = transfer(length, transferred, operation);

    if (completed == FALSE) {
        const DWORD operationError = GetLastError();
        if (operationError != ERROR_IO_PENDING) {
            CloseHandle(event);
            if (ClassifyPipeError(operationError) == PipeError::PeerClosed) {
                return SocketIOResult {.state = SocketIOState::PeerClosed, .bytes = 0};
            }
            return std::unexpected(Failure(ioError, operationError));
        }

        const DWORD waitResult = WaitForSingleObject(event, TimeoutValue(timeout));
        if (waitResult == WAIT_TIMEOUT) {
            CancelIo(handle);
            WaitForSingleObject(event, INFINITE);
            if (GetOverlappedResult(handle, &operation, &transferred, FALSE) == FALSE) {
                const DWORD resultError = GetLastError();
                CloseHandle(event);
                if (resultError == ERROR_OPERATION_ABORTED) {
                    return SocketIOResult {.state = SocketIOState::WouldBlock, .bytes = 0};
                }
                if (ClassifyPipeError(resultError) == PipeError::PeerClosed) {
                    return SocketIOResult {.state = SocketIOState::PeerClosed, .bytes = 0};
                }
                return std::unexpected(Failure(ioError, resultError));
            }
            CloseHandle(event);
        } else if (waitResult == WAIT_OBJECT_0) {
            if (GetOverlappedResult(handle, &operation, &transferred, FALSE) == FALSE) {
                const DWORD resultError = GetLastError();
                CloseHandle(event);
                if (ClassifyPipeError(resultError) == PipeError::PeerClosed) {
                    return SocketIOResult {.state = SocketIOState::PeerClosed, .bytes = 0};
                }
                return std::unexpected(Failure(ioError, resultError));
            }
            CloseHandle(event);
        } else {
            const DWORD waitError = GetLastError();
            CancelIo(handle);
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
            return std::unexpected(Failure(SocketError::WaitFailed, waitError));
        }
    } else {
        CloseHandle(event);
    }

    if (kind == TransferKind::Receive && transferred == 0) {
        return SocketIOResult {.state = SocketIOState::PeerClosed, .bytes = 0};
    }
    return SocketIOResult {.state = SocketIOState::Complete, .bytes = static_cast<size_t>(transferred)};
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
        _state = std::exchange(other._state, State::Closed);
    }
    return *this;
}

void LocalStream::Close() noexcept {
    if (_state == State::Open) {
        CloseHandle(LocalStreamAccess::Get(*this));
        _nativeHandle = 0;
        _state = State::Closed;
    }
}

auto LocalStream::Send(const std::span<const uint8_t> bytes, const std::chrono::milliseconds timeout) noexcept
    -> std::expected<SocketIOResult, SocketFailure> {
    if (_state != State::Open) {
        return std::unexpected(Failure(SocketError::InvalidSocket, ERROR_INVALID_HANDLE));
    }
    const auto writeFile = [handle = LocalStreamAccess::Get(*this), bytes](
                               const DWORD length,
                               DWORD& transferred,
                               OVERLAPPED& operation
                           ) noexcept -> BOOL {
        return WriteFile(handle, bytes.data(), length, &transferred, &operation);
    };
    return Transfer(LocalStreamAccess::Get(*this), bytes, timeout, TransferKind::Send, SocketError::SendFailed, writeFile);
}

auto LocalStream::Receive(const std::span<uint8_t> bytes, const std::chrono::milliseconds timeout) noexcept
    -> std::expected<SocketIOResult, SocketFailure> {
    if (_state != State::Open) {
        return std::unexpected(Failure(SocketError::InvalidSocket, ERROR_INVALID_HANDLE));
    }
    const auto readFile = [handle = LocalStreamAccess::Get(*this), bytes](
                              const DWORD length,
                              DWORD& transferred,
                              OVERLAPPED& operation
                          ) noexcept -> BOOL {
        return ReadFile(handle, bytes.data(), length, &transferred, &operation);
    };
    return Transfer(LocalStreamAccess::Get(*this), bytes, timeout, TransferKind::Receive, SocketError::ReceiveFailed, readFile);
}

auto ConnectLocalStream(const std::string_view endpoint, const std::chrono::milliseconds timeout) noexcept
    -> std::expected<LocalStream, SocketFailure> {
    constexpr std::string_view NamedPipePrefix = R"(\\?\pipe\)";
    if (!endpoint.starts_with(NamedPipePrefix) || endpoint.find('\0') != std::string_view::npos) {
        return std::unexpected(Failure(SocketError::InvalidEndpoint, ERROR_INVALID_NAME));
    }

    const std::string pipeName(endpoint);
    HANDLE            handle = CreateFileA(
        pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_BUSY) {
            if (WaitNamedPipeA(pipeName.c_str(), TimeoutValue(timeout)) == FALSE) {
                error = GetLastError();
                return std::unexpected(Failure(
                    error == ERROR_SEM_TIMEOUT ? SocketError::ConnectTimedOut : SocketError::ConnectFailed,
                    error
                ));
            }
            handle = CreateFileA(
                pipeName.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                nullptr
            );
        }
        if (handle == INVALID_HANDLE_VALUE) {
            return std::unexpected(Failure(SocketError::ConnectFailed, GetLastError()));
        }
    }

    DWORD mode = PIPE_READMODE_BYTE;
    if (SetNamedPipeHandleState(handle, &mode, nullptr, nullptr) == FALSE) {
        const DWORD error = GetLastError();
        CloseHandle(handle);
        return std::unexpected(Failure(SocketError::SetOptionFailed, error));
    }

    return LocalStreamAccess::Adopt(handle);
}

} // namespace ZHLN::Net
