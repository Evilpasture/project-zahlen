// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestSocket.cpp
//
// Exercises TCP and UDP using only the platform-neutral socket value API. Every
// peer stays on 127.0.0.1 and uses an ephemeral port; no network egress or
// platform socket headers are involved in this suite.

#include "TestsFramework.hpp"
#include <Socket/Socket.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using ZHLN::Net::Socket;
using ZHLN::Net::SocketAddressFamily;
using ZHLN::Net::SocketEndpoint;
using ZHLN::Net::SocketError;
using ZHLN::Net::SocketFailure;
using ZHLN::Net::SocketIOState;
using ZHLN::Net::SocketType;

struct BoundListener {
    Socket         socket;
    SocketEndpoint endpoint;
};

auto MakeListener() -> std::expected<BoundListener, SocketFailure> {
    auto opened = ZHLN::Net::OpenSocket(SocketType::Stream, SocketAddressFamily::IPv4);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    Socket listener = std::move(*opened);

    auto reuse = listener.SetReuseAddress(true);
    if (!reuse) {
        return std::unexpected(reuse.error());
    }
    auto bound = listener.Bind(SocketEndpoint::IPv4Loopback(0));
    if (!bound) {
        return std::unexpected(bound.error());
    }
    auto listening = listener.Listen(8);
    if (!listening) {
        return std::unexpected(listening.error());
    }
    auto nonblocking = listener.SetNonBlocking(true);
    if (!nonblocking) {
        return std::unexpected(nonblocking.error());
    }
    auto endpoint = listener.LocalEndpoint();
    if (!endpoint) {
        return std::unexpected(endpoint.error());
    }
    return BoundListener {.socket = std::move(listener), .endpoint = *endpoint};
}

auto Remaining(const std::chrono::steady_clock::time_point deadline) -> std::chrono::milliseconds {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
}

auto WriteAll(Socket& socket, const std::span<const uint8_t> bytes, const std::chrono::milliseconds timeout) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    size_t     offset   = 0;
    while (offset < bytes.size()) {
        auto result = socket.Send(bytes.subspan(offset));
        if (!result) {
            return false;
        }
        if (result->state == SocketIOState::WouldBlock) {
            auto readiness = socket.WaitFor(false, true, Remaining(deadline));
            if (!readiness || !readiness->writable) {
                return false;
            }
            continue;
        }
        if (result->state != SocketIOState::Complete || result->bytes == 0) {
            return false;
        }
        offset += result->bytes;
    }
    return true;
}

auto ReadExact(Socket& socket, const std::span<uint8_t> bytes, const std::chrono::milliseconds timeout) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    size_t     offset   = 0;
    while (offset < bytes.size()) {
        auto readiness = socket.WaitFor(true, false, Remaining(deadline));
        if (!readiness || !readiness->readable) {
            return false;
        }
        auto result = socket.Receive(bytes.subspan(offset));
        if (!result || result->state == SocketIOState::PeerClosed) {
            return false;
        }
        if (result->state == SocketIOState::WouldBlock) {
            continue;
        }
        if (result->bytes == 0) {
            return false;
        }
        offset += result->bytes;
    }
    return true;
}

} // namespace

namespace {

struct SocketTestSuite {
    enum class SocketTestError : uint8_t {
        SocketSetupFailed = 1,
        ExchangeFailed,
    };

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> the_socket_is_a_move_only_platform_neutral_value() {
            static_assert(!std::is_copy_constructible_v<Socket>);
            static_assert(std::is_nothrow_move_constructible_v<Socket>);
            static_assert(std::is_trivially_copyable_v<SocketEndpoint>);

            ZHLN::Test::ExpectTrue(SocketEndpoint::IPv4Loopback(1234).IsValid());
            ZHLN::Test::ExpectEq(SocketEndpoint::IPv4Loopback(1234).address[0], uint8_t {127});
            ZHLN::Test::ExpectEq(SocketEndpoint::IPv4Loopback(1234).address[3], uint8_t {1});
            ZHLN::Test::ExpectEq(static_cast<uint8_t>(SocketError::RuntimeInitializationFailed), uint8_t {1});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> tcp_listener_accepts_and_echoes_on_loopback() {
            auto listenerResult = MakeListener();
            if (!ZHLN::Test::ExpectTrue(listenerResult.has_value())) {
                return std::unexpected(SocketTestError::SocketSetupFailed);
            }

            Socket         listener = std::move(listenerResult->socket);
            SocketEndpoint endpoint = listenerResult->endpoint;
            auto resolved = ZHLN::Net::ResolveSocketEndpoints(
                "127.0.0.1", endpoint.port, SocketAddressFamily::IPv4, SocketType::Stream
            );
            if (!ZHLN::Test::ExpectTrue(resolved.has_value() && !resolved->empty())) {
                return std::unexpected(SocketTestError::SocketSetupFailed);
            }
            ZHLN::Test::ExpectTrue(resolved->front() == endpoint);
            std::atomic<bool> serverSucceeded = false;
            std::jthread server(
                [listener = std::move(listener), &serverSucceeded](std::stop_token) mutable {
                    auto readiness = listener.WaitFor(true, false, std::chrono::seconds(2));
                    if (!readiness || !readiness->readable) {
                        return;
                    }
                    auto accepted = ZHLN::Net::AcceptSocket(listener);
                    if (!accepted) {
                        return;
                    }
                    auto blocking = accepted->SetNonBlocking(false);
                    if (!blocking) {
                        return;
                    }

                    std::array<uint8_t, 8> request {};
                    if (!ReadExact(*accepted, request, std::chrono::seconds(2))) {
                        return;
                    }
                    serverSucceeded.store(WriteAll(*accepted, request, std::chrono::seconds(2)));
                }
            );

            auto opened = ZHLN::Net::OpenSocket(SocketType::Stream, SocketAddressFamily::IPv4);
            if (!ZHLN::Test::ExpectTrue(opened.has_value())) {
                server.request_stop();
                server.join();
                return std::unexpected(SocketTestError::SocketSetupFailed);
            }
            Socket client = std::move(*opened);
            auto connected = client.Connect(endpoint, std::chrono::seconds(2));
            if (!ZHLN::Test::ExpectTrue(connected.has_value())) {
                server.request_stop();
                server.join();
                return std::unexpected(SocketTestError::ExchangeFailed);
            }

            constexpr std::array<uint8_t, 8> request {0x5A, 0x48, 0x4C, 0x4E, 0x00, 0x01, 0xFE, 0xFF};
            if (!ZHLN::Test::ExpectTrue(WriteAll(client, request, std::chrono::seconds(2)))) {
                server.request_stop();
                server.join();
                return std::unexpected(SocketTestError::ExchangeFailed);
            }

            std::array<uint8_t, request.size()> response {};
            const bool received = ReadExact(client, response, std::chrono::seconds(2));
            server.join();
            ZHLN::Test::ExpectTrue(received);
            ZHLN::Test::ExpectTrue(serverSucceeded.load());
            ZHLN::Test::ExpectTrue(response == request);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> udp_sends_and_receives_datagrams_with_peer_values() {
            auto openedServer = ZHLN::Net::OpenSocket(SocketType::Datagram, SocketAddressFamily::IPv4);
            if (!ZHLN::Test::ExpectTrue(openedServer.has_value())) {
                return std::unexpected(SocketTestError::SocketSetupFailed);
            }
            Socket server = std::move(*openedServer);
            auto nonblockingServer = server.SetNonBlocking(true);
            auto bound = server.Bind(SocketEndpoint::IPv4Loopback(0));
            auto serverEndpoint = server.LocalEndpoint();
            if (!ZHLN::Test::ExpectTrue(nonblockingServer.has_value() && bound.has_value() && serverEndpoint.has_value())) {
                return std::unexpected(SocketTestError::SocketSetupFailed);
            }

            auto openedClient = ZHLN::Net::OpenSocket(SocketType::Datagram, SocketAddressFamily::IPv4);
            if (!ZHLN::Test::ExpectTrue(openedClient.has_value())) {
                return std::unexpected(SocketTestError::SocketSetupFailed);
            }
            Socket client = std::move(*openedClient);
            auto nonblockingClient = client.SetNonBlocking(true);
            if (!ZHLN::Test::ExpectTrue(nonblockingClient.has_value())) {
                return std::unexpected(SocketTestError::SocketSetupFailed);
            }

            constexpr std::array<uint8_t, 6> request {0x01, 0x02, 0x00, 0x7F, 0xFE, 0xFF};
            auto sent = client.SendTo(request, *serverEndpoint);
            if (!ZHLN::Test::ExpectTrue(sent.has_value() && sent->state == SocketIOState::Complete
                                        && sent->bytes == request.size())) {
                return std::unexpected(SocketTestError::ExchangeFailed);
            }

            std::array<uint8_t, 16> receiveBuffer {};
            auto serverReady = server.WaitFor(true, false, std::chrono::seconds(2));
            auto received = server.ReceiveFrom(receiveBuffer);
            if (!ZHLN::Test::ExpectTrue(serverReady.has_value() && serverReady->readable && received.has_value()
                                        && received->io.state == SocketIOState::Complete
                                        && received->io.bytes == request.size())) {
                return std::unexpected(SocketTestError::ExchangeFailed);
            }

            auto reply = server.SendTo(
                std::span<const uint8_t>(receiveBuffer.data(), received->io.bytes), received->peer
            );
            if (!ZHLN::Test::ExpectTrue(reply.has_value() && reply->state == SocketIOState::Complete
                                        && reply->bytes == request.size())) {
                return std::unexpected(SocketTestError::ExchangeFailed);
            }

            auto clientReady = client.WaitFor(true, false, std::chrono::seconds(2));
            auto response = client.ReceiveFrom(receiveBuffer);
            if (!ZHLN::Test::ExpectTrue(clientReady.has_value() && clientReady->readable && response.has_value()
                                        && response->io.state == SocketIOState::Complete
                                        && response->io.bytes == request.size())) {
                return std::unexpected(SocketTestError::ExchangeFailed);
            }
            ZHLN::Test::ExpectEq(
                static_cast<uint8_t>(response->peer.family), static_cast<uint8_t>(SocketAddressFamily::IPv4)
            );
            ZHLN::Test::ExpectTrue(response->peer.address == SocketEndpoint::IPv4Loopback(0).address);
            ZHLN::Test::ExpectTrue(response->peer.port != 0);
            ZHLN::Test::ExpectTrue(std::equal(request.begin(), request.end(), receiveBuffer.begin()));
            return {};
        }
    };
};

} // namespace
