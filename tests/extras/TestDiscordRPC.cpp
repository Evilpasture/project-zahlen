// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Discord/EngineIntegration.hpp>
#include <Discord/RichPresence.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <json/JSONSchema.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;

struct RpcHandshake {
    uint32_t    v = 0;
    std::string client_id;
};

struct RpcNonce {
    std::string nonce;
};

struct RpcActivity {
    uint32_t                    type = 0;
    std::optional<std::string>  details;
    std::optional<std::string>  state;
};

struct RpcActivityArguments {
    uint32_t            pid = 0;
    std::optional<RpcActivity> activity;
};

struct RpcSetActivity {
    std::string          cmd;
    RpcActivityArguments args;
    std::string          nonce;
};

struct RpcErrorData {
    int32_t     code = 0;
    std::string message;
};

struct RpcErrorEvent {
    std::string  cmd;
    std::string  evt;
    RpcErrorData data;
};

enum class DiscordTestError : uint8_t {
    UnexpectedProtocolResult = 1,
    UnexpectedClientResult,
    IpcSetupFailed,
    IpcExchangeFailed,
    ServerFailed,
    InvalidServerFrame
};

#if !defined(_WIN32)

class ScopedEnvironment {
  public:
    ScopedEnvironment(const std::string_view name, const std::string_view value): name(name) {
        if (const char* current = std::getenv(this->name.c_str()); current != nullptr) {
            previousValue = current;
        }
        if (::setenv(this->name.c_str(), std::string(value).c_str(), 1) != 0) {
            result = std::unexpected(DiscordTestError::IpcSetupFailed);
        }
    }

    ~ScopedEnvironment() {
        if (!result) {
            return;
        }
        if (previousValue) {
            ::setenv(name.c_str(), previousValue->c_str(), 1);
        } else {
            ::unsetenv(name.c_str());
        }
    }

    [[nodiscard]] auto Result() const noexcept -> const std::expected<void, DiscordTestError>& {
        return result;
    }

  private:
    std::string name;
    std::optional<std::string> previousValue;
    std::expected<void, DiscordTestError> result;
};

auto ReadExactDescriptor(const int descriptor, const std::span<uint8_t> bytes) noexcept
    -> std::expected<void, DiscordTestError> {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto received = ::recv(descriptor, bytes.data() + offset, bytes.size() - offset, 0);
        if (received > 0) {
            offset += static_cast<size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return std::unexpected(DiscordTestError::ServerFailed);
    }
    return {};
}

auto WriteAllDescriptor(const int descriptor, const std::span<const uint8_t> bytes) noexcept
    -> std::expected<void, DiscordTestError> {
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
        return std::unexpected(DiscordTestError::ServerFailed);
    }
    return {};
}

auto SendRpcFrame(
    const int descriptor,
    const ZHLN::Discord::RpcOpcode opcode,
    const std::string_view payload
) noexcept -> std::expected<void, DiscordTestError> {
    auto encoded = ZHLN::Discord::EncodeRpcFrame(opcode, payload);
    if (!encoded) {
        return std::unexpected(DiscordTestError::InvalidServerFrame);
    }
    return WriteAllDescriptor(descriptor, *encoded);
}

auto ReceiveRpcFrame(const int descriptor) noexcept -> std::expected<ZHLN::Discord::RpcFrame, DiscordTestError> {
    std::array<uint8_t, 8> header {};
    auto readHeader = ReadExactDescriptor(descriptor, header);
    if (!readHeader) {
        return std::unexpected(readHeader.error());
    }
    const uint32_t payloadSize = static_cast<uint32_t>(header[4]) | (static_cast<uint32_t>(header[5]) << 8U) |
                                 (static_cast<uint32_t>(header[6]) << 16U) | (static_cast<uint32_t>(header[7]) << 24U);
    if (payloadSize > 1024U * 1024U) {
        return std::unexpected(DiscordTestError::InvalidServerFrame);
    }

    std::vector<uint8_t> bytes(header.begin(), header.end());
    const size_t frameSize = bytes.size() + static_cast<size_t>(payloadSize);
    bytes.resize(frameSize);
    auto readPayload = ReadExactDescriptor(descriptor, std::span<uint8_t>(bytes).subspan(header.size()));
    if (!readPayload) {
        return std::unexpected(readPayload.error());
    }
    auto decoded = ZHLN::Discord::DecodeRpcFrame(bytes);
    if (!decoded) {
        return std::unexpected(DiscordTestError::InvalidServerFrame);
    }
    return std::move(*decoded);
}

auto HasReadiness(const pollfd& readiness) noexcept -> std::expected<void, DiscordTestError> {
    if ((readiness.revents & POLLIN) == 0) {
        return std::unexpected(DiscordTestError::IpcSetupFailed);
    }
    return {};
}

auto WaitForClient(const int listener) noexcept -> std::expected<void, DiscordTestError> {
    pollfd readiness {.fd = listener, .events = POLLIN, .revents = 0};
    if (::poll(&readiness, 1, 5000) <= 0) {
        return std::unexpected(DiscordTestError::IpcSetupFailed);
    }
    return HasReadiness(readiness);
}

auto VerifyHandshake(const ZHLN::Discord::RpcFrame& frame) -> std::expected<void, DiscordTestError> {
    if (frame.opcode != ZHLN::Discord::RpcOpcode::Handshake) {
        return std::unexpected(DiscordTestError::InvalidServerFrame);
    }
    auto handshake = ZHLN::ReflectJSON::Parse<RpcHandshake>(frame.payload);
    if (!handshake || handshake->v != 1 || handshake->client_id != "123456789012345678") {
        return std::unexpected(DiscordTestError::InvalidServerFrame);
    }
    return {};
}

auto RunRpcServer(const int listener) -> std::expected<void, DiscordTestError> {
    auto readiness = WaitForClient(listener);
    if (!readiness) {
        return std::unexpected(readiness.error());
    }
    const int accepted = ::accept(listener, nullptr, nullptr);
    if (accepted < 0) {
        return std::unexpected(DiscordTestError::ServerFailed);
    }

    timeval receiveTimeout {.tv_sec = 3, .tv_usec = 0};
    ::setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));
#if defined(SO_NOSIGPIPE)
    int noSignal = 1;
    ::setsockopt(accepted, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif

    auto handshakeFrame = ReceiveRpcFrame(accepted);
    if (!handshakeFrame) {
        ::close(accepted);
        return std::unexpected(handshakeFrame.error());
    }
    auto handshake = VerifyHandshake(*handshakeFrame);
    if (!handshake) {
        ::close(accepted);
        return std::unexpected(handshake.error());
    }

    auto ready = SendRpcFrame(
        accepted,
        ZHLN::Discord::RpcOpcode::Frame,
        R"({"cmd":"DISPATCH","evt":"READY","data":{}})"
    );
    if (!ready) {
        ::close(accepted);
        return std::unexpected(ready.error());
    }
    auto ping = SendRpcFrame(accepted, ZHLN::Discord::RpcOpcode::Ping, R"({"nonce":"server-ping"})");
    if (!ping) {
        ::close(accepted);
        return std::unexpected(ping.error());
    }

    std::optional<RpcSetActivity> activity;
    std::optional<RpcNonce> pong;
    for (size_t frameIndex = 0; frameIndex < 4 && (!activity || !pong); ++frameIndex) {
        auto frame = ReceiveRpcFrame(accepted);
        if (!frame) {
            ::close(accepted);
            return std::unexpected(frame.error());
        }
        if (frame->opcode == ZHLN::Discord::RpcOpcode::Pong) {
            auto parsedPong = ZHLN::ReflectJSON::Parse<RpcNonce>(frame->payload);
            if (parsedPong && parsedPong->nonce == "server-ping") {
                pong = std::move(*parsedPong);
            }
        }
        if (frame->opcode == ZHLN::Discord::RpcOpcode::Frame) {
            auto parsedActivity = ZHLN::ReflectJSON::Parse<RpcSetActivity>(frame->payload, {.omitEmpty = true});
            if (parsedActivity && parsedActivity->cmd == "SET_ACTIVITY" && parsedActivity->args.pid != 0 &&
                parsedActivity->args.activity &&
                parsedActivity->args.activity->details == "Loading \"Hanoi\"") {
                activity = std::move(*parsedActivity);
            }
        }
    }
    if (!activity || !pong) {
        ::close(accepted);
        return std::unexpected(DiscordTestError::ServerFailed);
    }

    const std::string errorPayload = ZHLN::ReflectJSON::SerializeJSON(RpcErrorEvent {
        .cmd = "SET_ACTIVITY",
        .evt = "ERROR",
        .data = {.code = 4006, .message = "Not authorized"}
    });
    auto rejection = SendRpcFrame(accepted, ZHLN::Discord::RpcOpcode::Frame, errorPayload);
    ::close(accepted);
    return rejection;
}

struct RpcServerJob {
    int listener = -1;
    std::expected<void, DiscordTestError> result = std::unexpected(DiscordTestError::ServerFailed);
};

void RunRpcServerTask(void* context) noexcept {
    auto& job = *static_cast<RpcServerJob*>(context);
    job.result = RunRpcServer(job.listener);
}

#endif

struct DiscordRpcTestSuite {
    enum class TestError : uint8_t {
        UnexpectedProtocolResult = 1,
        UnexpectedClientResult,
        IpcSetupFailed,
        IpcExchangeFailed
    };

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> rpc_frames_use_little_endian_lengths_and_round_trip_json() {
            constexpr std::string_view payload = R"({"cmd":"SET_ACTIVITY"})";
            auto encoded = ZHLN::Discord::EncodeRpcFrame(ZHLN::Discord::RpcOpcode::Frame, payload);
            if (!ZHLN::Test::ExpectTrue(encoded.has_value())) {
                return std::unexpected(TestError::UnexpectedProtocolResult);
            }

            ZHLN::Test::ExpectEq((*encoded)[0], uint8_t {1});
            ZHLN::Test::ExpectEq((*encoded)[1], uint8_t {0});
            ZHLN::Test::ExpectEq((*encoded)[2], uint8_t {0});
            ZHLN::Test::ExpectEq((*encoded)[3], uint8_t {0});
            ZHLN::Test::ExpectEq((*encoded)[4], static_cast<uint8_t>(payload.size() & 0xFFU));
            ZHLN::Test::ExpectEq((*encoded)[5], static_cast<uint8_t>((payload.size() >> 8U) & 0xFFU));

            auto decoded = ZHLN::Discord::DecodeRpcFrame(*encoded);
            if (!ZHLN::Test::ExpectTrue(decoded.has_value())) {
                return std::unexpected(TestError::UnexpectedProtocolResult);
            }
            ZHLN::Test::ExpectTrue(decoded->opcode == ZHLN::Discord::RpcOpcode::Frame);
            ZHLN::Test::ExpectTrue(decoded->payload == payload);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> rpc_decoder_rejects_truncated_and_oversized_frames() {
            constexpr std::array<uint8_t, 7> truncated {};
            auto incomplete = ZHLN::Discord::DecodeRpcFrame(truncated);
            ZHLN::Test::ExpectTrue(
                !incomplete && incomplete.error() == ZHLN::Discord::RpcProtocolError::InvalidFrame
            );

            constexpr std::array<uint8_t, 8> oversizedHeader {
                0x01, 0x00, 0x00, 0x00,
                0x01, 0x00, 0x10, 0x00
            };
            auto oversized = ZHLN::Discord::DecodeRpcFrame(oversizedHeader);
            ZHLN::Test::ExpectTrue(
                !oversized && oversized.error() == ZHLN::Discord::RpcProtocolError::PayloadTooLarge
            );
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> client_requires_a_decimal_application_id_before_starting() {
            static_assert(static_cast<uint8_t>(ZHLN::Discord::PresenceError::InvalidApplicationId) == 1);

            ZHLN::Discord::DiscordPresenceClient client("not-a-discord-application-id");
            auto started = client.Start();
            if (!ZHLN::Test::ExpectTrue(!started.has_value())) {
                return std::unexpected(TestError::UnexpectedClientResult);
            }
            ZHLN::Test::ExpectTrue(started.error() == ZHLN::Discord::PresenceError::InvalidApplicationId);
            ZHLN::Test::ExpectTrue(client.State() == ZHLN::Discord::PresenceState::Stopped);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> client_start_fails_fast_when_background_fiber_capacity_is_exhausted() {
            ZHLN::TaskSystem::Scope taskScope {1, 0, ZHLN::kMinimumFiberStackSize};
            ZHLN::Discord::DiscordPresenceClient client("123456789012345678");
            auto started = client.Start();
            if (!ZHLN::Test::ExpectTrue(!started.has_value())) {
                return std::unexpected(TestError::UnexpectedClientResult);
            }
            ZHLN::Test::ExpectTrue(started.error() == ZHLN::Discord::PresenceError::TaskCapacityExhausted);
            ZHLN::Test::ExpectTrue(client.State() == ZHLN::Discord::PresenceState::Stopped);
            return {};
        }

#if !defined(_WIN32)
        std::expected<void, ZHLN::ErrorCode> rpc_client_uses_task_system_and_surfaces_rpc_rejection() {
            ZHLN::TaskSystem::Scope taskScope {2, 64, ZHLN::kMinimumFiberStackSize};
            std::string directoryTemplate = "/tmp/zahlendiscord-XXXXXX";
            char*       directoryValue = ::mkdtemp(directoryTemplate.data());
            if (!ZHLN::Test::ExpectTrue(directoryValue != nullptr)) {
                return std::unexpected(TestError::IpcSetupFailed);
            }
            const std::string directory(directoryValue);
            const std::string endpoint = directory + "/discord-ipc-0";

            const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (!ZHLN::Test::ExpectTrue(listener >= 0)) {
                ::rmdir(directory.c_str());
                return std::unexpected(TestError::IpcSetupFailed);
            }

            sockaddr_un address {};
            if (endpoint.size() >= sizeof(address.sun_path)) {
                ::close(listener);
                ::rmdir(directory.c_str());
                return std::unexpected(TestError::IpcSetupFailed);
            }
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, endpoint.data(), endpoint.size());
            if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
                ::listen(listener, 1) != 0) {
                ::close(listener);
                ::unlink(endpoint.c_str());
                ::rmdir(directory.c_str());
                return std::unexpected(TestError::IpcSetupFailed);
            }

            ScopedEnvironment runtimeDirectory("XDG_RUNTIME_DIR", directory);
            if (!runtimeDirectory.Result()) {
                ::close(listener);
                ::unlink(endpoint.c_str());
                ::rmdir(directory.c_str());
                return std::unexpected(TestError::IpcSetupFailed);
            }

            RpcServerJob server {.listener = listener};
            ZHLN::TaskSystem::Counter serverCompletion;
            const ZHLN::TaskSystem::Task serverTask {.func = &RunRpcServerTask, .arg = &server};
            ZHLN::TaskSystem::Dispatch(std::span<const ZHLN::TaskSystem::Task> {&serverTask, 1}, &serverCompletion);

            ZHLN::Discord::DiscordPresenceClient client("123456789012345678");
            client.SetActivity(ZHLN::Discord::RichPresence {
                .details = "Loading \"Hanoi\"",
                .state = "Chapter 4",
                .startTimestamp = std::nullopt,
                .largeImage = std::nullopt,
                .largeText = std::nullopt
            });
            const auto started = client.Start();
            if (!started) {
                ZHLN::TaskSystem::Wait(&serverCompletion);
                ::close(listener);
                ::unlink(endpoint.c_str());
                ::rmdir(directory.c_str());
                return std::unexpected(TestError::UnexpectedClientResult);
            }

            const auto deadline = std::chrono::steady_clock::now() + 4s;
            while (std::chrono::steady_clock::now() < deadline &&
                   client.State() != ZHLN::Discord::PresenceState::RpcRejected) {
                std::this_thread::sleep_for(10ms);
            }
            client.Stop();
            const auto stopDeadline = std::chrono::steady_clock::now() + 4s;
            while (std::chrono::steady_clock::now() < stopDeadline &&
                   client.State() != ZHLN::Discord::PresenceState::Stopped) {
                std::this_thread::sleep_for(10ms);
            }
            const bool stopped = client.State() == ZHLN::Discord::PresenceState::Stopped;
            ZHLN::TaskSystem::Wait(&serverCompletion);
            ::close(listener);
            ::unlink(endpoint.c_str());
            ::rmdir(directory.c_str());

            ZHLN::Test::ExpectTrue(server.result.has_value());
            ZHLN::Test::ExpectTrue(stopped);
            if (!server.result || !stopped) {
                return std::unexpected(TestError::IpcExchangeFailed);
            }
            return {};
        }
#endif
    };
};

} // namespace
