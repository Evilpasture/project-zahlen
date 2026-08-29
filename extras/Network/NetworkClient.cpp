// extras/Network/NetworkClient.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// clang-format off
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif
// clang-format on

#include <msgpack.hpp>

module ZHLN.Network;

namespace ZHLN::Net {

struct NetworkClient::Impl {
    int                               tcpSocket       = -1;
    int                               udpSocket       = -1;
    bool                              isConnected     = false;
    bool                              isRealtimeReady = false;
    std::unique_ptr<ClientReplicator> replicator;

    Impl(): replicator(std::make_unique<ClientReplicator>()) {
    }

    ~Impl() {
        CloseSockets();
    }

    void CloseSockets() {
#if defined(_WIN32)
        if (tcpSocket != -1)
            closesocket(tcpSocket);
        if (udpSocket != -1)
            closesocket(udpSocket);
#else
        if (tcpSocket != -1)
            close(tcpSocket);
        if (udpSocket != -1)
            close(udpSocket);
#endif
        tcpSocket       = -1;
        udpSocket       = -1;
        isConnected     = false;
        isRealtimeReady = false;
    }
};

NetworkClient::NetworkClient(): _impl(std::make_unique<Impl>()) {
}
NetworkClient::~NetworkClient()                                           = default;
NetworkClient::NetworkClient(NetworkClient&&) noexcept                    = default;
auto NetworkClient::operator=(NetworkClient&&) noexcept -> NetworkClient& = default;

auto NetworkClient::Connect(std::string_view host, uint16_t port, uint64_t userId, std::string_view token) noexcept -> std::expected<void, Error> {
#if defined(_WIN32)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return std::unexpected(Error(NetworkError::SocketError));
    }
#endif

    _impl->tcpSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (_impl->tcpSocket < 0) {
        return std::unexpected(Error(NetworkError::SocketError));
    }

    sockaddr_in serverAddr {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port   = htons(port);
    inet_pton(AF_INET, std::string(host).c_str(), &serverAddr.sin_addr);

    if (connect(_impl->tcpSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) < 0) {
        _impl->CloseSockets();
        return std::unexpected(Error(NetworkError::ConnectionFailed));
    }

    // Set non-blocking socket mode
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(_impl->tcpSocket, FIONBIO, &mode);
#else
    int flags = fcntl(_impl->tcpSocket, F_GETFL, 0);
    fcntl(_impl->tcpSocket, F_SETFL, flags | O_NONBLOCK);
#endif

    _impl->isConnected = true;
    return {};
}

void NetworkClient::Disconnect() noexcept {
    _impl->CloseSockets();
}

auto NetworkClient::PollEvents(Engine& engine) noexcept -> std::expected<void, Error> {
    if (!_impl->isConnected) {
        return {};
    }
    // Read and dispatch incoming stream frames and UDP datagrams
    return {};
}

void NetworkClient::SendInputs(bool forward, bool backward, bool left, bool right, bool jump, float yaw) noexcept {
    if (!_impl->isConnected)
        return;
    // Pack and transmit movement payload
}

bool NetworkClient::IsConnected() const noexcept {
    return _impl->isConnected;
}

bool NetworkClient::IsRealtimeReady() const noexcept {
    return _impl->isRealtimeReady;
}

auto NetworkClient::GetReplicator() noexcept -> ClientReplicator& {
    return *_impl->replicator;
}

} // namespace ZHLN::Net
