// extensions/net/Network/NetworkClient.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
// NetworkClient — TCP handshake + framed stream, UDP realtime channel.
//
//   Connect:    non-blocking TCP connect (socket-backed timeout), ClientHello
//               frame, waits for the ServerWelcome that announces the UDP
//               realtime port, then connects the UDP socket.
//   PollEvents: drains both sockets, reassembles stream frames, verifies
//               CRC32, decodes envelopes and hands messages to the
//               ClientReplicator. Malformed frames close the stream; corrupt
//               datagrams are dropped.
//   SendInputs: packs a ClientInputMessage via ZHLN.Wire and fires a UDP
//               datagram frame (loss-tolerant, ordering irrelevant).
// ============================================================================

module;

#include <Socket/Socket.hpp>
#include <Zahlen/Core/BatchResult.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

module ZHLN.Network;

namespace ZHLN::Net {

namespace {

constexpr int    HANDSHAKE_TIMEOUT_MS = 5000;
constexpr size_t RECV_CHUNK_BYTES     = 64 * 1024;

enum class RecvStatus { Data, Closed, Blocked, Failed };

struct RecvResult {
    RecvStatus status     = RecvStatus::Failed;
    size_t     bytes      = 0;
    int        nativeCode = 0;
};

// Waits until the socket is readable/writable or the timeout expires.
auto WaitForSocket(Socket& socket, const bool writable, const int timeoutMs) -> bool {
    auto readiness = socket.WaitFor(!writable, writable, std::chrono::milliseconds(timeoutMs));
    return readiness.has_value() && (writable ? readiness->writable : readiness->readable);
}

auto SendAll(Socket& socket, const std::span<const uint8_t> bytes, const int timeoutMs) -> bool {
    size_t sent = 0;
    while (sent < bytes.size()) {
        auto result = socket.Send(bytes.subspan(sent));
        if (!result) {
            return false;
        }
        if (result->state == SocketIOState::WouldBlock) {
            if (!WaitForSocket(socket, true, timeoutMs)) {
                return false;
            }
            continue;
        }
        if (result->state != SocketIOState::Complete || result->bytes == 0) {
            return false;
        }
        sent += result->bytes;
    }
    return true;
}

auto RecvSome(Socket& socket, const std::span<uint8_t> destination) -> RecvResult {
    auto result = socket.Receive(destination);
    if (!result) {
        return RecvResult {.status = RecvStatus::Failed, .nativeCode = result.error().nativeCode};
    }
    switch (result->state) {
        case SocketIOState::Complete:
            return RecvResult {.status = RecvStatus::Data, .bytes = result->bytes};
        case SocketIOState::WouldBlock:
            return RecvResult {.status = RecvStatus::Blocked};
        case SocketIOState::PeerClosed:
            return RecvResult {.status = RecvStatus::Closed};
    }
    return {};
}

// Which socket a message arrived on. The same envelope over the stream and over
// a datagram are different failures -- a frame that desynchronised the stream
// against a datagram that simply went bad -- and worth telling apart in a log.
enum class Channel : uint8_t { Tcp, Udp };

/// One message that arrived whole and still could not be applied, and why.
struct MessageMiss {
    Channel channel = Channel::Tcp;
    //: The envelope's type, absent when the envelope itself did not decode.
    std::optional<MessageType> type {};
    ErrorCode                  error {};
    //: What the decoder said, when a decoder is what refused it.
    std::string detail;
};

/// Every message a poll applied, and every one it did not: the types that went
/// in against the misses that did not. This replaces an
/// `std::optional<ErrorCode>` chain in which a frame that failed to replicate
/// and a corrupt datagram behind it were the same value, the second one was
/// silently dropped, and everything after the first failure went unread until
/// the next poll -- if the caller looked at the error at all.
using MessageBatch = BatchResult<MessageType, MessageMiss>;

// Reads exactly one complete TCP frame (blocking with timeout).
auto RecvExactFrame(Socket& socket, Wire::Buffer& stream, int timeoutMs) -> Wire::Result<std::vector<uint8_t>> {
    uint8_t chunk[4096];
    while (stream.Size() < 4) {
        if (!WaitForSocket(socket, false, timeoutMs)) {
            return std::unexpected(Wire::MakeFailure(Wire::WireError::InvalidFrame, "timed out waiting for the frame header"));
        }
        const RecvResult received = RecvSome(socket, std::span<uint8_t>(chunk));
        switch (received.status) {
            case RecvStatus::Data: {
                auto appended = stream.Append({chunk, received.bytes});
                if (!appended) {
                    return std::unexpected(appended.error());
                }
                break;
            }
            case RecvStatus::Blocked:
                break;
            case RecvStatus::Closed:
                return std::unexpected(Wire::MakeFailure(Wire::WireError::InvalidFrame, "connection closed mid-frame"));
            case RecvStatus::Failed:
                return std::unexpected(Wire::MakeFailure(Wire::WireError::InvalidFrame,
                                                         std::format("socket error {} while reading the frame header", received.nativeCode)));
        }
    }

    auto length = PeekFrameLength(stream.Data());
    if (!length) {
        return std::unexpected(length.error());
    }

    while (stream.Size() < 4 + static_cast<size_t>(*length)) {
        if (!WaitForSocket(socket, false, timeoutMs)) {
            return std::unexpected(Wire::MakeFailure(Wire::WireError::InvalidFrame, "timed out waiting for the frame body"));
        }
        const RecvResult received = RecvSome(socket, std::span<uint8_t>(chunk));
        switch (received.status) {
            case RecvStatus::Data: {
                auto appended = stream.Append({chunk, received.bytes});
                if (!appended) {
                    return std::unexpected(appended.error());
                }
                break;
            }
            case RecvStatus::Blocked:
                break;
            case RecvStatus::Closed:
                return std::unexpected(Wire::MakeFailure(Wire::WireError::InvalidFrame, "connection closed mid-frame"));
            case RecvStatus::Failed:
                return std::unexpected(Wire::MakeFailure(Wire::WireError::InvalidFrame,
                                                         std::format("socket error {} while reading the frame body", received.nativeCode)));
        }
    }

    auto payload = DecodeFrame(stream.Data().first(4 + static_cast<size_t>(*length)));
    stream.Consume(4 + static_cast<size_t>(*length));
    return payload;
}

} // namespace

// ============================================================================
// Impl
// ============================================================================

struct NetworkClient::Impl {
    Socket tcpSocket;
    Socket udpSocket;
    bool         isConnected     = false;
    bool         isRealtimeReady = false;

    uint64_t    userId = 0;
    std::string token;

    SocketEndpoint serverAddress {};
    uint32_t       inputSequence = 0;

    Wire::Buffer tcpStream {MAX_STREAM_FRAME_BYTES};

    std::unique_ptr<ClientReplicator> replicator;

    Impl(): replicator(std::make_unique<ClientReplicator>()) {
    }

    ~Impl() {
        CloseSockets();
    }

    Impl(const Impl&)                    = delete;
    auto operator=(const Impl&) -> Impl& = delete;

    void CloseSockets() {
        tcpSocket.Close();
        udpSocket.Close();
        serverAddress   = {};
        isConnected     = false;
        isRealtimeReady = false;
        tcpStream.Clear();
    }

    // One message in, one batch out: the type it applied, or the miss that
    // stopped it. The caller merges these into the poll's ledger, so a message
    // that fails no longer decides for every message behind it.
    auto ApplyMessage(Engine& engine, const MessageEnvelope& envelope, Channel channel) -> MessageBatch {
        MessageBatch applied;
        const auto   record = [&applied, &envelope, channel](const std::expected<void, ErrorCode>& outcome) {
            if (outcome) {
                applied.AddSuccess(envelope.type);
                return;
            }
            applied.EmplaceError(MessageMiss {.channel = channel, .type = envelope.type, .error = outcome.error()});
        };
        switch (envelope.type) {
            case MessageType::InitialSnapshot:
                record(replicator->ApplyInitialObjects(engine, envelope.payload));
                break;
            case MessageType::PhysicsBatch:
                record(replicator->ApplyPhysicsBatch(engine, envelope.payload));
                break;
            case MessageType::ClientHello:
            case MessageType::ServerWelcome:
            case MessageType::ClientInput:
                // Not client-bound: nothing was applied and nothing went wrong,
                // so the batch stays empty -- which is a complete success.
                break;
        }
        return applied;
    }

    // The transport half of a poll: a stream that desynchronised is a frame-level
    // failure -- the socket is closed, nothing after it can be trusted -- so the
    // error comes back as the return value. What each individual message did is
    // the ledger's business.
    auto PumpTcp(Engine& engine, MessageBatch& ledger) -> ErrorCode {
        uint8_t chunk[RECV_CHUNK_BYTES];
        while (true) {
            const RecvResult received = RecvSome(tcpSocket, std::span<uint8_t>(chunk));
            if (received.status == RecvStatus::Data) {
                auto appended = tcpStream.Append({chunk, received.bytes});
                if (!appended) {
                    Log("Net: {} — closing stream", appended.error().Format());
                    CloseSockets();
                    return ErrorCode(NetworkError::InvalidPayload);
                }
                if (received.bytes < sizeof chunk) {
                    break; // socket buffer likely drained
                }
                continue;
            }
            if (received.status == RecvStatus::Closed) {
                CloseSockets();
                return ErrorCode(NetworkError::ServerDisconnected);
            }
            if (received.status == RecvStatus::Failed) {
                CloseSockets();
                return ErrorCode(NetworkError::SocketError);
            }
            break; // blocked
        }

        // Extract every complete frame currently buffered.
        while (tcpStream.Size() >= 4) {
            auto length = PeekFrameLength(tcpStream.Data());
            if (!length) {
                Log("Net: {} — closing stream", length.error().Format());
                CloseSockets();
                return ErrorCode(NetworkError::InvalidPayload);
            }
            if (tcpStream.Size() < 4 + static_cast<size_t>(*length)) {
                break; // incomplete frame, wait for more bytes
            }
            auto payload = DecodeFrame(tcpStream.Data().first(4 + static_cast<size_t>(*length)));
            tcpStream.Consume(4 + static_cast<size_t>(*length));
            if (!payload) {
                Log("Net: {} — closing stream", payload.error().Format());
                CloseSockets();
                return ErrorCode(NetworkError::InvalidPayload);
            }
            auto envelope = DecodeEnvelope(*payload);
            if (!envelope) {
                ledger.EmplaceError(
                    MessageMiss {.channel = Channel::Tcp, .error = ErrorCode(NetworkError::InvalidPayload), .detail = envelope.error().Format()}
                );
                continue;
            }
            ledger.Merge(ApplyMessage(engine, *envelope, Channel::Tcp));
        }
        return {};
    }

    auto PumpUdp(Engine& engine, MessageBatch& ledger) -> ErrorCode {
        if (!udpSocket.IsOpen()) {
            return {};
        }
        uint8_t datagram[RECV_CHUNK_BYTES];
        while (true) {
            const RecvResult received = RecvSome(udpSocket, std::span<uint8_t>(datagram));
            if (received.status != RecvStatus::Data) {
                if (received.status == RecvStatus::Failed) {
                    return ErrorCode(NetworkError::SocketError);
                }
                break; // blocked or "closed": nothing more to read
            }

            auto payload = DecodeDatagram({datagram, received.bytes});
            if (!payload) {
                ledger.EmplaceError(
                    MessageMiss {.channel = Channel::Udp, .error = ErrorCode(NetworkError::InvalidPayload), .detail = payload.error().Format()}
                );
                continue; // datagrams are self-contained; keep draining
            }
            auto envelope = DecodeEnvelope(*payload);
            if (!envelope) {
                ledger.EmplaceError(
                    MessageMiss {.channel = Channel::Udp, .error = ErrorCode(NetworkError::InvalidPayload), .detail = envelope.error().Format()}
                );
                continue;
            }
            // Only the realtime channel's one message type is read from a
            // datagram; anything else on that socket is still ignored, not
            // quietly promoted to a message the stream is meant to carry.
            if (envelope->type == MessageType::PhysicsBatch) {
                isRealtimeReady = true;
                ledger.Merge(ApplyMessage(engine, *envelope, Channel::Udp));
            }
        }
        return {};
    }
};

// ============================================================================
// NetworkClient
// ============================================================================

NetworkClient::NetworkClient(): _impl(std::make_unique<Impl>()) {
}
NetworkClient::~NetworkClient()                                           = default;
NetworkClient::NetworkClient(NetworkClient&&) noexcept                    = default;
auto NetworkClient::operator=(NetworkClient&&) noexcept -> NetworkClient& = default;

auto NetworkClient::Connect(std::string_view host, uint16_t port, uint64_t userId, std::string_view token) noexcept
    -> std::expected<void, ErrorCode> {
    _impl->CloseSockets();

    auto endpoints = ResolveSocketEndpoints(host, port, SocketAddressFamily::IPv4, SocketType::Stream);
    if (!endpoints) {
        return std::unexpected(ErrorCode(NetworkError::ConnectionFailed));
    }

    Socket tcp;
    bool   connected = false;
    for (const SocketEndpoint& endpoint: *endpoints) {
        auto opened = OpenSocket(SocketType::Stream, endpoint.family);
        if (!opened) {
            continue;
        }
        auto connection = opened->Connect(endpoint, std::chrono::milliseconds(HANDSHAKE_TIMEOUT_MS));
        if (!connection) {
            continue;
        }
        auto noDelay = opened->SetNoDelay(true);
        if (!noDelay) {
            Log("Net: TCP_NODELAY could not be set (socket error {})", noDelay.error().nativeCode);
        }
        tcp                         = std::move(*opened);
        _impl->serverAddress        = endpoint;
        connected                   = true;
        break;
    }
    if (!connected) {
        return std::unexpected(ErrorCode(NetworkError::ConnectionFailed));
    }

    _impl->tcpSocket     = std::move(tcp);
    _impl->userId        = userId;
    _impl->token         = std::string(token);
    _impl->inputSequence = 0;

    // --- Handshake: ClientHello → ServerWelcome ------------------------------
    const ClientHello hello {.protocolVersion = PROTOCOL_VERSION, .userId = userId, .token = std::string(token)};
    auto              encodedHello = EncodeClientHello(hello);
    if (encodedHello) {
        auto frame = EncodeFrame(*encodedHello);
        if (frame) {
            if (!SendAll(_impl->tcpSocket, *frame, HANDSHAKE_TIMEOUT_MS)) {
                _impl->CloseSockets();
                return std::unexpected(ErrorCode(NetworkError::ConnectionFailed));
            }
        } else {
            Log("Net: failed to frame ClientHello — {}", frame.error().Format());
            _impl->CloseSockets();
            return std::unexpected(ErrorCode(NetworkError::HandshakeFailed));
        }
    } else {
        Log("Net: failed to encode ClientHello — {}", encodedHello.error().Format());
        _impl->CloseSockets();
        return std::unexpected(ErrorCode(NetworkError::HandshakeFailed));
    }

    Wire::Buffer handshakeStream {MAX_STREAM_FRAME_BYTES};
    auto         welcomeFrame = RecvExactFrame(_impl->tcpSocket, handshakeStream, HANDSHAKE_TIMEOUT_MS);
    if (!welcomeFrame) {
        Log("Net: no ServerWelcome — {}", welcomeFrame.error().Format());
        _impl->CloseSockets();
        return std::unexpected(ErrorCode(NetworkError::HandshakeTimeout));
    }
    auto welcome = DecodeServerWelcome(*welcomeFrame);
    if (!welcome) {
        Log("Net: bad ServerWelcome — {}", welcome.error().Format());
        _impl->CloseSockets();
        return std::unexpected(ErrorCode(NetworkError::HandshakeFailed));
    }

    // --- UDP: realtime channel -----------------------------------------------
    auto openedUdp = OpenSocket(SocketType::Datagram, _impl->serverAddress.family);
    if (!openedUdp) {
        _impl->CloseSockets();
        return std::unexpected(ErrorCode(NetworkError::SocketError));
    }
    Socket udp = std::move(*openedUdp);
    auto   nonblocking = udp.SetNonBlocking(true);
    if (!nonblocking) {
        _impl->CloseSockets();
        return std::unexpected(ErrorCode(NetworkError::SocketError));
    }

    SocketEndpoint udpAddress = _impl->serverAddress;
    udpAddress.port = welcome->realtimePort;
    auto udpConnection = udp.Connect(udpAddress, std::chrono::milliseconds(HANDSHAKE_TIMEOUT_MS));
    if (!udpConnection) {
        _impl->CloseSockets();
        return std::unexpected(ErrorCode(NetworkError::SocketError));
    }

    _impl->udpSocket       = std::move(udp);
    _impl->isConnected     = true;
    _impl->isRealtimeReady = true;
    return {};
}

void NetworkClient::Disconnect() noexcept {
    _impl->CloseSockets();
}

auto NetworkClient::PollEvents(Engine& engine) noexcept -> std::expected<void, ErrorCode> {
    if (!_impl->isConnected || !_impl->tcpSocket.IsOpen()) {
        return {};
    }
    // Both channels drain into one ledger, and every message that did not make
    // it is reported once, with the channel and the type that produced it. The
    // frame still fails on the first of them, which is what the public signature
    // promises; what changed is that the failures behind the first are no longer
    // thrown away with it.
    MessageBatch    applied;
    const ErrorCode tcpFailure = _impl->PumpTcp(engine, applied);
    const ErrorCode udpFailure = _impl->PumpUdp(engine, applied);

    applied.ForEachError([](const MessageMiss& miss) {
        const char* carrier = miss.channel == Channel::Udp ? "datagram" : "stream frame";
        if (miss.type.has_value()) {
            Log("Net: {} carrying {} was received but not applied — {}", carrier, *miss.type, miss.error);
            return;
        }
        Log("Net: {} carrying an undecodable message was dropped — {}: {}", carrier, miss.error, miss.detail);
    });

    if (tcpFailure) {
        return std::unexpected(tcpFailure);
    }
    if (udpFailure) {
        return std::unexpected(udpFailure);
    }
    if (const auto* first = applied.FirstError(); first != nullptr) {
        return std::unexpected(first->error);
    }
    return {};
}

void NetworkClient::SendInputs(bool forward, bool backward, bool left, bool right, bool jump, float yaw) noexcept {
    if (!_impl->isConnected || !_impl->udpSocket.IsOpen()) {
        return;
    }

    uint8_t moveFlags = 0;
    if (forward) {
        moveFlags |= 1;
    }
    if (backward) {
        moveFlags |= 2;
    }
    if (left) {
        moveFlags |= 4;
    }
    if (right) {
        moveFlags |= 8;
    }
    if (jump) {
        moveFlags |= 16;
    }

    const ClientInputMessage input {
        .userId = _impl->userId, .sequence = _impl->inputSequence++, .moveFlags = moveFlags, .yaw = yaw
    };
    auto payload = EncodeClientInput(input);
    if (!payload) {
        Log("Net: failed to encode input — {}", payload.error().Format());
        return;
    }
    auto datagram = EncodeDatagram(*payload);
    if (!datagram) {
        Log("Net: failed to frame input — {}", datagram.error().Format());
        return;
    }
    if (datagram->size() > MAX_SAFE_UDP_PAYLOAD) {
        Log("Net: input datagram of {} byte(s) exceeds the {} byte safe payload size", datagram->size(), MAX_SAFE_UDP_PAYLOAD);
        return;
    }

    auto sent = _impl->udpSocket.Send(*datagram);
    if (!sent) {
        Log("Net: input datagram send failed (socket error {})", sent.error().nativeCode);
    }
}

bool NetworkClient::IsConnected() const noexcept {
    return _impl->isConnected;
}

bool NetworkClient::IsRealtimeReady() const noexcept {
    return _impl->isRealtimeReady;
}

} // namespace ZHLN::Net
