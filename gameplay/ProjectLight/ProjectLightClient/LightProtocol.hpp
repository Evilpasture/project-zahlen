// gameplay/ProjectLight/ProjectLightClient/LightProtocol.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Entity.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/physics/PhysicsHandles.hpp>
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
struct CommandHandler;
struct FrameContext;
} // namespace ZHLN

namespace ZHLN::ProjectLight {

// ============================================================================
// project-light transport constants and property channels
// ============================================================================

inline constexpr uint8_t  FRAME_FLAG_COMPRESSED        = 1u;
inline constexpr size_t   COMPRESSION_MIN_BYTES        = 1024u;
inline constexpr size_t   MAX_SAFE_UDP_PAYLOAD_BYTES   = 1200u;
inline constexpr size_t   MAX_STREAM_FRAME_BYTES       = 128u * 1024u * 1024u;
inline constexpr float    VEC3_SCALE                   = 32767.0f;
inline constexpr float    COLOR3_SCALE                 = 255.0f;
inline constexpr size_t   PROPERTY_CHANNEL_COUNT       = 104u;

[[nodiscard]] auto PropertyChannelName(uint16_t channel) noexcept -> std::string_view;
[[nodiscard]] auto PropertyNameChannel(std::string_view name) noexcept -> std::optional<uint16_t>;

// ============================================================================
// RFC 1950 / RFC 1951 helpers (zlib-framed DEFLATE)
// ============================================================================

[[nodiscard]] auto InflateZlib(std::span<const uint8_t> compressed, size_t maxOutputBytes = MAX_STREAM_FRAME_BYTES)
    -> std::expected<std::vector<uint8_t>, std::string>;
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
    std::string                                        strVal  {};
    std::vector<uint8_t>                               binVal  {};
    std::vector<MsgPackValue>                          arrayVal {};
    std::vector<std::pair<MsgPackValue, MsgPackValue>> mapVal {};

    [[nodiscard]] static auto Nil() -> MsgPackValue;
    [[nodiscard]] static auto Bool(bool value) -> MsgPackValue;
    [[nodiscard]] static auto Int(int64_t value) -> MsgPackValue;
    [[nodiscard]] static auto UInt(uint64_t value) -> MsgPackValue;
    [[nodiscard]] static auto Float(double value) -> MsgPackValue;
    [[nodiscard]] static auto String(std::string_view value) -> MsgPackValue;
    [[nodiscard]] static auto Binary(std::span<const uint8_t> value) -> MsgPackValue;
    [[nodiscard]] static auto Array(std::vector<MsgPackValue> value = {}) -> MsgPackValue;
    [[nodiscard]] static auto Map(std::vector<std::pair<MsgPackValue, MsgPackValue>> value = {}) -> MsgPackValue;

    [[nodiscard]] auto IsNil() const noexcept -> bool { return kind == Kind::Nil; }
    [[nodiscard]] auto IsMap() const noexcept -> bool { return kind == Kind::Map; }
    [[nodiscard]] auto IsArray() const noexcept -> bool { return kind == Kind::Array; }
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
    void Set(std::string_view key, MsgPackValue value);
    void SetIntKey(int64_t key, MsgPackValue value);
};

[[nodiscard]] auto EncodeMsgPack(const MsgPackValue& value) -> std::vector<uint8_t>;
[[nodiscard]] auto DecodeMsgPack(std::span<const uint8_t> bytes) -> std::expected<MsgPackValue, std::string>;

// TCP: [u32 big-endian body length][flags][payload]; UDP: raw MessagePack.
[[nodiscard]] auto EncodePayloadBody(const MsgPackValue& payload, bool compress = false) -> std::expected<std::vector<uint8_t>, std::string>;
[[nodiscard]] auto DecodePayloadBody(std::span<const uint8_t> body, size_t maxOutputBytes = MAX_STREAM_FRAME_BYTES)
    -> std::expected<MsgPackValue, std::string>;
[[nodiscard]] auto EncodeStreamFrame(const MsgPackValue& payload, bool compress = false) -> std::expected<std::vector<uint8_t>, std::string>;
[[nodiscard]] auto PeekStreamBodyLength(std::span<const uint8_t> streamBytes) noexcept -> std::optional<size_t>;
[[nodiscard]] auto DecodeStreamFrame(std::span<const uint8_t> frameBytes, size_t maxOutputBytes = MAX_STREAM_FRAME_BYTES)
    -> std::expected<MsgPackValue, std::string>;

// ============================================================================
// Launch URL / command-line connection settings
// ============================================================================

struct LaunchConfig {
    bool        enabled  = false;
    std::string host;
    uint16_t    port     = 0;
    uint64_t    userId   = 0;
    std::string username;
    std::string token;
    std::string place;
};

[[nodiscard]] auto ParseLaunchUrl(std::string_view url, std::string_view scheme, LaunchConfig base = {})
    -> std::expected<LaunchConfig, std::string>;
[[nodiscard]] auto ParseHostPort(std::string_view endpoint, std::string& host, uint16_t& port) -> bool;
[[nodiscard]] auto ExtractLaunchUrlArgs(std::span<const std::string_view> rawArgs, LaunchConfig& config, std::string_view scheme)
    -> std::expected<std::vector<std::string>, std::string>;

// ============================================================================
// Replicated server instance state (the live DataModel is held by the client)
// ============================================================================

enum class PartShape : uint8_t { Cube = 0, Sphere = 1, Wedge = 2, CornerWedge = 3, Cylinder = 4, Capsule = 5, Cone = 6, Head = 7 };
enum class HumanoidState : uint8_t { Idle = 0, Running = 1, Jumping = 2, Freefall = 3, FallingDown = 4, GettingUp = 5 };

struct TransformState {
    JPH::Vec3 position       = JPH::Vec3::sZero();
    JPH::Quat rotation       = JPH::Quat::sIdentity();
    JPH::Vec3 linearVelocity = JPH::Vec3::sZero();
};

struct NetworkTransformSample {
    JPH::Vec3 position = JPH::Vec3::sZero();
    JPH::Quat rotation = JPH::Quat::sIdentity();
    std::chrono::steady_clock::time_point receivedAt {};
};

// A small fixed ring keeps enough 60 Hz samples for a short, jitter-tolerant
// 20 ms playout target without imposing the Python client's longer delay.
struct NetworkTransformHistory {
    static constexpr std::size_t Capacity = 8;

    void Push(const NetworkTransformSample& sample) noexcept;
    void Clear() noexcept { m_next = 0; m_count = 0; }
    [[nodiscard]] auto Empty() const noexcept -> bool { return m_count == 0; }
    [[nodiscard]] auto Sample(
        std::chrono::steady_clock::time_point renderTime,
        JPH::Vec3& position,
        JPH::Quat& rotation
    ) const noexcept -> bool;

  private:
    std::array<NetworkTransformSample, Capacity> m_samples {};
    std::size_t m_next  = 0;
    std::size_t m_count = 0;
};

struct InputControls {
    bool  forward  = false;
    bool  backward = false;
    bool  left     = false;
    bool  right    = false;
    bool  jump     = false;
    float yaw      = -90.0f;
};

struct ReplicatedObject {
    uint64_t      uid              = 0u;
    std::string   className        {};
    std::string   name             = "Instance";
    uint64_t      parentUid        = 0u;

    JPH::Vec3     position         = JPH::Vec3::sZero();
    JPH::Vec3     rotationEulerDeg = JPH::Vec3::sZero();
    JPH::Quat     rotationQuat     = JPH::Quat::sIdentity();
    JPH::Vec3     linearVelocity   = JPH::Vec3::sZero();
    JPH::Vec3     renderPosition   = JPH::Vec3::sZero();
    JPH::Quat     renderRotation   = JPH::Quat::sIdentity();
    NetworkTransformHistory physicsHistory {};
    bool          hasPhysicsSample    = false;
    JPH::Vec3     size             = JPH::Vec3(4.0f, 1.0f, 2.0f);
    JPH::Vec3     color            = JPH::Vec3(0.75f, 0.75f, 0.75f);
    float         transparency     = 0.0f;
    float         metallic         = 0.0f;
    float         roughness        = 0.5f;
    float         emission         = 0.0f;
    bool          anchored         = false;
    bool          canCollide       = true;
    uint64_t      networkOwnerUid  = 0u;
    PartShape     shape            = PartShape::Cube;
    std::string   meshId           {};
    JPH::Vec3     meshScale        = JPH::Vec3::sReplicate(1.0f);

    uint64_t      primaryPartUid   = 0u;
    uint64_t      rootPartUid      = 0u;
    uint64_t      characterUid     = 0u;
    uint64_t      userId           = 0u;
    HumanoidState humanoidState    = HumanoidState::Idle;
    float         walkSpeed        = 16.0f;
    float         jumpPower        = 50.0f;
    float         health           = 100.0f;
    float         maxHealth        = 100.0f;

    uint64_t      part1Uid         = 0u;
    uint64_t      part2Uid         = 0u;
    JPH::Vec3     offset1          = JPH::Vec3::sZero();
    JPH::Vec3     offset2          = JPH::Vec3::sZero();
    JPH::Vec3     rotationOffset1  = JPH::Vec3::sZero();
    JPH::Vec3     rotationOffset2  = JPH::Vec3::sZero();
    float         currentAngle     = 0.0f;
    float         desiredAngle     = 0.0f;
    float         maxVelocity      = 30.0f;

    bool          enabled          = true;
    float         intensity        = 1.0f;
    bool          shadows          = true;
    float         sourceRadius     = 0.0f;
    JPH::Vec3     direction        = JPH::Vec3(0.0f, -1.0f, 0.0f);
    bool          isMain           = false;
    float         range            = 16.0f;
    JPH::Vec3     ambient           = JPH::Vec3(0.1f, 0.1f, 0.1f);
    float         timeOfDay        = 14.0f;
    float         latitude         = 41.7f;
    bool          serverAuthority  = false;

    std::string   soundId          {};
    float         volume           = 0.5f;
    bool          loops            = false;
    bool          playing          = false;

    Entity        entity           = Entity::Null();
    MaterialID    materialId       = InvalidMaterialID;
    JPH::Vec3     spawnedSize      = JPH::Vec3::sZero();
    PartShape     spawnedShape     = PartShape::Cube;
    uint64_t      spawnedOwnerUid  = 0u;
    bool          hasPhysicsBody   = false;
    bool          spawnedAnchored  = false;
    bool          spawnedCanCollide = true;
    bool          spawnedBlend     = false;
    bool          visualDirty      = true;
    bool          transformDirty   = true;
    bool          removalPending   = false;
    AudioHandle   audioHandle      = AudioHandle::Invalid;
};

// ============================================================================
// TCP/UDP session, DataModel replicator, and ECS/renderer bridge
// ============================================================================

class ClientSession {
  public:
    ClientSession() noexcept = default;
    ~ClientSession() noexcept;

    ClientSession(const ClientSession&)            = delete;
    auto operator=(const ClientSession&) -> ClientSession& = delete;

    [[nodiscard]] auto Connect(const LaunchConfig& config) -> std::expected<void, std::string>;
    void               Disconnect() noexcept;
    [[nodiscard]] auto IsConnected() const noexcept -> bool { return m_connected; }
    [[nodiscard]] auto IsSnapshotComplete() const noexcept -> bool { return m_snapshotComplete; }
    [[nodiscard]] auto IsRealtimeOverTcp() const noexcept -> bool { return m_realtimeOverTcp; }
    [[nodiscard]] auto IsServerAuthority() const noexcept -> bool { return m_serverAuthority; }
    [[nodiscard]] auto LocalUserId() const noexcept -> uint64_t { return m_config.userId; }
    [[nodiscard]] auto LocalPlayerUid() const noexcept -> uint64_t { return m_localPlayerUid; }
    [[nodiscard]] auto LocalCharacterUid() const noexcept -> uint64_t { return m_localCharacterUid; }
    [[nodiscard]] auto LocalRootPartUid() const noexcept -> uint64_t { return m_localRootPartUid; }
    [[nodiscard]] auto Objects() const noexcept -> const std::unordered_map<uint64_t, ReplicatedObject>& { return m_objects; }
    [[nodiscard]] auto FindObject(uint64_t uid) const noexcept -> const ReplicatedObject*;

    // Public packet hook is useful for deterministic, socket-free protocol tests.
    auto HandlePacket(const MsgPackValue& packet) -> bool;
    void PollNetwork(Engine& engine);
    void PrePhysicsTick(Engine& engine, float dt);
    void PostPhysicsTick(Engine& engine, float dt);
    void ResetSceneBindings(Engine& engine) noexcept;
    void OnEngineTeardown(Engine& engine) noexcept;

  private:
    void ApplySnapshotMap(const MsgPackValue& snapshot);
    void ApplySnapshotEntry(uint64_t uid, const MsgPackValue& properties);
    void ApplyProperties(ReplicatedObject& object, const MsgPackValue& properties);
    void ApplyTreeUpdates(const MsgPackValue& treeUpdates);
    void ApplyPhysicsUpdates(const MsgPackValue& physicsUpdates);
    void ApplyControllerState(const MsgPackValue& controllerState);
    void ApplySoundUpdates(const MsgPackValue& soundUpdates);
    void DeleteObjectSubtree(uint64_t uid);
    void RefreshServiceLinks() noexcept;
    void RefreshPlayerLinks() noexcept;
    [[nodiscard]] auto IsPartOfCharacter(uint64_t partUid, uint64_t characterUid) const noexcept -> bool;
    [[nodiscard]] auto IsLocalCharacterPart(uint64_t uid) const noexcept -> bool;
    [[nodiscard]] auto ShouldCreatePhysics(const ReplicatedObject& object) const noexcept -> bool;

    void SyncSceneEntities(Engine& engine);
    void SyncEnvironment(Engine& engine);
    void SpawnOrUpdatePartEntity(Engine& engine, ReplicatedObject& object);
    void SpawnOrUpdateLightEntity(Engine& engine, ReplicatedObject& object);
    void UpdateLocalController(Engine& engine);
    void UpdateLocalCharacterParts(Engine& engine, float dt);
    void UpdateRemoteCharacterParts(Engine& engine, float dt);
    void UpdateSoundVoices(Engine& engine);
    void SendRealtimeFrame(Engine& engine);

    [[nodiscard]] auto SendTcpPacket(const MsgPackValue& packet) -> bool;
    [[nodiscard]] auto SendRealtimePacket(const MsgPackValue& packet) -> bool;
    void FlushTcpTxBuffer() noexcept;
    void CloseSockets() noexcept;

    LaunchConfig                         m_config             {};
    intptr_t                             m_tcpSocket          = -1;
    intptr_t                             m_udpSocket          = -1;
    alignas(16) std::array<uint8_t, 128> m_serverAddress      {};
    uint32_t                             m_serverAddressBytes = 0u;
    uint16_t                             m_localUdpPort       = 0u;
    bool                                 m_connected          = false;
    bool                                 m_snapshotComplete   = false;
    bool                                 m_realtimeOverTcp    = false;
    bool                                 m_serverAuthority    = false;
    bool                                 m_winsockStarted     = false;
    bool                                 m_streamErrorLogged  = false;
    bool                                 m_realtimeModeReceived = false;
    bool                                 m_networkPollStarted = false;
    bool                                 m_sceneSyncSummaryLogged = false;
    bool                                 m_snapshotClassProbeLogged = false;
    std::optional<uint16_t>              m_remoteClassNameChannel {};
    std::array<std::string_view, PROPERTY_CHANNEL_COUNT> m_remotePropertyNameOverrides {};

    std::chrono::steady_clock::time_point m_lastNetworkStatusLog {};
    uint64_t                             m_tcpBytesSent       = 0u;
    uint64_t                             m_tcpBytesReceived   = 0u;
    uint64_t                             m_tcpFramesReceived  = 0u;
    uint64_t                             m_udpBytesSent       = 0u;
    uint64_t                             m_udpBytesReceived   = 0u;
    uint64_t                             m_udpDatagramsSent   = 0u;
    uint64_t                             m_udpDatagramsReceived = 0u;
    uint64_t                             m_udpDecodeErrors    = 0u;
    uint64_t                             m_unrecognizedPackets = 0u;

    uint64_t                             m_dataModelUid       = 0u;
    uint64_t                             m_workspaceUid       = 0u;
    uint64_t                             m_playersServiceUid  = 0u;
    uint64_t                             m_lightingUid        = 0u;
    uint64_t                             m_physicsServiceUid  = 0u;
    uint64_t                             m_localPlayerUid     = 0u;
    uint64_t                             m_localCharacterUid  = 0u;
    uint64_t                             m_localHumanoidUid   = 0u;
    uint64_t                             m_localRootPartUid   = 0u;

    float                                m_time               = 0.0f;
    float                                m_sendAccumulator    = 0.0f;
    float                                m_jumpRemaining      = 0.0f;
    bool                                 m_wasJumpDown        = false;
    InputControls                        m_lastControls       {};
    Entity                               m_localController    = Entity::Null();
    uint64_t                             m_controllerForRootUid = 0u;
    Physics::BodyHandle                 m_localControllerBody = Physics::BodyHandle::Null();

    std::vector<uint8_t>                 m_tcpRxBuffer        {};
    std::vector<uint8_t>                 m_tcpTxBuffer        {};
    size_t                               m_tcpTxOffset        = 0u;
    std::unordered_map<uint64_t, NetworkTransformHistory> m_playerControllerStates {};
    std::vector<uint64_t>                m_pendingRemovals    {};
    std::unordered_map<uint64_t, ReplicatedObject> m_objects     {};
};

// Installs frame-phase, scene-cleanup, and engine-teardown hooks. Call after
// Engine::Create and before InitializeDefaultScene; app/ owns all wiring.
void InstallClient(Engine& engine, ClientSession& session);

} // namespace ZHLN::ProjectLight
