// extensions/net/Network/NetworkClient.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
// NetworkClient — TCP handshake + framed stream, UDP realtime channel.
//
//   Connect:    non-blocking TCP connect (select-based timeout), ClientHello
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

#if defined(_WIN32)
// winsock2.h reaches <cstdlib> through winnt.h's intrinsic headers. Load
// libstdc++'s configuration with C++ linkage first, as the module interfaces do.
#include <cstddef>
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <Zahlen/Core/BatchResult.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

module ZHLN.Network;

namespace ZHLN::Net {

namespace {

constexpr int    HANDSHAKE_TIMEOUT_MS = 5000;
constexpr size_t RECV_CHUNK_BYTES     = 64 * 1024;

#if defined(_WIN32)
using SocketHandle              = SOCKET;
constexpr SocketHandle kInvalid = INVALID_SOCKET;

auto LastSocketError() -> int {
    return WSAGetLastError();
}

auto WouldBlockError() -> bool {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

auto InterruptedError() -> bool {
    return WSAGetLastError() == WSAEINTR;
}

auto CloseSocket(SocketHandle& fd) -> void {
    if (fd != kInvalid) {
        closesocket(fd);
    }
    fd = kInvalid;
}
#else
using SocketHandle              = int;
constexpr SocketHandle kInvalid = -1;

auto LastSocketError() -> int {
    return errno;
}

auto WouldBlockError() -> bool {
#if EAGAIN != EWOULDBLOCK
    return errno == EAGAIN || errno == EWOULDBLOCK;
#else
    return errno == EAGAIN; // EWOULDBLOCK is the same value on this platform
#endif
}

auto InterruptedError() -> bool {
    return errno == EINTR;
}

auto CloseSocket(SocketHandle& fd) -> void {
    if (fd != kInvalid) {
        close(fd);
    }
    fd = kInvalid;
}
#endif

auto SetNonBlocking(SocketHandle fd) -> bool {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// Waits until the socket is readable/writable or the timeout expires.
auto WaitForSocket(SocketHandle fd, bool writable, int timeoutMs) -> bool {
    if (fd == kInvalid) {
        return false;
    }
    fd_set      set;
    struct timeval tv;
    tv.tv_sec  = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    FD_ZERO(&set);
    FD_SET(fd, &set);
#if defined(_WIN32)
    const int ready = select(0, writable ? nullptr : &set, writable ? &set : nullptr, nullptr, &tv);
#else
    if (static_cast<long>(fd) >= FD_SETSIZE) {
        return false; // select() cannot handle this descriptor
    }
    const int ready = select(fd + 1, writable ? nullptr : &set, writable ? &set : nullptr, nullptr, &tv);
#endif
    return ready > 0;
}

auto SendAll(SocketHandle fd, std::span<const uint8_t> bytes, int timeoutMs) -> bool {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const int n = static_cast<int>(
            send(fd, reinterpret_cast<const char*>(bytes.data() + sent), static_cast<int>(bytes.size() - sent), 0)
        );
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && InterruptedError()) {
            continue;
        }
        if (n < 0 && WouldBlockError()) {
            if (!WaitForSocket(fd, true, timeoutMs)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

enum class RecvStatus { Data, Closed, Blocked, Failed };

auto RecvSome(SocketHandle fd, uint8_t* destination, size_t capacity, size_t& outBytes) -> RecvStatus {
    while (true) {
        const int n = static_cast<int>(recv(fd, reinterpret_cast<char*>(destination), static_cast<int>(capacity), 0));
        if (n > 0) {
            outBytes = static_cast<size_t>(n);
            return RecvStatus::Data;
        }
        if (n == 0) {
            return RecvStatus::Closed;
        }
        if (InterruptedError()) {
            continue;
        }
        if (WouldBlockError()) {
            return RecvStatus::Blocked;
        }
        return RecvStatus::Failed;
    }
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
auto RecvExactFrame(SocketHandle fd, Wire::Buffer& stream, int timeoutMs) -> Wire::Result<std::vector<uint8_t>> {
    uint8_t chunk[4096];
    while (stream.Size() < 4) {
        if (!WaitForSocket(fd, false, timeoutMs)) {
            return std::unexpected(Wire::MakeFailure(Wire::WireError::InvalidFrame, "timed out waiting for the frame header"));
        }
        size_t got = 0;
        switch (RecvSome(fd, chunk, sizeof chunk, got)) {
            case RecvStatus::Data: {
                auto appended = stream.Append({chunk, got});
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
                                                         std::format("socket error {} while reading the frame header", LastSocketError())));
        }
    }

    auto length = PeekFrameLength(stream.Data());
    if (!length) {
        return std::unexpected(length.error());
    }

    while (stream.Size() < 4 + static_cast<size_t>(*length)) {
        if (!WaitForSocket(fd, false, timeoutMs)) {
            return std::unexpected(Wire::MakeFailure(Wire::WireError::InvalidFrame, "timed out waiting for the frame body"));
        }
        size_t got = 0;
        switch (RecvSome(fd, chunk, sizeof chunk, got)) {
            case RecvStatus::Data: {
                auto appended = stream.Append({chunk, got});
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
                                                         std::format("socket error {} while reading the frame body", LastSocketError())));
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
    SocketHandle tcpSocket       = kInvalid;
    SocketHandle udpSocket       = kInvalid;
    bool         isConnected     = false;
    bool         isRealtimeReady = false;

    uint64_t    userId = 0;
    std::string token;

    sockaddr_in serverAddress {};
    uint16_t    realtimePort  = 0;
    uint32_t    inputSequence = 0;

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
        CloseSocket(tcpSocket);
        CloseSocket(udpSocket);
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
            size_t got = 0;
            const RecvStatus status = RecvSome(tcpSocket, chunk, sizeof chunk, got);
            if (status == RecvStatus::Data) {
                auto appended = tcpStream.Append({chunk, got});
                if (!appended) {
                    Log("Net: {} — closing stream", appended.error().Format());
                    CloseSockets();
                    return ErrorCode(NetworkError::InvalidPayload);
                }
                if (got < sizeof chunk) {
                    break; // socket buffer likely drained
                }
                continue;
            }
            if (status == RecvStatus::Closed) {
                CloseSockets();
                return ErrorCode(NetworkError::ServerDisconnected);
            }
            if (status == RecvStatus::Failed) {
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
        if (udpSocket == kInvalid) {
            return {};
        }
        uint8_t datagram[RECV_CHUNK_BYTES];
        while (true) {
            size_t got = 0;
            const RecvStatus status = RecvSome(udpSocket, datagram, sizeof datagram, got);
            if (status != RecvStatus::Data) {
                if (status == RecvStatus::Failed) {
                    return ErrorCode(NetworkError::SocketError);
                }
                break; // blocked or "closed": nothing more to read
            }

            auto payload = DecodeDatagram({datagram, got});
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
#if defined(_WIN32)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return std::unexpected(ErrorCode(NetworkError::SocketError));
    }
#endif
    _impl->CloseSockets();

    // --- TCP: non-blocking connect with timeout -----------------------------
    SocketHandle tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp == kInvalid) {
        return std::unexpected(ErrorCode(NetworkError::SocketError));
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port   = htons(port);
    if (inet_pton(AF_INET, std::string(host).c_str(), &address.sin_addr) != 1) {
        CloseSocket(tcp);
        return std::unexpected(ErrorCode(NetworkError::ConnectionFailed));
    }

    if (!SetNonBlocking(tcp)) {
        CloseSocket(tcp);
        return std::unexpected(ErrorCode(NetworkError::SocketError));
    }

    const int rc = connect(tcp, reinterpret_cast<sockaddr*>(&address), sizeof address);
    if (rc != 0) {
#if defined(_WIN32)
        const bool inProgress = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool inProgress = errno == EINPROGRESS;
#endif
        if (!inProgress) {
            CloseSocket(tcp);
            return std::unexpected(ErrorCode(NetworkError::ConnectionFailed));
        }
        if (!WaitForSocket(tcp, true, HANDSHAKE_TIMEOUT_MS)) {
            CloseSocket(tcp);
            return std::unexpected(ErrorCode(NetworkError::ConnectionFailed));
        }
        int soError = 0;
#if defined(_WIN32)
        int soLen = sizeof soError;
#else
        socklen_t soLen = sizeof soError;
#endif
        getsockopt(tcp, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soLen);
        if (soError != 0) {
            CloseSocket(tcp);
            return std::unexpected(ErrorCode(NetworkError::ConnectionFailed));
        }
    }

    int nodelay = 1;
    setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);

    _impl->tcpSocket       = tcp;
    _impl->serverAddress   = address;
    _impl->userId          = userId;
    _impl->token           = std::string(token);
    _impl->inputSequence   = 0;

    // --- Handshake: ClientHello → ServerWelcome ------------------------------
    const ClientHello hello {.protocolVersion = PROTOCOL_VERSION, .userId = userId, .token = std::string(token)};
    auto              encodedHello = EncodeClientHello(hello);
    if (encodedHello) {
        auto frame = EncodeFrame(*encodedHello);
        if (frame) {
            if (!SendAll(tcp, *frame, HANDSHAKE_TIMEOUT_MS)) {
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
    auto         welcomeFrame = RecvExactFrame(tcp, handshakeStream, HANDSHAKE_TIMEOUT_MS);
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
    SocketHandle udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == kInvalid) {
        _impl->CloseSockets();
        return std::unexpected(ErrorCode(NetworkError::SocketError));
    }
    if (!SetNonBlocking(udp)) {
        CloseSocket(udp);
        _impl->CloseSockets();
        return std::unexpected(ErrorCode(NetworkError::SocketError));
    }

    sockaddr_in udpAddress = address;
    udpAddress.sin_port     = htons(welcome->realtimePort);
    if (connect(udp, reinterpret_cast<sockaddr*>(&udpAddress), sizeof udpAddress) != 0) {
        CloseSocket(udp);
        _impl->CloseSockets();
        return std::unexpected(ErrorCode(NetworkError::SocketError));
    }

    _impl->udpSocket       = udp;
    _impl->realtimePort    = welcome->realtimePort;
    _impl->isConnected     = true;
    _impl->isRealtimeReady = true;
    return {};
}

void NetworkClient::Disconnect() noexcept {
    _impl->CloseSockets();
}

auto NetworkClient::PollEvents(Engine& engine) noexcept -> std::expected<void, ErrorCode> {
    if (!_impl->isConnected || _impl->tcpSocket == kInvalid) {
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
    if (!_impl->isConnected || _impl->udpSocket == kInvalid) {
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

    (void) send(_impl->udpSocket, reinterpret_cast<const char*>(datagram->data()), static_cast<int>(datagram->size()), 0);
}

bool NetworkClient::IsConnected() const noexcept {
    return _impl->isConnected;
}

bool NetworkClient::IsRealtimeReady() const noexcept {
    return _impl->isRealtimeReady;
}

} // namespace ZHLN::Net
