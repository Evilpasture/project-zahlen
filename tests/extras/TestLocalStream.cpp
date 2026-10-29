// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Socket/LocalStream.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

enum class LocalStreamTestError : uint8_t {
    ListenerSetupFailed = 1,
    ExchangeFailed,
    ServerFailed
};

enum class PipeTransferKind : uint8_t {
    Send,
    Receive
};

#if defined(_WIN32)

template <typename Byte>
auto TransferPipe(const HANDLE pipe, const std::span<Byte> bytes, const PipeTransferKind kind) noexcept
    -> std::expected<void, LocalStreamTestError> {
    size_t offset = 0;
    while (offset < bytes.size()) {
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) {
            return std::unexpected(LocalStreamTestError::ServerFailed);
        }

        OVERLAPPED operation {};
        operation.hEvent = event;
        DWORD transferred = 0;
        const DWORD length = static_cast<DWORD>(bytes.size() - offset);
        BOOL completed = FALSE;
        if (kind == PipeTransferKind::Send) {
            if constexpr (std::is_const_v<Byte>) {
                completed = WriteFile(pipe, bytes.data() + offset, length, &transferred, &operation);
            } else {
                CloseHandle(event);
                return std::unexpected(LocalStreamTestError::ServerFailed);
            }
        } else if constexpr (!std::is_const_v<Byte>) {
            completed = ReadFile(pipe, bytes.data() + offset, length, &transferred, &operation);
        } else {
            CloseHandle(event);
            return std::unexpected(LocalStreamTestError::ServerFailed);
        }
        if (completed == FALSE) {
            const DWORD error = GetLastError();
            if (error != ERROR_IO_PENDING || WaitForSingleObject(event, 5000) != WAIT_OBJECT_0 ||
                GetOverlappedResult(pipe, &operation, &transferred, FALSE) == FALSE) {
                CancelIo(pipe);
                WaitForSingleObject(event, INFINITE);
                CloseHandle(event);
                return std::unexpected(LocalStreamTestError::ServerFailed);
            }
        }
        CloseHandle(event);
        if (transferred == 0) {
            return std::unexpected(LocalStreamTestError::ServerFailed);
        }
        offset += static_cast<size_t>(transferred);
    }
    return {};
}

auto AcceptPipe(const HANDLE pipe) noexcept -> std::expected<void, LocalStreamTestError> {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        return std::unexpected(LocalStreamTestError::ServerFailed);
    }

    OVERLAPPED operation {};
    operation.hEvent = event;
    const BOOL connected = ConnectNamedPipe(pipe, &operation);
    if (connected != FALSE) {
        CloseHandle(event);
        return {};
    }

    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) {
        CloseHandle(event);
        return {};
    }
    if (error != ERROR_IO_PENDING || WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) {
        CancelIo(pipe);
        WaitForSingleObject(event, INFINITE);
        CloseHandle(event);
        return std::unexpected(LocalStreamTestError::ServerFailed);
    }

    DWORD transferred = 0;
    const BOOL completed = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
    CloseHandle(event);
    if (completed == FALSE) {
        return std::unexpected(LocalStreamTestError::ServerFailed);
    }
    return {};
}

#else

auto ReadExactDescriptor(const int descriptor, const std::span<uint8_t> bytes) noexcept
    -> std::expected<void, LocalStreamTestError> {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto received = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (received > 0) {
            offset += static_cast<size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return std::unexpected(LocalStreamTestError::ServerFailed);
    }
    return {};
}

auto WriteAllDescriptor(const int descriptor, const std::span<const uint8_t> bytes) noexcept
    -> std::expected<void, LocalStreamTestError> {
    size_t offset = 0;
#if defined(MSG_NOSIGNAL)
    constexpr int NoSignalFlag = MSG_NOSIGNAL;
#else
    constexpr int NoSignalFlag = 0;
#endif
    while (offset < bytes.size()) {
        const auto sent = ::send(descriptor, bytes.data() + offset, bytes.size() - offset, NoSignalFlag);
        if (sent > 0) {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        return std::unexpected(LocalStreamTestError::ServerFailed);
    }
    return {};
}

#endif

auto SendExactLocalStream(
    ZHLN::Net::LocalStream& stream,
    const std::span<const uint8_t> bytes,
    const std::chrono::milliseconds timeout
) noexcept -> std::expected<void, LocalStreamTestError> {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    size_t     offset = 0;
    while (offset < bytes.size()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::unexpected(LocalStreamTestError::ExchangeFailed);
        }
        auto sent = stream.Send(bytes.subspan(offset), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        if (!sent || sent->state == ZHLN::Net::SocketIOState::PeerClosed) {
            return std::unexpected(LocalStreamTestError::ExchangeFailed);
        }
        if (sent->state == ZHLN::Net::SocketIOState::WouldBlock) {
            continue;
        }
        if (sent->bytes == 0) {
            return std::unexpected(LocalStreamTestError::ExchangeFailed);
        }
        offset += sent->bytes;
    }
    return {};
}

struct LocalServerJob {
#if defined(_WIN32)
    HANDLE pipe = INVALID_HANDLE_VALUE;
#else
    int listener = -1;
#endif
    std::array<uint8_t, 8> request {};
    std::expected<void, LocalStreamTestError> result = std::unexpected(LocalStreamTestError::ServerFailed);
};

#if defined(_WIN32)

auto RunLocalServer(LocalServerJob& job) -> std::expected<void, LocalStreamTestError> {
    auto accepted = AcceptPipe(job.pipe);
    if (!accepted) {
        DisconnectNamedPipe(job.pipe);
        CloseHandle(job.pipe);
        return std::unexpected(accepted.error());
    }

    std::array<uint8_t, 8> received {};
    auto read = TransferPipe(job.pipe, std::span<uint8_t>(received), PipeTransferKind::Receive);
    if (!read || received != job.request) {
        FlushFileBuffers(job.pipe);
        DisconnectNamedPipe(job.pipe);
        CloseHandle(job.pipe);
        return std::unexpected(LocalStreamTestError::ServerFailed);
    }
    auto written = TransferPipe(job.pipe, std::span<const uint8_t>(received), PipeTransferKind::Send);
    FlushFileBuffers(job.pipe);
    DisconnectNamedPipe(job.pipe);
    CloseHandle(job.pipe);
    if (!written) {
        return std::unexpected(written.error());
    }
    return {};
}

#else

auto RunLocalServer(LocalServerJob& job) -> std::expected<void, LocalStreamTestError> {
    pollfd readiness {.fd = job.listener, .events = POLLIN, .revents = 0};
    if (::poll(&readiness, 1, 5000) <= 0 || (readiness.revents & POLLIN) == 0) {
        return std::unexpected(LocalStreamTestError::ServerFailed);
    }

    const int accepted = ::accept(job.listener, nullptr, nullptr);
    if (accepted < 0) {
        return std::unexpected(LocalStreamTestError::ServerFailed);
    }
    std::array<uint8_t, 8> received {};
    auto read = ReadExactDescriptor(accepted, received);
    if (!read || received != job.request) {
        ::close(accepted);
        return std::unexpected(LocalStreamTestError::ServerFailed);
    }
    auto written = WriteAllDescriptor(accepted, received);
    ::close(accepted);
    if (!written) {
        return std::unexpected(written.error());
    }
    return {};
}

#endif

void RunLocalServerTask(void* context) noexcept {
    auto& job = *static_cast<LocalServerJob*>(context);
    job.result = RunLocalServer(job);
}

} // namespace

namespace {

struct LocalStreamTestSuite {
    enum class TestError : uint8_t {
        ListenerSetupFailed = 1,
        ExchangeFailed
    };

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> local_stream_echoes_bytes_and_honors_read_timeouts() {
            ZHLN::TaskSystem::Scope taskScope {2, 64, ZHLN::kMinimumFiberStackSize};
            constexpr std::array<uint8_t, 8> request {0x5A, 0x48, 0x4C, 0x4E, 0x00, 0x01, 0xFE, 0xFF};
            std::string endpoint;
            LocalServerJob server {.request = request};
#if defined(_WIN32)
            endpoint = "\\\\?\\pipe\\zahlensocket-test-" + std::to_string(GetCurrentProcessId());
            server.pipe = CreateNamedPipeA(
                endpoint.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1,
                4096,
                4096,
                0,
                nullptr
            );
            if (!ZHLN::Test::ExpectTrue(server.pipe != INVALID_HANDLE_VALUE)) {
                return std::unexpected(TestError::ListenerSetupFailed);
            }
#else
            endpoint = "/tmp/zahlensocket-test-" + std::to_string(::getpid()) + ".sock";
            sockaddr_un address {};
            if (endpoint.size() >= sizeof(address.sun_path)) {
                return std::unexpected(TestError::ListenerSetupFailed);
            }

            server.listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (!ZHLN::Test::ExpectTrue(server.listener >= 0)) {
                return std::unexpected(TestError::ListenerSetupFailed);
            }
            ::unlink(endpoint.c_str());
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, endpoint.data(), endpoint.size());
            if (::bind(server.listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
                ::listen(server.listener, 1) != 0) {
                ::close(server.listener);
                ::unlink(endpoint.c_str());
                return std::unexpected(TestError::ListenerSetupFailed);
            }
#endif

            ZHLN::TaskSystem::Counter serverCompletion;
            const ZHLN::TaskSystem::Task serverTask {.func = &RunLocalServerTask, .arg = &server};
            ZHLN::TaskSystem::Dispatch(std::span<const ZHLN::TaskSystem::Task> {&serverTask, 1}, &serverCompletion);

            std::expected<void, LocalStreamTestError> exchange;
            auto connected = ZHLN::Net::ConnectLocalStream(endpoint, 2s);
            if (!connected) {
                exchange = std::unexpected(LocalStreamTestError::ExchangeFailed);
            } else {
                std::array<uint8_t, request.size()> response {};
                auto timeout = connected->Receive(response, 10ms);
                if (!timeout || timeout->state != ZHLN::Net::SocketIOState::WouldBlock) {
                    exchange = std::unexpected(LocalStreamTestError::ExchangeFailed);
                } else {
                    exchange = SendExactLocalStream(*connected, request, 2s);
                    if (exchange) {
                        size_t offset = 0;
                        const auto deadline = std::chrono::steady_clock::now() + 2s;
                        while (offset < response.size() && std::chrono::steady_clock::now() < deadline) {
                            auto received = connected->Receive(std::span<uint8_t>(response).subspan(offset), 100ms);
                            if (!received || received->state == ZHLN::Net::SocketIOState::PeerClosed) {
                                exchange = std::unexpected(LocalStreamTestError::ExchangeFailed);
                                break;
                            }
                            if (received->state == ZHLN::Net::SocketIOState::Complete) {
                                offset += received->bytes;
                            }
                        }
                        if (exchange && response != request) {
                            exchange = std::unexpected(LocalStreamTestError::ExchangeFailed);
                        }
                    }
                }
                connected->Close();
            }

            ZHLN::TaskSystem::Wait(&serverCompletion);
#if !defined(_WIN32)

            ::close(server.listener);
            ::unlink(endpoint.c_str());
#endif
            ZHLN::Test::ExpectTrue(exchange.has_value());
            ZHLN::Test::ExpectTrue(server.result.has_value());
            if (!exchange || !server.result) {
                return std::unexpected(TestError::ExchangeFailed);
            }
            return {};
        }
    };
};

} // namespace
