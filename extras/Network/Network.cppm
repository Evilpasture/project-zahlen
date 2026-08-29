// extras/Network/Network.cppm
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
// ZHLN.Network — client networking stack for the Zahlen engine.
//
// Dependency-free: serialization, framing, integrity checking and block
// compression all live in ZHLN.Wire; only OS sockets are used here. The
// module is fully exception-free (-fno-exceptions / -fno-rtti clean) and
// reports every failure through std::expected.
//
// Transport layout (see extras/Network/WireProtocol.md for the full spec):
//   * TCP — framed message stream: handshake, snapshot bursts
//   * UDP — realtime datagrams: physics batches (in), inputs (out)
// ============================================================================

module;

// --- Global Module Fragment: External non-modular includes only ---
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Common.h>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module ZHLN.Network;

export import ZHLN.Wire;

export namespace ZHLN::Net {

// ============================================================================
// Protocol & Quantization Constants
// ============================================================================

inline constexpr uint16_t DEFAULT_GAME_PORT    = 5555;
inline constexpr size_t   MAX_SAFE_UDP_PAYLOAD = 1200;

/// Position/velocity quantization: fixed-point with 1/256 m resolution over
/// the full int32 range (~±8.4 million metres).
inline constexpr double POSITION_SCALE    = 256.0;
inline constexpr float INV_POSITION_SCALE = 1.0f / 256.0f;

/// Rotation quantization: unit quaternion components over int16.
inline constexpr float QUAT_SCALE     = 32767.0f;
inline constexpr float INV_QUAT_SCALE = 1.0f / 32767.0f;

// ============================================================================
// Error Codes (API level — placeholder-free messages; wire-level errors use
// ZHLN::Wire::WireError and carry formatted context in ZHLN::Wire::Failure)
// ============================================================================

enum class NetworkError : uint8_t {
    SocketError[[= ZHLN::Reflect::Description<"Socket operation failed"> {}]] = 1,
    ConnectionFailed[[= ZHLN::Reflect::Description<"Failed to establish server connection"> {}]],
    HandshakeFailed[[= ZHLN::Reflect::Description<"Server handshake or token verification failed"> {}]],
    HandshakeTimeout[[= ZHLN::Reflect::Description<"Server did not respond to the handshake in time"> {}]],
    ServerDisconnected[[= ZHLN::Reflect::Description<"Server closed the connection"> {}]],
    ReplicationFailed[[= ZHLN::Reflect::Description<"Failed to apply a replicated server update"> {}]],
    InvalidPayload[[= ZHLN::Reflect::Description<"Payload structure does not match expected protocol schema"> {}]]
};

// ============================================================================
// Protocol Messages (wire schema v2 — see WireProtocol.md)
// ============================================================================

enum class MessageType : uint8_t {
    ClientHello[[= ZHLN::Reflect::Description<"Client to server: identity and auth token"> {}]] = 1,
    ServerWelcome[[= ZHLN::Reflect::Description<"Server to client: session acceptance and realtime port"> {}]] = 2,
    InitialSnapshot[[= ZHLN::Reflect::Description<"Server to client: full world snapshot"> {}]] = 3,
    PhysicsBatch[[= ZHLN::Reflect::Description<"Server to client: realtime physics state batch"> {}]] = 4,
    ClientInput[[= ZHLN::Reflect::Description<"Client to server: per-frame movement input"> {}]] = 5
};

struct ClientHello {
    uint32_t    protocolVersion [[= ZHLN::Reflect::Description<"Wire protocol version the client speaks"> {}]];
    uint64_t    userId [[= ZHLN::Reflect::Description<"Player identity issued by the server operator"> {}]];
    std::string token [[= ZHLN::Reflect::Description<"Shared secret used for this login session"> {}]];
};

struct ServerWelcome {
    uint32_t serverTick [[= ZHLN::Reflect::Description<"Server simulation tick at handshake time"> {}]];
    uint16_t realtimePort [[= ZHLN::Reflect::Description<"UDP port serving realtime physics and input traffic"> {}]];
    uint8_t  tickRateHz [[= ZHLN::Reflect::Description<"Authoritative server tick rate in hertz"> {},
                         = ZHLN::Wire::Range<1, 240> {}]];
};

struct ObjectSnapshot {
    uint64_t  uid [[= ZHLN::Reflect::Description<"Server-assigned stable object identity"> {}]];
    JPH::Vec3 position [[= ZHLN::Reflect::Description<"World-space position, quantized to 1/256 m"> {}]];
    JPH::Vec3 size [[= ZHLN::Reflect::Description<"Axis-aligned object extents, quantized to 1/256 m"> {}]];
};

struct [[= ZHLN::Wire::Version<2> {}]] InitialSnapshotMessage {
    uint32_t                    serverTick [[= ZHLN::Reflect::Description<"Server simulation tick of this snapshot"> {}]];
    std::vector<ObjectSnapshot> objects [[= ZHLN::Reflect::Description<"Every replicated object in the world"> {}]];
};

struct PhysicsBodyState {
    uint64_t  uid [[= ZHLN::Reflect::Description<"Server-assigned stable object identity"> {}]];
    JPH::Vec3 position [[= ZHLN::Reflect::Description<"World-space position, quantized to 1/256 m"> {}]];
    JPH::Quat rotation [[= ZHLN::Reflect::Description<"Sign-canonical orientation, quantized to 1/32767 per component"> {}]];
    JPH::Vec3 velocity [[= ZHLN::Reflect::Description<"Linear velocity in metres per second"> {}]];
};

struct [[= ZHLN::Wire::Version<2> {}]] PhysicsBatchMessage {
    uint32_t                      serverTick [[= ZHLN::Reflect::Description<"Server simulation tick of this batch"> {}]];
    std::vector<PhysicsBodyState> bodies [[= ZHLN::Reflect::Description<"One entry per moving replicated body"> {}]];
};

struct ClientInputMessage {
    uint64_t userId [[= ZHLN::Reflect::Description<"Player identity issued by the server operator"> {}]];
    uint32_t sequence [[= ZHLN::Reflect::Description<"Monotonically increasing input counter"> {}]];
    uint8_t  moveFlags [[= ZHLN::Reflect::Description<"Movement bitfield: 1=forward 2=backward 4=left 8=right 16=jump"> {},
                        = ZHLN::Wire::Range<0, 31> {}]];
    float    yaw [[= ZHLN::Reflect::Description<"Camera yaw in degrees, clockwise positive"> {},
               = ZHLN::Wire::Range<-1000.0f, 1000.0f> {}]];
};

// ============================================================================
// Stream Frames — length-prefixed, CRC32-protected, optionally compressed
//
//   TCP frame:   [u32 BE length][frame body]        (length covers the body)
//   UDP datagram:                [frame body]
//   frame body:  [u8 flags][u32 LE rawLen if compressed][payload][u32 LE CRC32]
//                CRC32 is computed over the uncompressed payload.
// ============================================================================

inline constexpr uint8_t FRAME_FLAG_COMPRESSED  = 0x01;
inline constexpr size_t  MAX_STREAM_FRAME_BYTES = 128 * 1024 * 1024;
inline constexpr size_t  COMPRESSION_MIN_BYTES  = 1024;
inline constexpr uint8_t PROTOCOL_VERSION       = 2;

/// Reads the big-endian frame length from the first 4 bytes of a TCP stream.
[[nodiscard]] auto PeekFrameLength(std::span<const uint8_t> streamPrefix) -> Wire::Result<uint32_t>;

/// Encodes payload as a TCP stream frame. Compression is applied when it
/// actually shrinks the payload.
[[nodiscard]] auto EncodeFrame(std::span<const uint8_t> payload) -> Wire::Result<std::vector<uint8_t>>;

/// Encodes payload as a UDP datagram frame (same body, no length prefix).
[[nodiscard]] auto EncodeDatagram(std::span<const uint8_t> payload) -> Wire::Result<std::vector<uint8_t>>;

/// Decodes a complete TCP frame (length prefix included, exact size expected).
[[nodiscard]] auto DecodeFrame(std::span<const uint8_t> frame) -> Wire::Result<std::vector<uint8_t>>;

/// Decodes a UDP datagram frame (same body layout, no length prefix).
[[nodiscard]] auto DecodeDatagram(std::span<const uint8_t> datagram) -> Wire::Result<std::vector<uint8_t>>;

// ============================================================================
// Message Envelope:  [u8 'Z'][u8 'W'][u8 version][u8 type][payload...]
// ============================================================================

struct MessageEnvelope {
    MessageType           type {};
    std::vector<uint8_t> payload {};
};

[[nodiscard]] auto EncodeEnvelope(MessageType type, std::span<const uint8_t> payload) -> Wire::Result<std::vector<uint8_t>>;
[[nodiscard]] auto DecodeEnvelope(std::span<const uint8_t> bytes) -> Wire::Result<MessageEnvelope>;

// -- Typed per-message encode/decode (non-template wrappers: every ZHLN.Wire
// -- template instantiation stays inside this module) -------------------------

[[nodiscard]] auto EncodeClientHello(const ClientHello& message) -> Wire::Result<std::vector<uint8_t>>;
[[nodiscard]] auto DecodeClientHello(std::span<const uint8_t> bytes) -> Wire::Result<ClientHello>;
[[nodiscard]] auto EncodeServerWelcome(const ServerWelcome& message) -> Wire::Result<std::vector<uint8_t>>;
[[nodiscard]] auto DecodeServerWelcome(std::span<const uint8_t> bytes) -> Wire::Result<ServerWelcome>;
[[nodiscard]] auto EncodeInitialSnapshot(const InitialSnapshotMessage& message) -> Wire::Result<std::vector<uint8_t>>;
[[nodiscard]] auto DecodeInitialSnapshot(std::span<const uint8_t> bytes) -> Wire::Result<InitialSnapshotMessage>;
[[nodiscard]] auto EncodePhysicsBatch(const PhysicsBatchMessage& message) -> Wire::Result<std::vector<uint8_t>>;
[[nodiscard]] auto DecodePhysicsBatch(std::span<const uint8_t> bytes) -> Wire::Result<PhysicsBatchMessage>;
[[nodiscard]] auto EncodeClientInput(const ClientInputMessage& message) -> Wire::Result<std::vector<uint8_t>>;
[[nodiscard]] auto DecodeClientInput(std::span<const uint8_t> bytes) -> Wire::Result<ClientInputMessage>;

// ============================================================================
// Quantization Codecs for Jolt math types (ZHLN::Wire::Codec specializations).
// Declared before any use: every instantiation of ZHLN.Wire templates inside
// this module must observe them.
// ============================================================================

} // namespace ZHLN::Net

template <>
struct ZHLN::Wire::Codec<JPH::Vec3> {
    // Quantized wire unit: int32 over POSITION_SCALE (1/256 m) → ±8,388,607 m.
    static constexpr double WorldMax = 2147483647.0 / ZHLN::Net::POSITION_SCALE;

    static auto Encode(const JPH::Vec3& value, ZHLN::Wire::Writer& writer) -> ZHLN::Wire::Result<void> {
        const float components[3] = {value.GetX(), value.GetY(), value.GetZ()};
        for (const float component: components) {
            const double meters = static_cast<double>(component);
            if (!std::isfinite(component) || meters < -WorldMax || meters > WorldMax) {
                return std::unexpected(
                    writer.Fail(ZHLN::Wire::WireError::ValueOutOfRange, meters, -WorldMax, WorldMax));
            }
            const double scaled  = std::round(meters * ZHLN::Net::POSITION_SCALE);
            const double clamped = std::clamp(scaled, -2147483647.0, 2147483647.0);
            const auto   res     = writer.Put(static_cast<int32_t>(clamped));
            if (!res) {
                return res;
            }
        }
        return {};
    }

    static auto Decode(JPH::Vec3& value, ZHLN::Wire::Reader& reader) -> ZHLN::Wire::Result<void> {
        int32_t quantized[3] = {};
        for (size_t index = 0; index < 3; ++index) {
            const ZHLN::Wire::IndexPathScope scope(reader.Path(), index);
            const auto                              res = reader.Get(quantized[index]);
            if (!res) {
                return res;
            }
        }
        value = JPH::Vec3(static_cast<float>(quantized[0]) * ZHLN::Net::INV_POSITION_SCALE,
                          static_cast<float>(quantized[1]) * ZHLN::Net::INV_POSITION_SCALE,
                          static_cast<float>(quantized[2]) * ZHLN::Net::INV_POSITION_SCALE);
        return {};
    }
};

template <>
struct ZHLN::Wire::Codec<JPH::Quat> {
    static auto Encode(const JPH::Quat& value, ZHLN::Wire::Writer& writer) -> ZHLN::Wire::Result<void> {
        // Canonical form: the representative of the ±q pair with w >= 0, so
        // the same orientation always produces identical bytes.
        float x = value.GetX();
        float y = value.GetY();
        float z = value.GetZ();
        float w = value.GetW();
        if (w < 0.0f) {
            x = -x;
            y = -y;
            z = -z;
            w = -w;
        }
        const float components[4] = {x, y, z, w};
        for (const float component: components) {
            if (!std::isfinite(component)) {
                return std::unexpected(
                    writer.Fail(ZHLN::Wire::WireError::ValueOutOfRange, static_cast<double>(component), -1.0, 1.0));
            }
            // Clamp instead of rejecting: normalized quaternions can overshoot
            // ±1 by one ULP.
            const float scaled  = std::round(std::clamp(component, -1.0f, 1.0f) * ZHLN::Net::QUAT_SCALE);
            const auto  res     = writer.Put(static_cast<int16_t>(scaled));
            if (!res) {
                return res;
            }
        }
        return {};
    }

    static auto Decode(JPH::Quat& value, ZHLN::Wire::Reader& reader) -> ZHLN::Wire::Result<void> {
        int16_t quantized[4] = {};
        for (size_t index = 0; index < 4; ++index) {
            const ZHLN::Wire::IndexPathScope scope(reader.Path(), index);
            const auto                              res = reader.Get(quantized[index]);
            if (!res) {
                return res;
            }
        }
        value = JPH::Quat(static_cast<float>(quantized[0]) * ZHLN::Net::INV_QUAT_SCALE,
                          static_cast<float>(quantized[1]) * ZHLN::Net::INV_QUAT_SCALE,
                          static_cast<float>(quantized[2]) * ZHLN::Net::INV_QUAT_SCALE,
                          static_cast<float>(quantized[3]) * ZHLN::Net::INV_QUAT_SCALE)
                    .Normalized();
        return {};
    }
};

export namespace ZHLN::Net {

// ============================================================================
// Frame & envelope codec implementation
// ============================================================================

namespace FrameDetail {

using Wire::Result;

inline auto Fail(Wire::WireError error, auto&&... args) -> Wire::Failure {
    return Wire::MakeFailure(error, static_cast<decltype(args)>(args)...);
}

inline auto PutBE32(std::vector<uint8_t>& out, uint32_t value) -> void {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
}

inline auto PutLE32(std::vector<uint8_t>& out, uint32_t value) -> void {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

inline auto ReadBE32(std::span<const uint8_t> bytes) -> uint32_t {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16)
           | (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

inline auto ReadLE32(std::span<const uint8_t> bytes) -> uint32_t {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8)
           | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

/// Shared frame-body decoding: [flags][rawLen | compressed payload][crc32].
inline auto DecodeFrameBody(std::span<const uint8_t> body) -> Result<std::vector<uint8_t>> {
    if (body.size() < 5) { // flags byte + trailing CRC32
        return std::unexpected(
            Fail(Wire::WireError::InvalidFrame, std::format("frame body of {} byte(s) is smaller than flags + CRC32", body.size())));
    }
    const uint8_t flags = body[0];
    if ((flags & ~FRAME_FLAG_COMPRESSED) != 0) {
        return std::unexpected(Fail(Wire::WireError::InvalidFrame, std::format("unknown frame flags 0x{:02x}", flags)));
    }
    if ((flags & FRAME_FLAG_COMPRESSED) != 0) {
        if (body.size() < 9) { // flags + rawLen + crc32
            return std::unexpected(Fail(Wire::WireError::InvalidFrame, "compressed frame is missing its raw length"));
        }
        const uint32_t            rawLength     = ReadLE32(body.subspan(1, 4));
        const std::span<const uint8_t> compressed    = body.subspan(5, body.size() - 9);
        const uint32_t            expectedCrc   = ReadLE32(body.subspan(body.size() - 4, 4));
        if (rawLength > MAX_STREAM_FRAME_BYTES) {
            return std::unexpected(Fail(Wire::WireError::FrameTooLarge, rawLength, MAX_STREAM_FRAME_BYTES));
        }
        auto raw = Wire::Compression::Decompress(compressed, rawLength);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        if (raw->size() != rawLength) {
            return std::unexpected(Fail(Wire::WireError::InvalidFrame,
                                        std::format("decompressed {} byte(s) but the frame announced {}", raw->size(), rawLength)));
        }
        const uint32_t computedCrc = Wire::Checksum::Crc32(*raw);
        if (computedCrc != expectedCrc) {
            return std::unexpected(Fail(Wire::WireError::ChecksumMismatch, computedCrc, expectedCrc));
        }
        return raw;
    }

    const std::span<const uint8_t> payload      = body.subspan(1, body.size() - 5);
    const uint32_t            expectedCrc = ReadLE32(body.subspan(body.size() - 4, 4));
    const uint32_t            computedCrc = Wire::Checksum::Crc32(payload);
    if (computedCrc != expectedCrc) {
        return std::unexpected(Fail(Wire::WireError::ChecksumMismatch, computedCrc, expectedCrc));
    }
    return std::vector<uint8_t>(payload.begin(), payload.end());
}

} // namespace FrameDetail

auto PeekFrameLength(std::span<const uint8_t> streamPrefix) -> Wire::Result<uint32_t> {
    if (streamPrefix.size() < 4) {
        return std::unexpected(FrameDetail::Fail(Wire::WireError::InvalidFrame, "need at least 4 bytes to read a frame length"));
    }
    const uint32_t length = FrameDetail::ReadBE32(streamPrefix.subspan(0, 4));
    if (length < 5 || length > MAX_STREAM_FRAME_BYTES) {
        return std::unexpected(FrameDetail::Fail(Wire::WireError::FrameTooLarge, length, MAX_STREAM_FRAME_BYTES));
    }
    return length;
}

namespace FrameDetail {

/// Shared frame-body encoding: [flags][rawLen | compressed payload][crc32].
/// CRC32 is computed over the uncompressed payload.
inline auto EncodeFrameBody(std::span<const uint8_t> payload) -> Result<std::vector<uint8_t>> {
    uint8_t              flags = 0;
    std::vector<uint8_t> body; // everything between the flags byte and the CRC32

    if (payload.size() >= COMPRESSION_MIN_BYTES) {
        auto compressed = Wire::Compression::Compress(payload, MAX_STREAM_FRAME_BYTES);
        if (compressed && compressed->size() + 4 + 32 < payload.size()) {
            flags |= FRAME_FLAG_COMPRESSED;
            body.reserve(compressed->size() + 4);
            PutLE32(body, static_cast<uint32_t>(payload.size())); // announced raw length
            body.insert(body.end(), compressed->begin(), compressed->end());
        }
    }
    if ((flags & FRAME_FLAG_COMPRESSED) == 0) {
        body.reserve(payload.size());
        body.insert(body.end(), payload.begin(), payload.end());
    }

    std::vector<uint8_t> frameBody;
    frameBody.reserve(body.size() + 5);
    frameBody.push_back(flags);
    frameBody.insert(frameBody.end(), body.begin(), body.end());
    PutLE32(frameBody, Wire::Checksum::Crc32(payload));
    return frameBody;
}

} // namespace FrameDetail

auto EncodeFrame(std::span<const uint8_t> payload) -> Wire::Result<std::vector<uint8_t>> {
    auto body = FrameDetail::EncodeFrameBody(payload);
    if (!body) {
        return std::unexpected(body.error());
    }
    std::vector<uint8_t> frame;
    frame.reserve(body->size() + 4);
    FrameDetail::PutBE32(frame, static_cast<uint32_t>(body->size()));
    frame.insert(frame.end(), body->begin(), body->end());
    return frame;
}

auto EncodeDatagram(std::span<const uint8_t> payload) -> Wire::Result<std::vector<uint8_t>> {
    return FrameDetail::EncodeFrameBody(payload);
}

auto DecodeFrame(std::span<const uint8_t> frame) -> Wire::Result<std::vector<uint8_t>> {
    const auto length = PeekFrameLength(frame);
    if (!length) {
        return std::unexpected(length.error());
    }
    if (frame.size() != static_cast<size_t>(*length) + 4) {
        return std::unexpected(FrameDetail::Fail(Wire::WireError::FrameLengthMismatch, *length + 4ull, frame.size()));
    }
    return FrameDetail::DecodeFrameBody(frame.subspan(4));
}

auto DecodeDatagram(std::span<const uint8_t> datagram) -> Wire::Result<std::vector<uint8_t>> {
    return FrameDetail::DecodeFrameBody(datagram);
}

auto EncodeEnvelope(MessageType type, std::span<const uint8_t> payload) -> Wire::Result<std::vector<uint8_t>> {
    std::vector<uint8_t> bytes;
    bytes.reserve(payload.size() + 4);
    bytes.push_back('Z');
    bytes.push_back('W');
    bytes.push_back(PROTOCOL_VERSION);
    bytes.push_back(static_cast<uint8_t>(type));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

auto DecodeEnvelope(std::span<const uint8_t> bytes) -> Wire::Result<MessageEnvelope> {
    if (bytes.size() < 4) {
        return std::unexpected(FrameDetail::Fail(
            Wire::WireError::InvalidFrame, std::format("envelope of {} byte(s) is smaller than its 4 byte header", bytes.size())));
    }
    if (bytes[0] != 'Z' || bytes[1] != 'W') {
        return std::unexpected(FrameDetail::Fail(Wire::WireError::InvalidFrame, "envelope magic bytes are not 'ZW'"));
    }
    if (bytes[2] != PROTOCOL_VERSION) {
        return std::unexpected(FrameDetail::Fail(Wire::WireError::ProtocolVersionMismatch, bytes[2], PROTOCOL_VERSION));
    }
    const uint8_t rawType = bytes[3];
    if (!ZHLN::Reflect::EnumHasValue<MessageType>(rawType)) {
        return std::unexpected(FrameDetail::Fail(Wire::WireError::UnknownMessageType, rawType, PROTOCOL_VERSION));
    }
    MessageEnvelope envelope;
    envelope.type    = static_cast<MessageType>(rawType);
    envelope.payload = std::vector<uint8_t>(bytes.begin() + 4, bytes.end());
    return envelope;
}

// -- Typed message codecs ----------------------------------------------------

namespace MessageDetail {

template <typename T>
auto EncodeMessage(MessageType type, const T& message) -> Wire::Result<std::vector<uint8_t>> {
    auto payload = Wire::Encode(message);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    return EncodeEnvelope(type, *payload);
}

template <typename T>
auto DecodeMessage(MessageType expected, std::span<const uint8_t> bytes) -> Wire::Result<T> {
    auto envelope = DecodeEnvelope(bytes);
    if (!envelope) {
        return std::unexpected(envelope.error());
    }
    if (envelope->type != expected) {
        return std::unexpected(FrameDetail::Fail(
            Wire::WireError::InvalidFrame,
            std::format("expected message type {}, received {}", ZHLN::Reflect::EnumToString(expected),
                        ZHLN::Reflect::EnumToString(envelope->type))));
    }
    return Wire::Decode<T>(envelope->payload);
}

} // namespace MessageDetail

auto EncodeClientHello(const ClientHello& message) -> Wire::Result<std::vector<uint8_t>> {
    return MessageDetail::EncodeMessage(MessageType::ClientHello, message);
}

auto DecodeClientHello(std::span<const uint8_t> bytes) -> Wire::Result<ClientHello> {
    return MessageDetail::DecodeMessage<ClientHello>(MessageType::ClientHello, bytes);
}

auto EncodeServerWelcome(const ServerWelcome& message) -> Wire::Result<std::vector<uint8_t>> {
    return MessageDetail::EncodeMessage(MessageType::ServerWelcome, message);
}

auto DecodeServerWelcome(std::span<const uint8_t> bytes) -> Wire::Result<ServerWelcome> {
    return MessageDetail::DecodeMessage<ServerWelcome>(MessageType::ServerWelcome, bytes);
}

auto EncodeInitialSnapshot(const InitialSnapshotMessage& message) -> Wire::Result<std::vector<uint8_t>> {
    return MessageDetail::EncodeMessage(MessageType::InitialSnapshot, message);
}

auto DecodeInitialSnapshot(std::span<const uint8_t> bytes) -> Wire::Result<InitialSnapshotMessage> {
    return MessageDetail::DecodeMessage<InitialSnapshotMessage>(MessageType::InitialSnapshot, bytes);
}

auto EncodePhysicsBatch(const PhysicsBatchMessage& message) -> Wire::Result<std::vector<uint8_t>> {
    return MessageDetail::EncodeMessage(MessageType::PhysicsBatch, message);
}

auto DecodePhysicsBatch(std::span<const uint8_t> bytes) -> Wire::Result<PhysicsBatchMessage> {
    return MessageDetail::DecodeMessage<PhysicsBatchMessage>(MessageType::PhysicsBatch, bytes);
}

auto EncodeClientInput(const ClientInputMessage& message) -> Wire::Result<std::vector<uint8_t>> {
    return MessageDetail::EncodeMessage(MessageType::ClientInput, message);
}

auto DecodeClientInput(std::span<const uint8_t> bytes) -> Wire::Result<ClientInputMessage> {
    return MessageDetail::DecodeMessage<ClientInputMessage>(MessageType::ClientInput, bytes);
}

// ============================================================================
// ECS Network Components
// ============================================================================

struct NetworkIdentityComponent {
    uint64_t serverUID    = 0;
    bool     isLocalOwner = false;
};

struct NetworkInterpolationComponent {
    JPH::Vec3 targetPosition     = JPH::Vec3::sZero();
    JPH::Quat targetRotation     = JPH::Quat::sIdentity();
    JPH::Vec3 linearVelocity     = JPH::Vec3::sZero();
    float     interpolationSpeed = 20.0f;
};

// ============================================================================
// Public Subsystem API
// ============================================================================

} // namespace ZHLN::Net

// ClientReplicator is defined here (module linkage, not exported) so both
// implementation units of ZHLN.Network see the complete type; the method
// bodies live in NetworkReplicator.cpp.
namespace ZHLN::Net {

class ClientReplicator {
  public:
    HashMap<uint64_t, Entity> uidToEntityMap;

    auto ApplyInitialObjects(Engine& engine, std::span<const uint8_t> payload) noexcept -> std::expected<void, Error>;
    auto ApplyPhysicsBatch(Engine& engine, std::span<const uint8_t> payload) noexcept -> std::expected<void, Error>;

  private:
    auto GetOrCreateEntity(ECS::Registry& reg, uint64_t uid) -> Entity;
};

} // namespace ZHLN::Net

export namespace ZHLN::Net {

class ZHLN_API NetworkClient {
  public:
    NetworkClient();
    ~NetworkClient();

    NetworkClient(const NetworkClient&)                    = delete;
    auto operator=(const NetworkClient&) -> NetworkClient& = delete;
    NetworkClient(NetworkClient&&) noexcept;
    auto operator=(NetworkClient&&) noexcept -> NetworkClient&;

    [[nodiscard]] auto Connect(std::string_view host, uint16_t port, uint64_t userId, std::string_view token) noexcept
        -> std::expected<void, Error>;
    void Disconnect() noexcept;

    [[nodiscard]] auto PollEvents(Engine& engine) noexcept -> std::expected<void, Error>;
    void               SendInputs(bool forward, bool backward, bool left, bool right, bool jump, float yaw) noexcept;

    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] bool IsRealtimeReady() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

void RegisterNetworkSubsystem(Engine& engine);

} // namespace ZHLN::Net
