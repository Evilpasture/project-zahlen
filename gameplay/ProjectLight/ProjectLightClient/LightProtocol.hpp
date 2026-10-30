// gameplay/ProjectLight/ProjectLightClient/LightProtocol.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Zahlen/Physics/Jolt.hpp>
// clang-format on
#include <ProjectLightClient/SceneMirror.hpp>
#include <Socket/Socket.hpp>
#include <Replication/ClassVocabulary.hpp>
#include <Replication/ReplicaState.hpp>
#include <Replication/ReplicationComponents.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/Physics/PhysicsHandles.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ZHLN {
class Engine;
} // namespace ZHLN

namespace ZHLN::ProjectLight {

// ============================================================================
// project-light transport constants and property channels
// ============================================================================

inline constexpr uint8_t FRAME_FLAG_COMPRESSED      = 1u;
inline constexpr size_t  COMPRESSION_MIN_BYTES      = 1024u;
inline constexpr size_t  MAX_SAFE_UDP_PAYLOAD_BYTES = 1200u;
inline constexpr size_t  MAX_STREAM_FRAME_BYTES     = static_cast<const size_t>(128u * 1024u * 1024u);
inline constexpr float   VEC3_SCALE                 = 32767.0f;
inline constexpr float   COLOR3_SCALE               = 255.0f;
inline constexpr size_t  PROPERTY_CHANNEL_COUNT     = 104u;

[[nodiscard]] auto PropertyChannelName(uint16_t channel) noexcept -> std::string_view;
[[nodiscard]] auto PropertyNameChannel(std::string_view name) noexcept -> std::optional<uint16_t>;

// ============================================================================
// RFC 1950 / RFC 1951 helpers (zlib-framed DEFLATE)
// ============================================================================

[[nodiscard]] auto
    InflateZlib(std::span<const uint8_t> compressed, size_t maxOutputBytes = MAX_STREAM_FRAME_BYTES) -> std::expected<std::vector<uint8_t>, std::string>;
[[nodiscard]] auto DeflateZlib(std::span<const uint8_t> raw) -> std::expected<std::vector<uint8_t>, std::string>;

// ============================================================================
// Small self-contained MessagePack value model
// ============================================================================

struct MsgPackValue {
    enum class Kind : uint8_t { Nil, Bool, Int, UInt, Float, String, Binary, Array, Map };

    Kind                                               kind     = Kind::Nil;
    bool                                               boolVal  = false;
    int64_t                                            intVal   = 0;
    uint64_t                                           uintVal  = 0;
    double                                             floatVal = 0.0;
    std::string                                        strVal;
    std::vector<uint8_t>                               binVal;
    std::vector<MsgPackValue>                          arrayVal;
    std::vector<std::pair<MsgPackValue, MsgPackValue>> mapVal;

    [[nodiscard]] static auto Nil() -> MsgPackValue;
    [[nodiscard]] static auto Bool(bool value) -> MsgPackValue;
    [[nodiscard]] static auto Int(int64_t value) -> MsgPackValue;
    [[nodiscard]] static auto UInt(uint64_t value) -> MsgPackValue;
    [[nodiscard]] static auto Float(double value) -> MsgPackValue;
    [[nodiscard]] static auto String(std::string_view value) -> MsgPackValue;
    [[nodiscard]] static auto Binary(std::span<const uint8_t> value) -> MsgPackValue;
    [[nodiscard]] static auto Array(std::vector<MsgPackValue> value = {}) -> MsgPackValue;
    [[nodiscard]] static auto Map(std::vector<std::pair<MsgPackValue, MsgPackValue>> value = {}) -> MsgPackValue;

    [[nodiscard]] auto IsNil() const noexcept -> bool {
        return kind == Kind::Nil;
    }
    [[nodiscard]] auto IsMap() const noexcept -> bool {
        return kind == Kind::Map;
    }
    [[nodiscard]] auto IsArray() const noexcept -> bool {
        return kind == Kind::Array;
    }
    [[nodiscard]] auto AsBool(bool fallback = false) const noexcept -> bool;
    [[nodiscard]] auto AsInt(int64_t fallback = 0) const noexcept -> int64_t;
    [[nodiscard]] auto AsUInt(uint64_t fallback = 0) const noexcept -> uint64_t;
    [[nodiscard]] auto AsDouble(double fallback = 0.0) const noexcept -> double;
    [[nodiscard]] auto AsFloat(float fallback = 0.0f) const noexcept -> float;
    [[nodiscard]] auto AsString(std::string_view fallback = {}) const -> std::string;
    [[nodiscard]] auto AsUid() const noexcept -> uint64_t;
    [[nodiscard]] auto AsVec3(float integerScale = 1.0f, JPH::Vec3 fallback = JPH::Vec3::sZero()) const noexcept -> JPH::Vec3;
    [[nodiscard]] auto Find(std::string_view key) const noexcept -> const MsgPackValue*;
    [[nodiscard]] auto FindIntKey(int64_t key) const noexcept -> const MsgPackValue*;
    [[nodiscard]] auto FindProperty(uint16_t channel, std::string_view propertyName = {}) const noexcept -> const MsgPackValue*;
    void               Set(std::string_view key, MsgPackValue value);
    void               SetIntKey(int64_t key, MsgPackValue value);
};

[[nodiscard]] auto EncodeMsgPack(const MsgPackValue& value) -> std::vector<uint8_t>;
[[nodiscard]] auto DecodeMsgPack(std::span<const uint8_t> bytes) -> std::expected<MsgPackValue, std::string>;

// TCP: [u32 big-endian body length][flags][payload]; UDP: raw MessagePack.
[[nodiscard]] auto EncodePayloadBody(const MsgPackValue& payload, bool compress = false) -> std::expected<std::vector<uint8_t>, std::string>;
[[nodiscard]] auto DecodePayloadBody(std::span<const uint8_t> body, size_t maxOutputBytes = MAX_STREAM_FRAME_BYTES) -> std::expected<MsgPackValue, std::string>;
[[nodiscard]] auto EncodeStreamFrame(const MsgPackValue& payload, bool compress = false) -> std::expected<std::vector<uint8_t>, std::string>;
[[nodiscard]] auto PeekStreamBodyLength(std::span<const uint8_t> streamBytes) noexcept -> std::optional<size_t>;
[[nodiscard]] auto
    DecodeStreamFrame(std::span<const uint8_t> frameBytes, size_t maxOutputBytes = MAX_STREAM_FRAME_BYTES) -> std::expected<MsgPackValue, std::string>;

// ============================================================================
// Launch URL / command-line connection settings
// ============================================================================

struct LaunchConfig {
    bool        enabled = false;
    std::string host;
    uint16_t    port   = 0;
    uint64_t    userId = 0;
    std::string username;
    std::string token;
    std::string place;
};

[[nodiscard]] auto ParseLaunchUrl(std::string_view url, std::string_view scheme, LaunchConfig base = {}) -> std::expected<LaunchConfig, std::string>;
[[nodiscard]] auto ParseHostPort(std::string_view endpoint, std::string_view& host, uint16_t& port) -> bool;
[[nodiscard]] auto ExtractLaunchUrlArgs(std::span<const std::string_view> rawArgs, LaunchConfig& config, std::string_view scheme)
    -> std::expected<std::vector<std::string>, std::string>;

// ============================================================================
// Replicated instance state
// ============================================================================
//
// The mirrored record, its playout history and the identity components all live
// under Replication/. What is left in this header is transport: bytes in, bytes
// out, and the session that owns both.

// InputControls is session-side: it is what this client sends, not what it is
// told. Kept here rather than in Replication/ so a change to the outbound shape
// lands next to the sender.
struct InputControls {
    bool  forward  = false;
    bool  backward = false;
    bool  left     = false;
    bool  right    = false;
    bool  jump     = false;
    float yaw      = -90.0f;
};

// ============================================================================
// TCP/UDP session and DataModel replicator
// ============================================================================
//
// ClientSession talks to the server: it connects, polls, decodes packets, applies
// what they say to the mirrored records, and sends this client's own state back.
// It does not build the scene -- that is SceneMirror's job, which it reaches
// through MirrorState() and nothing else. The split matters because the two halves
// fail differently: a session bug loses state, a mirror bug loses entities, and
// only one of them needs a socket to reproduce.

class ClientSession {
  public:
    ClientSession() noexcept = default;
    ~ClientSession() noexcept;

    ClientSession(const ClientSession&)                    = delete;
    auto operator=(const ClientSession&) -> ClientSession& = delete;

    [[nodiscard]] auto Connect(const LaunchConfig& config) -> std::expected<void, std::string>;
    void               Disconnect() noexcept;
    [[nodiscard]] auto IsConnected() const noexcept -> bool {
        return m_connected;
    }
    [[nodiscard]] auto IsSnapshotComplete() const noexcept -> bool {
        return m_snapshotComplete;
    }
    [[nodiscard]] auto IsRealtimeOverTcp() const noexcept -> bool {
        return m_realtimeOverTcp;
    }
    [[nodiscard]] auto IsServerAuthority() const noexcept -> bool {
        return m_serverAuthority;
    }
    [[nodiscard]] auto LocalUserId() const noexcept -> uint64_t {
        return m_config.userId;
    }
    [[nodiscard]] auto LocalPlayerUid() const noexcept -> uint64_t {
        return m_localPlayerUid;
    }
    [[nodiscard]] auto LocalCharacterUid() const noexcept -> uint64_t {
        return m_localCharacterUid;
    }
    [[nodiscard]] auto LocalRootPartUid() const noexcept -> uint64_t {
        return m_localRootPartUid;
    }
    [[nodiscard]] auto Objects() const noexcept -> const std::unordered_map<uint64_t, ReplicatedObject>& {
        return m_objects;
    }
    [[nodiscard]] auto FindObject(uint64_t uid) const noexcept -> const ReplicatedObject*;

    // The session's uid -> entity binding: the one owner of "where is instance N
    // in the scene". Spawning binds, teardown takes.
    [[nodiscard]] auto Index() noexcept -> ReplicationIndex& {
        return m_index;
    }
    [[nodiscard]] auto Index() const noexcept -> const ReplicationIndex& {
        return m_index;
    }
    [[nodiscard]] auto EntityFor(InstanceId uid) const noexcept -> Entity {
        return m_index.Find(uid);
    }

    // Public packet hook is useful for deterministic, socket-free protocol tests.
    auto HandlePacket(const MsgPackValue& packet) -> bool;
    void PollNetwork(Engine& engine);
    void PrePhysicsTick(Engine& engine, float dt);
    void PostPhysicsTick(Engine& engine, float dt);
    void ResetSceneBindings(Engine& engine) noexcept;
    void OnEngineTeardown(Engine& engine) noexcept;

  private:
    void               ApplySnapshotMap(const MsgPackValue& snapshot);
    void               ApplySnapshotEntry(uint64_t uid, const MsgPackValue& properties);
    void               ApplyProperties(ReplicatedObject& object, const MsgPackValue& properties);
    void               ApplyTreeUpdates(const MsgPackValue& treeUpdates);
    void               ApplyPhysicsUpdates(const MsgPackValue& physicsUpdates);
    void               ApplyControllerState(const MsgPackValue& controllerState);
    void               ApplySoundUpdates(const MsgPackValue& soundUpdates);
    void               DeleteObjectSubtree(uint64_t uid);
    void               RefreshServiceLinks() noexcept;
    void               RefreshPlayerLinks() noexcept;
    [[nodiscard]] auto IsPartOfCharacter(uint64_t partUid, uint64_t characterUid) const noexcept -> bool;
    [[nodiscard]] auto IsLocalCharacterPart(uint64_t uid) const noexcept -> bool;

    // The scene half's view of this session. Built per call, never stored: the
    // members above stay the owners, the mirror only visits them.
    [[nodiscard]] auto MirrorState() noexcept -> SceneMirrorState;

    void SendRealtimeFrame(Engine& engine);

    [[nodiscard]] auto SendTcpPacket(const MsgPackValue& packet) -> bool;
    [[nodiscard]] auto SendRealtimePacket(const MsgPackValue& packet) -> bool;
    void               FlushTcpTxBuffer() noexcept;
    void               CloseSockets() noexcept;

    LaunchConfig m_config {};
    Net::Socket  m_tcpSocket;
    Net::Socket  m_udpSocket;
    Net::SocketEndpoint                         m_serverEndpoint {};
    uint16_t                                    m_localUdpPort             = 0u;
    bool                                        m_connected                = false;
    bool                                        m_snapshotComplete         = false;
    bool                                        m_realtimeOverTcp          = false;
    bool                                        m_serverAuthority          = false;
    bool                                                 m_streamErrorLogged        = false;
    bool                                                 m_realtimeModeReceived     = false;
    bool                                                 m_networkPollStarted       = false;
    bool                                                 m_sceneSyncSummaryLogged   = false;
    bool                                                 m_snapshotClassProbeLogged = false;
    std::optional<uint16_t>                              m_remoteClassNameChannel;
    std::array<std::string_view, PROPERTY_CHANNEL_COUNT> m_remotePropertyNameOverrides {};

    std::chrono::steady_clock::time_point m_lastNetworkStatusLog;
    uint64_t                              m_tcpBytesSent         = 0u;
    uint64_t                              m_tcpBytesReceived     = 0u;
    uint64_t                              m_tcpFramesReceived    = 0u;
    uint64_t                              m_udpBytesSent         = 0u;
    uint64_t                              m_udpBytesReceived     = 0u;
    uint64_t                              m_udpDatagramsSent     = 0u;
    uint64_t                              m_udpDatagramsReceived = 0u;
    uint64_t                              m_udpDecodeErrors      = 0u;
    uint64_t                              m_unrecognizedPackets  = 0u;

    uint64_t m_dataModelUid      = 0u;
    uint64_t m_workspaceUid      = 0u;
    uint64_t m_playersServiceUid = 0u;
    uint64_t m_lightingUid       = 0u;
    uint64_t m_physicsServiceUid = 0u;
    uint64_t m_localPlayerUid    = 0u;
    uint64_t m_localCharacterUid = 0u;
    uint64_t m_localHumanoidUid  = 0u;
    uint64_t m_localRootPartUid  = 0u;

    float               m_time            = 0.0f;
    float               m_sendAccumulator = 0.0f;
    float               m_jumpRemaining   = 0.0f;
    bool                m_wasJumpDown     = false;
    InputControls       m_lastControls {};
    Entity              m_localController      = Entity::Null();
    uint64_t            m_controllerForRootUid = 0u;
    Physics::BodyHandle m_localControllerBody  = Physics::BodyHandle::Null();

    std::vector<uint8_t>                                  m_tcpRxBuffer;
    std::vector<uint8_t>                                  m_tcpTxBuffer;
    size_t                                                m_tcpTxOffset = 0u;
    std::unordered_map<uint64_t, NetworkTransformHistory> m_playerControllerStates;
    std::vector<uint64_t>                                 m_pendingRemovals;
    std::unordered_map<uint64_t, ReplicatedObject>        m_objects;
    ReplicationIndex                                      m_index;
    SceneMirror                                           m_mirror;
};

// The module as a value: the client's two frame steps, the scene-cleanup pass
// that rebinds the session after a scene reset, and the engine-teardown hook
// that detaches it. InstallClient binds the session these hooks look up and then
// installs this description, so a host that wants the placement error itself
// reads the log.
auto DescribeClient() -> Module;

void InstallClient(Engine& engine, ClientSession& session);

} // namespace ZHLN::ProjectLight
