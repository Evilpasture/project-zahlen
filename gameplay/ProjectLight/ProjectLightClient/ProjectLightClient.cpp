// gameplay/ProjectLight/ProjectLightClient/ProjectLightClient.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ProjectLightClient/LightProtocol.hpp>

#include <CharacterController/CharacterComponents.hpp>
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Engine/FramePlan.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ZHLN::ProjectLight {
namespace {

constexpr int kConnectTimeoutMs = 5000;
constexpr size_t kReceiveChunkBytes = 64 * 1024;
constexpr float kSendRateSeconds = 1.0f / 60.0f;

auto SocketErrorText(std::string_view operation, int error) -> std::string {
    return std::format("{} failed (socket error {})", operation, error);
}

auto ReadReference(const MsgPackValue& value) noexcept -> uint64_t {
    return value.IsNil() ? 0u : value.AsUid();
}

auto GetFrameAction(const MsgPackValue& packet) -> std::string_view {
    if (const MsgPackValue* action = packet.Find("action"); action && action->kind == MsgPackValue::Kind::String) return action->strVal;
    return {};
}

auto NetworkPacketSummary(const MsgPackValue& packet) -> std::string {
    std::string summary;
    auto append = [&summary](std::string field) {
        if (!summary.empty()) summary += ", ";
        summary += field;
    };
    const auto mapCount = [](const MsgPackValue* value) -> size_t {
        return value != nullptr && value->IsMap() ? value->mapVal.size() : 0u;
    };
    const auto arrayCount = [](const MsgPackValue* value) -> size_t {
        return value != nullptr && value->IsArray() ? value->arrayVal.size() : 0u;
    };

    if (const auto* value = packet.Find("initialDataModelSnapshot")) {
        append(std::format("initialDataModelSnapshot={}", mapCount(value)));
    }
    if (const auto* value = packet.Find("initialServicesSnapshot")) {
        append(std::format("initialServicesSnapshot={}", mapCount(value)));
    }
    if (const auto* value = packet.Find("initialObjectsSnapshot")) {
        append(std::format("initialObjectsSnapshot={}", mapCount(value)));
    }
    if (const auto* value = packet.Find("treeUpdates")) {
        const auto* updates = value->Find("updates");
        const auto* removals = value->Find("removals");
        append(std::format("treeUpdates(updates={}, removals={})", mapCount(updates), arrayCount(removals)));
    }
    if (const auto* value = packet.Find("physicsUpdates")) {
        append(std::format("physicsUpdates={}", mapCount(value->Find("physics"))));
    }
    if (packet.Find("controllerState") != nullptr) append("controllerState");
    if (const auto* value = packet.Find("soundUpdates")) {
        append(std::format("soundUpdates={}", mapCount(value->Find("updates"))));
    }

    if (summary.empty()) {
        const std::string_view action = GetFrameAction(packet);
        return action.empty() ? "no recognized fields" : std::format("action={}", action);
    }
    return summary;
}

auto IsKnownReplicatedClassName(std::string_view name) noexcept -> bool {
    return name == "DataModel" || name == "CoreService" || name == "WorkspaceService" || name == "MathService"
           || name == "LightingService" || name == "PhysicsService" || name == "SoundService" || name == "UserInputService"
           || name == "PlayersService" || name == "RunService" || name == "ScriptService" || name == "ShaderService"
           || name == "Player" || name == "Humanoid" || name == "Camera" || name == "Model" || name == "Part"
           || name == "MeshPart" || name == "BasePart" || name == "Light" || name == "DirectionalLight" || name == "PointLight"
           || name == "Weld" || name == "Motor" || name == "Sound" || name == "Decal" || name == "SpawnPoint"
           || name == "InternalScript" || name == "Gizmo" || name == "Highlight" || name == "Shader" || name == "MaterialModifier";
}

auto SnapshotClassProbe(const MsgPackValue& snapshot, std::optional<uint16_t>* inferredClassNameChannel = nullptr) -> std::string {
    if (!snapshot.IsMap()) return "snapshot value is not a map";

    const auto classChannel = PropertyNameChannel("ClassName");
    const int channelIndex = classChannel ? static_cast<int>(*classChannel) : -1;
    size_t entries = 0;
    size_t numericClassKeys = 0;
    size_t stringClassKeys = 0;
    size_t stringValues = 0;
    size_t nonStringValues = 0;
    size_t missingClassKeys = 0;
    std::unordered_map<std::string, size_t> classCounts;
    std::unordered_map<int64_t, std::unordered_map<std::string, size_t>> alternateClassChannels;
    std::string sampleKeys;

    for (const auto& [uid, properties]: snapshot.mapVal) {
        (void)uid;
        if (!properties.IsMap()) continue;
        ++entries;

        const MsgPackValue* numericClassValue = classChannel ? properties.FindIntKey(*classChannel) : nullptr;
        const MsgPackValue* stringClassValue = properties.Find("ClassName");
        if (numericClassValue != nullptr) ++numericClassKeys;
        if (stringClassValue != nullptr) ++stringClassKeys;
        const MsgPackValue* classValue = numericClassValue != nullptr ? numericClassValue : stringClassValue;
        if (classValue == nullptr) {
            ++missingClassKeys;
        } else if (classValue->kind == MsgPackValue::Kind::String) {
            ++stringValues;
            ++classCounts[classValue->strVal];
        } else {
            ++nonStringValues;
        }

        for (const auto& [key, value]: properties.mapVal) {
            if ((key.kind != MsgPackValue::Kind::Int && key.kind != MsgPackValue::Kind::UInt)
                || value.kind != MsgPackValue::Kind::String || !IsKnownReplicatedClassName(value.strVal)) {
                continue;
            }
            const int64_t candidateChannel = key.AsInt(-1);
            if (candidateChannel >= 0 && (!classChannel || candidateChannel != static_cast<int64_t>(*classChannel))) {
                ++alternateClassChannels[candidateChannel][value.strVal];
            }
        }

        if (sampleKeys.empty()) {
            for (const auto& [key, value]: properties.mapVal) {
                (void)value;
                std::string keyText;
                if (key.kind == MsgPackValue::Kind::String) keyText = key.strVal;
                else if (key.kind == MsgPackValue::Kind::Int || key.kind == MsgPackValue::Kind::UInt) keyText = std::format("{}", key.AsInt(-1));
                else continue;
                if (!sampleKeys.empty()) sampleKeys += ",";
                sampleKeys += keyText;
                if (sampleKeys.size() >= 80u) break;
            }
        }
    }

    std::vector<std::pair<std::string, size_t>> distribution(classCounts.begin(), classCounts.end());
    std::sort(distribution.begin(), distribution.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first;
    });
    std::string classSummary;
    for (size_t i = 0; i < std::min<size_t>(distribution.size(), 12u); ++i) {
        if (!classSummary.empty()) classSummary += ", ";
        classSummary += std::format("{}={}", distribution[i].first, distribution[i].second);
    }
    if (classSummary.empty()) classSummary = "<none>";
    if (sampleKeys.empty()) sampleKeys = "<none>";

    std::vector<std::pair<int64_t, size_t>> alternateChannels;
    alternateChannels.reserve(alternateClassChannels.size());
    for (const auto& [channel, values]: alternateClassChannels) {
        size_t count = 0;
        for (const auto& [name, frequency]: values) {
            (void)name;
            count += frequency;
        }
        alternateChannels.emplace_back(channel, count);
    }
    std::sort(alternateChannels.begin(), alternateChannels.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first;
    });
    if (inferredClassNameChannel != nullptr) {
        inferredClassNameChannel->reset();
        const auto nameChannel = PropertyNameChannel("Name");
        for (const auto& [channel, count]: alternateChannels) {
            if (count < 3u) break;
            if (nameChannel && channel == static_cast<int64_t>(*nameChannel)) continue;
            if (channel < 0 || channel > static_cast<int64_t>(std::numeric_limits<uint16_t>::max())) continue;
            *inferredClassNameChannel = static_cast<uint16_t>(channel);
            break;
        }
    }
    std::string alternateSummary;
    for (size_t i = 0; i < std::min<size_t>(alternateChannels.size(), 6u); ++i) {
        const int64_t channel = alternateChannels[i].first;
        std::vector<std::pair<std::string, size_t>> values(alternateClassChannels[channel].begin(), alternateClassChannels[channel].end());
        std::sort(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first;
        });
        std::string channelClasses;
        for (size_t j = 0; j < std::min<size_t>(values.size(), 4u); ++j) {
            if (!channelClasses.empty()) channelClasses += ",";
            channelClasses += std::format("{}={}", values[j].first, values[j].second);
        }
        if (!alternateSummary.empty()) alternateSummary += "; ";
        alternateSummary += std::format("{}:[{}]", channel, channelClasses);
    }
    if (alternateSummary.empty()) alternateSummary = "<none>";

    return std::format("entries={} ClassNameChannel={} numericKeys={} stringKeys={} stringValues={} nonStringValues={} missingKeys={} classes=[{}] alternateClassChannels=[{}] firstEntryKeys=[{}]",
                       entries, channelIndex, numericClassKeys, stringClassKeys, stringValues, nonStringValues, missingClassKeys, classSummary,
                       alternateSummary, sampleKeys);
}

auto IsArray3OfNumbers(const MsgPackValue* value) noexcept -> bool {
    if (value == nullptr || value->kind != MsgPackValue::Kind::Array || value->arrayVal.size() != 3u) return false;
    return std::all_of(value->arrayVal.begin(), value->arrayVal.end(), [](const MsgPackValue& component) {
        return component.kind == MsgPackValue::Kind::Int || component.kind == MsgPackValue::Kind::UInt
               || component.kind == MsgPackValue::Kind::Float;
    });
}

auto MatchesObservedRemotePropertySchema(const MsgPackValue& snapshot, uint16_t classNameChannel) noexcept -> bool {
    if (!snapshot.IsMap()) return false;
    size_t partCount = 0u;
    size_t positionCount = 0u;
    size_t rotationCount = 0u;
    size_t sizeCount = 0u;
    size_t colorCount = 0u;
    size_t anchoredCount = 0u;
    size_t canCollideCount = 0u;
    size_t transparencyCount = 0u;

    for (const auto& [uid, properties]: snapshot.mapVal) {
        (void)uid;
        if (!properties.IsMap()) continue;
        const MsgPackValue* classValue = properties.FindIntKey(classNameChannel);
        if (classValue == nullptr || classValue->kind != MsgPackValue::Kind::String
            || (classValue->strVal != "Part" && classValue->strVal != "MeshPart")) {
            continue;
        }
        ++partCount;
        if (IsArray3OfNumbers(properties.FindIntKey(5))) ++positionCount;
        if (IsArray3OfNumbers(properties.FindIntKey(20))) ++rotationCount;
        if (IsArray3OfNumbers(properties.FindIntKey(21))) ++sizeCount;
        if (IsArray3OfNumbers(properties.FindIntKey(24))) ++colorCount;
        const MsgPackValue* anchored = properties.FindIntKey(22);
        if (anchored != nullptr && anchored->kind == MsgPackValue::Kind::Bool) ++anchoredCount;
        const MsgPackValue* canCollide = properties.FindIntKey(23);
        if (canCollide != nullptr && canCollide->kind == MsgPackValue::Kind::Bool) ++canCollideCount;
        const MsgPackValue* transparency = properties.FindIntKey(25);
        if (transparency != nullptr && transparency->kind == MsgPackValue::Kind::Float) ++transparencyCount;
    }

    if (partCount < 3u) return false;
    const size_t required = (partCount * 4u + 4u) / 5u;
    return positionCount >= required && rotationCount >= required && sizeCount >= required && colorCount >= required
           && anchoredCount >= required && canCollideCount >= required && transparencyCount >= required;
}

constexpr std::array<std::pair<uint16_t, std::string_view>, 22> kObservedRemotePropertyChannels {
    {{5u, "Position"},    {18u, "UserId"},  {19u, "Character"},    {20u, "Rotation"},     {21u, "Size"},         {22u, "Anchored"},
     {23u, "CanCollide"}, {24u, "Color"},   {25u, "Transparency"}, {32u, "NetworkOwner"}, {33u, "Shape"},        {49u, "RootPart"},
     {50u, "State"},      {56u, "Enabled"}, {57u, "Intensity"},    {58u, "Shadows"},      {59u, "ShadowRadius"}, {60u, "SourceRadius"},
     {61u, "Direction"},  {62u, "IsMain"},  {63u, "Range"},        {64u, "Falloff"}}
};

auto BuildConnectPacket(const LaunchConfig& config, uint16_t udpPort) -> MsgPackValue {
    MsgPackValue packet = MsgPackValue::Map();
    packet.Set("action", MsgPackValue::String("connect"));
    packet.Set("userId", MsgPackValue::UInt(config.userId));
    packet.Set("token", MsgPackValue::String(config.token));
    packet.Set("username", MsgPackValue::String(config.username));
    packet.Set("udpPort", MsgPackValue::UInt(udpPort));
    return packet;
}

auto BuildSnapshotReadyPacket() -> MsgPackValue {
    MsgPackValue packet = MsgPackValue::Map();
    packet.Set("action", MsgPackValue::String("snapshotReady"));
    return packet;
}

auto BuildDisconnectPacket() -> MsgPackValue {
    MsgPackValue packet = MsgPackValue::Map();
    packet.Set("action", MsgPackValue::String("disconnect"));
    return packet;
}

auto FloatArray(std::initializer_list<float> values) -> MsgPackValue {
    std::vector<MsgPackValue> array;
    array.reserve(values.size());
    for (float value: values) array.push_back(MsgPackValue::Float(value));
    return MsgPackValue::Array(std::move(array));
}

auto IsReferenceProperty(std::string_view property) noexcept -> bool {
    return property == "Parent" || property == "CurrentCamera" || property == "Part1" || property == "Part2" || property == "NetworkOwner"
           || property == "Character" || property == "RootPart" || property == "PrimaryPart" || property == "Adornee" || property == "ParentShader";
}

auto ReadPropertyName(
    const MsgPackValue& key,
    std::optional<uint16_t> remoteClassNameChannel,
    std::span<const std::string_view, PROPERTY_CHANNEL_COUNT> remotePropertyNameOverrides
) noexcept -> std::string_view {
    if (key.kind == MsgPackValue::Kind::String) return key.strVal;
    const int64_t index = key.AsInt(-1);
    if (index < 0 || index >= static_cast<int64_t>(PROPERTY_CHANNEL_COUNT)) return {};
    const uint16_t channel = static_cast<uint16_t>(index);
    if (remoteClassNameChannel && channel == *remoteClassNameChannel) return "ClassName";
    if (!remotePropertyNameOverrides[channel].empty()) return remotePropertyNameOverrides[channel];
    return PropertyChannelName(channel);
}

struct InstalledSession {
    std::unordered_map<Engine*, ClientSession*> sessions;
};

InstalledSession& InstalledSessions() {
    static InstalledSession installed;
    return installed;
}

void LightClientPrePhysicsStep(Engine& engine, float dt, FrameContext&) {
    auto& installed = InstalledSessions().sessions;
    auto found = installed.find(&engine);
    if (found != installed.end() && found->second != nullptr) found->second->PrePhysicsTick(engine, dt);
}

void LightClientPostPhysicsStep(Engine& engine, float dt, FrameContext&) {
    auto& installed = InstalledSessions().sessions;
    auto found = installed.find(&engine);
    if (found != installed.end() && found->second != nullptr) found->second->PostPhysicsTick(engine, dt);
}

void LightClientSceneCleanup(Engine& engine, bool all) {
    if (!all) return;
    auto& installed = InstalledSessions().sessions;
    auto found = installed.find(&engine);
    if (found != installed.end() && found->second != nullptr) found->second->ResetSceneBindings(engine);
}

void LightClientEngineTeardown(Engine& engine) {
    auto& installed = InstalledSessions().sessions;
    auto found = installed.find(&engine);
    if (found == installed.end()) return;
    if (found->second != nullptr) found->second->OnEngineTeardown(engine);
    installed.erase(found);
}

// The client's two frame steps, declared.
//
// Polling sits after hot-reload dispatch so the frame that receives a snapshot
// is the frame that can act on it, and the send sits after the physics step so
// what leaves is the state the physics just settled on. Both used to be
// inserted by a callback whose fallback appended them at the end of the frame
// when their anchor was missing -- a network poll after present, which is a
// whole frame of latency and a silent one.
constexpr ModuleStep kFrameSteps[] = {
    {
        .phase     = FramePhase::Input,
        .name      = "ProjectLightNetworkPoll",
        .run       = &LightClientPrePhysicsStep,
        .placement = StepPlacement::After,
        .anchor    = "StateAndShaderReload",
    },
    {
        .phase     = FramePhase::Physics,
        .name      = "ProjectLightNetworkSend",
        .run       = &LightClientPostPhysicsStep,
        .placement = StepPlacement::After,
        .anchor    = "PhysicsSystem",
    },
};

} // namespace

// ============================================================================
// TCP / UDP session lifecycle
// ============================================================================

ClientSession::~ClientSession() noexcept {
    Disconnect();
}

void ClientSession::CloseSockets() noexcept {
    m_tcpSocket.Close();
    m_udpSocket.Close();
    m_serverEndpoint = {};
    m_connected = false;
    m_snapshotComplete = false;
    m_realtimeOverTcp = false;
    m_realtimeModeReceived = false;
    m_localUdpPort = 0;
    m_tcpRxBuffer.clear();
    m_tcpTxBuffer.clear();
    m_tcpTxOffset = 0;
}

auto ClientSession::Connect(const LaunchConfig& config) -> std::expected<void, std::string> {
    Disconnect();
    if (config.host.empty()) return std::unexpected("project-light server host is not configured");
    if (config.port == 0) return std::unexpected("project-light server port is not configured");
    m_config = config;
    m_snapshotComplete = false;
    m_streamErrorLogged = false;
    m_realtimeModeReceived = false;
    m_networkPollStarted = false;
    m_sceneSyncSummaryLogged = false;
    m_snapshotClassProbeLogged = false;
    m_remoteClassNameChannel.reset();
    m_remotePropertyNameOverrides.fill(std::string_view {});
    m_lastNetworkStatusLog = {};
    m_tcpBytesSent = 0u;
    m_tcpBytesReceived = 0u;
    m_tcpFramesReceived = 0u;
    m_udpBytesSent = 0u;
    m_udpBytesReceived = 0u;
    m_udpDatagramsSent = 0u;
    m_udpDatagramsReceived = 0u;
    m_udpDecodeErrors = 0u;
    m_unrecognizedPackets = 0u;
    m_playerControllerStates.clear();
    m_sendAccumulator = 0.0f;

    Log("[ProjectLight][net] opening TCP connection to {}:{} (authentication values are not logged).", config.host, config.port);

    auto addresses = Net::ResolveSocketEndpoints(
        config.host, config.port, Net::SocketAddressFamily::IPv4, Net::SocketType::Stream
    );
    if (!addresses) {
        return std::unexpected(std::format(
            "could not resolve server host '{}' (resolver error {})", config.host, addresses.error().nativeCode
        ));
    }

    Net::Socket      tcp;
    std::string      connectError = "no IPv4 address was returned";
    for (const Net::SocketEndpoint& endpoint: *addresses) {
        auto candidate = Net::OpenSocket(Net::SocketType::Stream, endpoint.family);
        if (!candidate) {
            connectError = SocketErrorText("TCP socket", candidate.error().nativeCode);
            continue;
        }

        auto connection = candidate->Connect(endpoint, std::chrono::milliseconds(kConnectTimeoutMs));
        if (!connection) {
            connectError = SocketErrorText("connect", connection.error().nativeCode);
            continue;
        }

        auto noDelay = candidate->SetNoDelay(true);
        if (!noDelay) {
            LogWarning("[ProjectLight][net] TCP_NODELAY could not be set (socket error {}).", noDelay.error().nativeCode);
        }

        tcp = std::move(*candidate);
        m_serverEndpoint = endpoint;
        break;
    }
    if (!tcp.IsOpen()) {
        CloseSockets();
        return std::unexpected(connectError);
    }

    auto openedUdp = Net::OpenSocket(Net::SocketType::Datagram, m_serverEndpoint.family);
    if (!openedUdp) {
        const std::string error = SocketErrorText("create UDP socket", openedUdp.error().nativeCode);
        CloseSockets();
        return std::unexpected(error);
    }
    Net::Socket udp = std::move(*openedUdp);

    auto nonblocking = udp.SetNonBlocking(true);
    if (!nonblocking) {
        const std::string error = SocketErrorText("set nonblocking UDP", nonblocking.error().nativeCode);
        CloseSockets();
        return std::unexpected(error);
    }

    auto bound = udp.Bind(Net::SocketEndpoint::IPv4Any(0));
    if (!bound) {
        const std::string error = SocketErrorText("bind UDP socket", bound.error().nativeCode);
        CloseSockets();
        return std::unexpected(error);
    }

    auto localEndpoint = udp.LocalEndpoint();
    if (!localEndpoint) {
        const std::string error = SocketErrorText("query UDP port", localEndpoint.error().nativeCode);
        CloseSockets();
        return std::unexpected(error);
    }

    m_tcpSocket = std::move(tcp);
    m_udpSocket = std::move(udp);
    m_localUdpPort = localEndpoint->port;
    m_connected = true;

    Log("[ProjectLight][net] TCP transport connected to {}:{}; local UDP port {}.", config.host, config.port, m_localUdpPort);
    if (!SendTcpPacket(BuildConnectPacket(config, m_localUdpPort))) {
        CloseSockets();
        return std::unexpected("send connect packet failed");
    }
    FlushTcpTxBuffer();
    const size_t pendingBytes = m_tcpTxBuffer.size() - std::min(m_tcpTxOffset, m_tcpTxBuffer.size());
    Log("[ProjectLight][net] connect request submitted (TCP bytes sent={}, pending={}); waiting for a server reply; TCP connect alone does not confirm protocol acceptance.",
        m_tcpBytesSent, pendingBytes);
    return {};
}

void ClientSession::Disconnect() noexcept {
    if (m_connected) {
        (void)SendTcpPacket(BuildDisconnectPacket());
        FlushTcpTxBuffer();
    }
    CloseSockets();
}

auto ClientSession::SendTcpPacket(const MsgPackValue& packet) -> bool {
    if (!m_connected || !m_tcpSocket.IsOpen()) return false;
    auto frame = EncodeStreamFrame(packet, false);
    if (!frame) {
        Log("[ProjectLight] TCP packet encode failed: {}", frame.error());
        return false;
    }
    if (m_tcpTxOffset == m_tcpTxBuffer.size()) {
        m_tcpTxBuffer.clear();
        m_tcpTxOffset = 0;
    }
    m_tcpTxBuffer.insert(m_tcpTxBuffer.end(), frame->begin(), frame->end());
    FlushTcpTxBuffer();
    return m_connected;
}

void ClientSession::FlushTcpTxBuffer() noexcept {
    if (!m_connected || !m_tcpSocket.IsOpen()) return;
    while (m_tcpTxOffset < m_tcpTxBuffer.size()) {
        const size_t remaining = m_tcpTxBuffer.size() - m_tcpTxOffset;
        const auto   bytes = std::span<const uint8_t>(m_tcpTxBuffer.data() + m_tcpTxOffset, remaining);
        auto         result = m_tcpSocket.Send(bytes);
        if (!result) {
            Log("[ProjectLight] TCP send failed (socket error {}); disconnecting.", result.error().nativeCode);
            CloseSockets();
            return;
        }
        if (result->state == Net::SocketIOState::WouldBlock) {
            break;
        }
        if (result->state != Net::SocketIOState::Complete || result->bytes == 0) {
            Log("[ProjectLight] TCP send made no progress; disconnecting.");
            CloseSockets();
            return;
        }
        m_tcpTxOffset += result->bytes;
        m_tcpBytesSent += static_cast<uint64_t>(result->bytes);
    }
    if (m_tcpTxOffset == m_tcpTxBuffer.size()) {
        m_tcpTxBuffer.clear();
        m_tcpTxOffset = 0;
    }
}

auto ClientSession::SendRealtimePacket(const MsgPackValue& packet) -> bool {
    if (!m_connected) return false;
    const std::vector<uint8_t> packed = EncodeMsgPack(packet);
    if (m_realtimeOverTcp || packed.size() > MAX_SAFE_UDP_PAYLOAD_BYTES) return SendTcpPacket(packet);
    if (!m_udpSocket.IsOpen() || !m_serverEndpoint.IsValid()) return false;

    auto result = m_udpSocket.SendTo(std::span<const uint8_t>(packed), m_serverEndpoint);
    if (!result) {
        Log("[ProjectLight] UDP send failed (socket error {}).", result.error().nativeCode);
        return false;
    }
    if (result->state == Net::SocketIOState::WouldBlock) {
        return false;
    }

    const bool complete = result->state == Net::SocketIOState::Complete && result->bytes == packed.size();
    if (complete) {
        ++m_udpDatagramsSent;
        m_udpBytesSent += static_cast<uint64_t>(packed.size());
    }
    return complete;
}

// ============================================================================
// DataModel replication
// ============================================================================

auto ClientSession::FindObject(uint64_t uid) const noexcept -> const ReplicatedObject* {
    const auto found = m_objects.find(uid);
    return found == m_objects.end() ? nullptr : &found->second;
}

void ClientSession::ApplySnapshotEntry(uint64_t uid, const MsgPackValue& properties) {
    if (uid == 0 || !properties.IsMap()) return;
    ReplicatedObject& object = m_objects[uid];
    object.uid = uid;
    object.removalPending = false;
    ApplyProperties(object, properties);
}

void ClientSession::ApplySnapshotMap(const MsgPackValue& snapshot) {
    if (!snapshot.IsMap()) return;
    for (const auto& [key, properties]: snapshot.mapVal) {
        const uint64_t uid = key.AsUid();
        if (uid == 0 || !properties.IsMap()) continue;
        ReplicatedObject& object = m_objects[uid];
        object.uid = uid;
        object.removalPending = false;
    }
    for (const auto& [key, properties]: snapshot.mapVal) {
        const uint64_t uid = key.AsUid();
        if (uid != 0) ApplySnapshotEntry(uid, properties);
    }
}

void ClientSession::ApplyProperties(ReplicatedObject& object, const MsgPackValue& properties) {
    if (!properties.IsMap()) return;
    for (const auto& [key, value]: properties.mapVal) {
        const std::string_view property = ReadPropertyName(key, m_remoteClassNameChannel, m_remotePropertyNameOverrides);
        if (property.empty()) continue;
        if (property == "ClassName") {
            object.className = value.AsString(object.className);
        } else if (property == "Name") {
            object.name = value.AsString(object.name);
        } else if (IsReferenceProperty(property)) {
            const uint64_t reference = ReadReference(value);
            if (property == "Parent") object.parentUid = reference;
            else if (property == "Part1") object.part1Uid = reference;
            else if (property == "Part2") object.part2Uid = reference;
            else if (property == "NetworkOwner") {
                if (object.networkOwnerUid != reference) {
                    object.networkOwnerUid = reference;
                    object.physicsHistory.Clear();
                    object.hasPhysicsSample = false;
                    object.renderPosition = object.position;
                    object.renderRotation = object.rotationQuat;
                    object.transformDirty = true;
                }
            }
            else if (property == "Character") object.characterUid = reference;
            else if (property == "RootPart") object.rootPartUid = reference;
            else if (property == "PrimaryPart") object.primaryPartUid = reference;
        } else if (property == "Position") {
            object.position = value.AsVec3(VEC3_SCALE, object.position);
            object.physicsHistory.Clear();
            object.hasPhysicsSample = false;
            object.transformDirty = true;
        } else if (property == "Rotation") {
            object.rotationEulerDeg = value.AsVec3(VEC3_SCALE, object.rotationEulerDeg);
            object.rotationQuat     = Math::EulerDegreesToQuat(object.rotationEulerDeg).Normalized();
            object.physicsHistory.Clear();
            object.hasPhysicsSample = false;
            object.transformDirty = true;
        } else if (property == "Size") {
            object.size = value.AsVec3(VEC3_SCALE, object.size);
            object.visualDirty = true;
        } else if (property == "Color") {
            object.color = value.AsVec3(COLOR3_SCALE, object.color);
            object.visualDirty = true;
        } else if (property == "Transparency") {
            object.transparency = std::clamp(value.AsFloat(object.transparency), 0.0f, 1.0f);
            object.visualDirty = true;
        } else if (property == "Metallic") {
            object.metallic = std::clamp(value.AsFloat(object.metallic), 0.0f, 1.0f);
            object.visualDirty = true;
        } else if (property == "Roughness") {
            object.roughness = std::clamp(value.AsFloat(object.roughness), 0.0f, 1.0f);
            object.visualDirty = true;
        } else if (property == "Emission") {
            object.emission = std::max(0.0f, value.AsFloat(object.emission));
            object.visualDirty = true;
        } else if (property == "Anchored") {
            object.anchored = value.AsBool(object.anchored);
            object.visualDirty = true;
        } else if (property == "CanCollide") {
            object.canCollide = value.AsBool(object.canCollide);
            object.visualDirty = true;
        } else if (property == "Shape") {
            object.shape = PartShapeFromValue(value.AsInt(static_cast<int64_t>(object.shape)));
            object.visualDirty = true;
        } else if (property == "MeshId") {
            object.meshId = value.AsString(object.meshId);
            object.visualDirty = true;
        } else if (property == "MeshScale") {
            object.meshScale = value.AsVec3(VEC3_SCALE, object.meshScale);
            object.visualDirty = true;
        } else if (property == "UserId") {
            object.userId = value.AsUInt(object.userId);
        } else if (property == "State") {
            object.humanoidState = StateFromValue(value.AsInt(static_cast<int64_t>(object.humanoidState)));
        } else if (property == "WalkSpeed") {
            object.walkSpeed = value.AsFloat(object.walkSpeed);
        } else if (property == "JumpPower") {
            object.jumpPower = value.AsFloat(object.jumpPower);
        } else if (property == "Health") {
            object.health = value.AsFloat(object.health);
        } else if (property == "MaxHealth") {
            object.maxHealth = value.AsFloat(object.maxHealth);
        } else if (property == "Offset1") {
            object.offset1 = value.AsVec3(VEC3_SCALE, object.offset1);
        } else if (property == "Offset2") {
            object.offset2 = value.AsVec3(VEC3_SCALE, object.offset2);
        } else if (property == "RotationOffset1") {
            object.rotationOffset1 = value.AsVec3(VEC3_SCALE, object.rotationOffset1);
        } else if (property == "RotationOffset2") {
            object.rotationOffset2 = value.AsVec3(VEC3_SCALE, object.rotationOffset2);
        } else if (property == "CurrentAngle") {
            object.currentAngle = value.AsFloat(object.currentAngle);
        } else if (property == "DesiredAngle") {
            object.desiredAngle = value.AsFloat(object.desiredAngle);
        } else if (property == "MaxVelocity") {
            object.maxVelocity = value.AsFloat(object.maxVelocity);
        } else if (property == "Enabled") {
            object.enabled = value.AsBool(object.enabled);
            object.visualDirty = true;
        } else if (property == "Intensity") {
            object.intensity = value.AsFloat(object.intensity);
            object.visualDirty = true;
        } else if (property == "Shadows") {
            object.shadows = value.AsBool(object.shadows);
            object.visualDirty = true;
        } else if (property == "SourceRadius") {
            object.sourceRadius = value.AsFloat(object.sourceRadius);
        } else if (property == "Direction") {
            object.direction = value.AsVec3(VEC3_SCALE, object.direction);
            object.visualDirty = true;
        } else if (property == "NightColor") {
            object.nightColor = value.AsVec3(COLOR3_SCALE, object.nightColor);
            object.visualDirty = true;
        } else if (property == "NightIntensity") {
            object.nightIntensity = value.AsFloat(object.nightIntensity);
            object.visualDirty = true;
        } else if (property == "IsMain") {
            object.isMain = value.AsBool(object.isMain);
            object.visualDirty = true;
        } else if (property == "Range") {
            object.range = value.AsFloat(object.range);
            object.visualDirty = true;
        } else if (property == "Ambient") {
            // LightingService.Ambient is a scalar in ProjectLight, not Color3.
            object.ambient = value.AsFloat(object.ambient);
        } else if (property == "TimeOfDay") {
            object.timeOfDay = value.AsFloat(object.timeOfDay);
        } else if (property == "Latitude") {
            object.latitude = value.AsFloat(object.latitude);
        } else if (property == "ProceduralSky") {
            object.proceduralSky = value.AsBool(object.proceduralSky);
        } else if (property == "ServerAuthority") {
            object.serverAuthority = value.AsBool(object.serverAuthority);
            m_serverAuthority = object.serverAuthority;
            object.visualDirty = true;
        } else if (property == "SoundId") {
            object.soundId = value.AsString(object.soundId);
        } else if (property == "Volume") {
            object.volume = std::clamp(value.AsFloat(object.volume), 0.0f, 10.0f);
        } else if (property == "Loops") {
            object.loops = value.AsBool(object.loops);
        } else if (property == "Playing") {
            object.playing = value.AsBool(object.playing);
        }
    }

    if (object.className == "DataModel") m_dataModelUid = object.uid;
    if (object.className == "PhysicsService") m_serverAuthority = object.serverAuthority;
}

void ClientSession::ApplyTreeUpdates(const MsgPackValue& treeUpdates) {
    const MsgPackValue* updates = treeUpdates.Find("updates");
    if (updates != nullptr && updates->IsMap()) {
        for (const auto& [key, properties]: updates->mapVal) {
            const uint64_t uid = key.AsUid();
            if (uid == 0 || !properties.IsMap()) continue;
            ReplicatedObject& object = m_objects[uid];
            object.uid = uid;
            object.removalPending = false;
        }
        for (const auto& [key, properties]: updates->mapVal) {
            const uint64_t uid = key.AsUid();
            if (uid != 0) ApplySnapshotEntry(uid, properties);
        }
    }

    const MsgPackValue* removals = treeUpdates.Find("removals");
    if (removals != nullptr && removals->IsArray()) {
        for (const MsgPackValue& entry: removals->arrayVal) {
            const uint64_t uid = entry.AsUid();
            if (uid != 0) DeleteObjectSubtree(uid);
        }
    }
}

void ClientSession::ApplyPhysicsUpdates(const MsgPackValue& physicsUpdates) {
    const MsgPackValue* physics = physicsUpdates.Find("physics");
    if (physics == nullptr || !physics->IsMap()) return;
    const auto receivedAt = std::chrono::steady_clock::now();
    for (const auto& [key, value]: physics->mapVal) {
        const uint64_t uid = key.AsUid();
        if (uid == 0 || !value.IsArray() || value.arrayVal.size() < 10) continue;
        ReplicatedObject* object = nullptr;
        if (auto found = m_objects.find(uid); found != m_objects.end()) object = &found->second;
        if (object == nullptr || !IsPartClass(object->className)) continue;
        if (m_localPlayerUid != 0 && object->networkOwnerUid == m_localPlayerUid) continue;
        if (IsPartOfCharacter(uid, m_localCharacterUid)) continue;

        const auto& data = value.arrayVal;
        auto scaled = [](const MsgPackValue& component) { return component.AsFloat() / VEC3_SCALE; };
        const JPH::Vec3 position(scaled(data[0]), scaled(data[1]), scaled(data[2]));
        const JPH::Quat rotation = JPH::Quat(scaled(data[3]), scaled(data[4]), scaled(data[5]), scaled(data[6])).Normalized();
        object->physicsHistory.Push(NetworkTransformSample {.position = position, .rotation = rotation, .receivedAt = receivedAt});
        object->position = position;
        object->rotationQuat = rotation;
        object->rotationEulerDeg = Math::QuatToEulerDegrees(object->rotationQuat);
        object->linearVelocity   = JPH::Vec3(scaled(data[7]), scaled(data[8]), scaled(data[9]));
        object->hasPhysicsSample = true;
        object->transformDirty = true;
    }
}

void ClientSession::ApplyControllerState(const MsgPackValue& packet) {
    if (!packet.IsArray() || packet.arrayVal.size() < 2) return;
    const uint64_t userId = packet.arrayVal[0].AsUid();
    const MsgPackValue& state = packet.arrayVal[1];
    if (userId == 0 || !state.IsArray() || state.arrayVal.size() < 10) return;

    NetworkTransformHistory& history = m_playerControllerStates[userId];
    history.Push(NetworkTransformSample {
        .position = JPH::Vec3(state.arrayVal[0].AsFloat(), state.arrayVal[1].AsFloat(), state.arrayVal[2].AsFloat()),
        .rotation = JPH::Quat(
            state.arrayVal[3].AsFloat(), state.arrayVal[4].AsFloat(), state.arrayVal[5].AsFloat(), state.arrayVal[6].AsFloat()
        ).Normalized(),
        .receivedAt = std::chrono::steady_clock::now()
    });
}

void ClientSession::ApplySoundUpdates(const MsgPackValue& packet) {
    const MsgPackValue* updates = packet.Find("updates");
    if (updates == nullptr || !updates->IsMap()) return;
    for (const auto& [key, update]: updates->mapVal) {
        const uint64_t uid = key.AsUid();
        if (uid == 0) continue;
        auto found = m_objects.find(uid);
        if (found == m_objects.end() || found->second.className != "Sound") continue;
        const MsgPackValue* type = update.Find("type");
        if (type == nullptr) continue;
        if (type->AsString() == "play") found->second.playing = true;
        else if (type->AsString() == "stop") found->second.playing = false;
    }
}

void ClientSession::DeleteObjectSubtree(uint64_t uid) {
    auto found = m_objects.find(uid);
    if (found == m_objects.end() || found->second.removalPending) return;
    found->second.removalPending = true;
    m_pendingRemovals.push_back(uid);
    std::vector<uint64_t> children;
    for (const auto& [childUid, object]: m_objects) {
        if (object.parentUid == uid && !object.removalPending) children.push_back(childUid);
    }
    for (uint64_t child: children) DeleteObjectSubtree(child);
}

void ClientSession::RefreshServiceLinks() noexcept {
    m_workspaceUid = 0;
    m_playersServiceUid = 0;
    m_lightingUid = 0;
    m_physicsServiceUid = 0;
    for (const auto& [uid, object]: m_objects) {
        if (object.className == "WorkspaceService" || object.name == "Workspace") m_workspaceUid = uid;
        else if (object.className == "PlayersService" || object.name == "Players") m_playersServiceUid = uid;
        else if (object.className == "LightingService" || object.name == "Lighting") m_lightingUid = uid;
        else if (object.className == "PhysicsService" || object.name == "Physics") {
            m_physicsServiceUid = uid;
            m_serverAuthority = object.serverAuthority;
        }
    }
}

void ClientSession::RefreshPlayerLinks() noexcept {
    m_localPlayerUid = 0;
    m_localCharacterUid = 0;
    m_localHumanoidUid = 0;
    m_localRootPartUid = 0;
    for (const auto& [uid, object]: m_objects) {
        if (object.className == "Player" && object.userId == m_config.userId) {
            m_localPlayerUid = uid;
            m_localCharacterUid = object.characterUid;
            break;
        }
    }
    if (m_localCharacterUid == 0) return;
    for (const auto& [uid, object]: m_objects) {
        if (object.parentUid != m_localCharacterUid) continue;
        if (object.className == "Humanoid") {
            m_localHumanoidUid = uid;
            m_localRootPartUid = object.rootPartUid;
            break;
        }
    }
    if (m_localRootPartUid == 0) {
        for (const auto& [uid, object]: m_objects) {
            if (object.parentUid == m_localCharacterUid && object.name == "Torso" && IsPartClass(object.className)) {
                m_localRootPartUid = uid;
                break;
            }
        }
    }
}

// Both of these are record queries, so Replication/ReplicaState.hpp owns them
// and the mirror asks the same questions the session does.
auto ClientSession::IsPartOfCharacter(uint64_t partUid, uint64_t characterUid) const noexcept -> bool {
    return ProjectLight::IsPartOfCharacter(m_objects, partUid, characterUid);
}

auto ClientSession::IsLocalCharacterPart(uint64_t uid) const noexcept -> bool {
    return ProjectLight::IsLocalCharacterPart(m_objects, uid, m_localCharacterUid);
}

auto ClientSession::MirrorState() noexcept -> SceneMirrorState {
    return SceneMirrorState {
        .objects            = m_objects,
        .controllerStates   = m_playerControllerStates,
        .pendingRemovals    = m_pendingRemovals,
        .index              = m_index,
        .localUserId        = m_config.userId,
        .localPlayerUid     = m_localPlayerUid,
        .localCharacterUid  = m_localCharacterUid,
        .localHumanoidUid   = m_localHumanoidUid,
        .localRootPartUid   = m_localRootPartUid,
        .lightingUid        = m_lightingUid,
        .workspaceUid       = m_workspaceUid,
        .serverAuthority    = m_serverAuthority,
        .snapshotComplete   = m_snapshotComplete,
        .time               = m_time,
        .localController    = m_localController,
        .localControllerBody = m_localControllerBody,
        .controllerForRootUid = m_controllerForRootUid,
        .jumpRemaining      = m_jumpRemaining,
        .wasJumpDown        = m_wasJumpDown,
        .summaryLogged      = m_sceneSyncSummaryLogged,
    };
}

auto ClientSession::HandlePacket(const MsgPackValue& packet) -> bool {
    if (!packet.IsMap()) return false;
    bool handled = false;
    if (const MsgPackValue* snapshot = packet.Find("initialDataModelSnapshot")) {
        ApplySnapshotMap(*snapshot);
        handled = true;
    }
    if (const MsgPackValue* snapshot = packet.Find("initialServicesSnapshot")) {
        ApplySnapshotMap(*snapshot);
        handled = true;
    }
    if (const MsgPackValue* snapshot = packet.Find("initialObjectsSnapshot")) {
        ApplySnapshotMap(*snapshot);
        handled = true;
    }
    if (const MsgPackValue* updates = packet.Find("treeUpdates")) {
        ApplyTreeUpdates(*updates);
        handled = true;
    }
    if (const MsgPackValue* physics = packet.Find("physicsUpdates")) {
        ApplyPhysicsUpdates(*physics);
        handled = true;
    }
    if (const MsgPackValue* state = packet.Find("controllerState")) {
        ApplyControllerState(*state);
        handled = true;
    }
    if (const MsgPackValue* sounds = packet.Find("soundUpdates")) {
        ApplySoundUpdates(*sounds);
        handled = true;
    }
    if (handled) {
        RefreshServiceLinks();
        RefreshPlayerLinks();
    }
    return handled;
}

// ============================================================================
// Network event pump
// ============================================================================

void ClientSession::PollNetwork(Engine& engine) {
    if (!m_connected) return;
    const auto now = std::chrono::steady_clock::now();
    if (!m_networkPollStarted) {
        m_networkPollStarted = true;
        m_lastNetworkStatusLog = now;
        Log("[ProjectLight][net] frame poll is active; waiting for the first server reply.");
    }

    FlushTcpTxBuffer();
    if (!m_connected) return;

    std::array<uint8_t, kReceiveChunkBytes> buffer {};
    auto logStatus = [&]() {
        size_t partCount = 0;
        size_t lightCount = 0;
        size_t boundEntityCount = 0;
        for (const auto& [uid, object]: m_objects) {
            if (IsPartClass(object.className)) ++partCount;
            if (IsLightClass(object.className)) ++lightCount;
            if (m_index.Find(uid) != Entity::Null()) ++boundEntityCount;
        }
        const std::string_view realtimeMode = !m_realtimeModeReceived ? "awaiting server mode"
                                                                       : (m_realtimeOverTcp ? "TCP" : "UDP");
        Log("[ProjectLight][net] status: TCP transportConnected={} tx={}B rx={}B frames={} buffered={}B; UDP tx={}/{}B rx={}/{}B decodeErrors={}; mode={} snapshot={} objects={} parts={} lights={} boundEntities={} unrecognizedPackets={}",
            m_connected, m_tcpBytesSent, m_tcpBytesReceived, m_tcpFramesReceived, m_tcpRxBuffer.size(), m_udpDatagramsSent,
            m_udpBytesSent, m_udpDatagramsReceived, m_udpBytesReceived, m_udpDecodeErrors, realtimeMode, m_snapshotComplete,
            m_objects.size(), partCount, lightCount, boundEntityCount, m_unrecognizedPackets);
        m_lastNetworkStatusLog = std::chrono::steady_clock::now();
    };

    auto drainFrames = [&]() -> bool {
        while (m_tcpRxBuffer.size() >= 4) {
            const auto length = PeekStreamBodyLength(m_tcpRxBuffer);
            if (!length) break;
            if (*length == 0 || *length > MAX_STREAM_FRAME_BYTES) {
                Log("[ProjectLight] invalid TCP frame length; disconnecting.");
                CloseSockets();
                return false;
            }
            const size_t frameSize = *length + 4;
            if (m_tcpRxBuffer.size() < frameSize) break;
            auto packet = DecodeStreamFrame(std::span<const uint8_t>(m_tcpRxBuffer.data(), frameSize));
            m_tcpRxBuffer.erase(m_tcpRxBuffer.begin(), m_tcpRxBuffer.begin() + static_cast<std::ptrdiff_t>(frameSize));
            if (!packet) {
                Log("[ProjectLight] malformed TCP frame: {}; disconnecting.", packet.error());
                CloseSockets();
                return false;
            }

            const uint64_t frameNumber = ++m_tcpFramesReceived;
            if (!m_snapshotClassProbeLogged) {
                if (const MsgPackValue* snapshot = packet->Find("initialObjectsSnapshot")) {
                    std::optional<uint16_t> inferredClassNameChannel;
                    Log("[ProjectLight][net] raw initialObjectsSnapshot class probe: {}",
                        SnapshotClassProbe(*snapshot, &inferredClassNameChannel));
                    if (inferredClassNameChannel) {
                        m_remoteClassNameChannel = inferredClassNameChannel;
                        const auto compiledClassNameChannel = PropertyNameChannel("ClassName");
                        Log("[ProjectLight][net] inferred remote ClassName channel {}; compiled property table expects {}.",
                            *m_remoteClassNameChannel, compiledClassNameChannel.value_or(0u));
                        m_remotePropertyNameOverrides.fill(std::string_view {});
                        if (compiledClassNameChannel && *m_remoteClassNameChannel != *compiledClassNameChannel
                            && MatchesObservedRemotePropertySchema(*snapshot, *m_remoteClassNameChannel)) {
                            for (const auto& [channel, property]: kObservedRemotePropertyChannels) {
                                m_remotePropertyNameOverrides[channel] = property;
                            }
                            Log("[ProjectLight][net] audited remote property layout matched; mapped Position=5, Rotation=20, Size=21, Anchored=22, CanCollide=23, Color=24, Transparency=25, UserId=18, Character=19, NetworkOwner=32, Shape=33, RootPart=49, State=50, and light fields=56-64.");
                        } else if (compiledClassNameChannel && *m_remoteClassNameChannel != *compiledClassNameChannel) {
                            LogWarning("[ProjectLight][net] no known numeric property layout matched this snapshot; only ClassName will use a remote override.");
                        }
                    } else {
                        m_remoteClassNameChannel.reset();
                        m_remotePropertyNameOverrides.fill(std::string_view {});
                        Log("[ProjectLight][net] no alternate ClassName channel inferred; using the compiled property table.");
                    }
                    m_snapshotClassProbeLogged = true;
                }
            }
            const std::string_view action = GetFrameAction(*packet);
            if (action == "realtimeMode") {
                const MsgPackValue* modeValue = packet->Find("mode");
                const std::string mode = modeValue != nullptr ? modeValue->AsString() : std::string {};
                const bool recognizedMode = mode == "tcp" || mode == "udp";
                m_realtimeModeReceived = recognizedMode;
                m_realtimeOverTcp = mode == "tcp";
                if (recognizedMode) {
                    Log("[ProjectLight][net] server selected {} for realtime traffic.", mode);
                } else {
                    ++m_unrecognizedPackets;
                    LogWarning("[ProjectLight][net] server sent an unknown realtimeMode value '{}'.", mode);
                }
            } else if (action == "snapshotEnd") {
                const bool firstSnapshotEnd = !m_snapshotComplete;
                m_snapshotComplete = true;
                if (firstSnapshotEnd) {
                    size_t partCount = 0;
                    size_t lightCount = 0;
                    for (const auto& [uid, object]: m_objects) {
                        (void)uid;
                        if (IsPartClass(object.className)) ++partCount;
                        if (IsLightClass(object.className)) ++lightCount;
                    }
                    Log("[ProjectLight][net] snapshotEnd received: {} replicated objects ({} parts/mesh parts, {} lights); sending snapshotReady.",
                        m_objects.size(), partCount, lightCount);
                }
                if (SendTcpPacket(BuildSnapshotReadyPacket())) {
                    if (firstSnapshotEnd) Log("[ProjectLight][net] snapshotReady sent/queued on TCP.");
                } else {
                    LogWarning("[ProjectLight][net] could not send snapshotReady.");
                }
            } else if (action == "disconnect") {
                Log("[ProjectLight] server requested disconnect.");
                CloseSockets();
                return false;
            } else {
                const bool handled = HandlePacket(*packet);
                if (!handled) {
                    ++m_unrecognizedPackets;
                    if (m_unrecognizedPackets <= 8u) {
                        LogWarning("[ProjectLight][net] unhandled TCP packet #{}: action='{}', fields={}", m_unrecognizedPackets,
                                   action.empty() ? "<none>" : std::string(action), NetworkPacketSummary(*packet));
                    }
                }
            }

            if (frameNumber <= 12u) {
                Log("[ProjectLight][net] RX TCP frame #{} ({} bytes): action='{}', {}; replicatedObjects={}", frameNumber, frameSize,
                    action.empty() ? "<none>" : std::string(action), NetworkPacketSummary(*packet), m_objects.size());
            }
            if (!m_connected) return false;
        }
        return true;
    };

    while (true) {
        auto received = m_tcpSocket.Receive(std::span<uint8_t>(buffer));
        if (!received) {
            const int error = received.error().nativeCode;
            if (!m_streamErrorLogged) Log("[ProjectLight] TCP receive failed (socket error {}).", error);
            m_streamErrorLogged = true;
            CloseSockets();
            return;
        }
        if (received->state == Net::SocketIOState::WouldBlock) break;
        if (received->state == Net::SocketIOState::PeerClosed) {
            if (!m_streamErrorLogged) Log("[ProjectLight] server closed the TCP connection.");
            m_streamErrorLogged = true;
            CloseSockets();
            return;
        }
        const size_t receivedBytes = received->bytes;
        if (receivedBytes == 0) break;

        m_tcpBytesReceived += static_cast<uint64_t>(receivedBytes);
        m_tcpRxBuffer.insert(
            m_tcpRxBuffer.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(receivedBytes)
        );
        if (m_tcpRxBuffer.size() > MAX_STREAM_FRAME_BYTES + 4 + kReceiveChunkBytes || !drainFrames()) {
            if (m_connected) {
                Log("[ProjectLight] incoming TCP frame buffer exceeds the configured limit; disconnecting.");
                CloseSockets();
            }
            return;
        }
    }
    if (!drainFrames()) return;

    if (m_udpSocket.IsOpen()) {
        std::array<uint8_t, 65536> datagram {};
        while (true) {
            auto received = m_udpSocket.ReceiveFrom(std::span<uint8_t>(datagram));
            if (!received) {
                Log("[ProjectLight] UDP receive failed (socket error {}).", received.error().nativeCode);
                break;
            }
            if (received->io.state == Net::SocketIOState::WouldBlock) break;
            if (received->io.state != Net::SocketIOState::Complete) break;
            const size_t receivedBytes = received->io.bytes;
            if (receivedBytes == 0) continue;

            ++m_udpDatagramsReceived;
            m_udpBytesReceived += static_cast<uint64_t>(receivedBytes);
            auto packet = DecodeMsgPack(std::span<const uint8_t>(datagram.data(), receivedBytes));
            if (!packet) {
                ++m_udpDecodeErrors;
                if (m_udpDecodeErrors <= 4u) {
                    LogWarning("[ProjectLight][net] malformed UDP datagram #{} ({} bytes): {}", m_udpDatagramsReceived,
                               receivedBytes, packet.error());
                }
                continue;
            }
            if (!packet->IsMap()) {
                ++m_udpDecodeErrors;
                if (m_udpDecodeErrors <= 4u) {
                    LogWarning("[ProjectLight][net] UDP datagram #{} decoded to a non-map MessagePack value.", m_udpDatagramsReceived);
                }
                continue;
            }
            const bool handled = HandlePacket(*packet);
            if (!handled) {
                ++m_unrecognizedPackets;
                if (m_unrecognizedPackets <= 8u) {
                    const std::string_view action = GetFrameAction(*packet);
                    LogWarning("[ProjectLight][net] unhandled UDP packet #{}: action='{}', fields={}", m_unrecognizedPackets,
                               action.empty() ? "<none>" : std::string(action), NetworkPacketSummary(*packet));
                }
            }
            if (m_udpDatagramsReceived <= 3u) {
                Log("[ProjectLight][net] RX UDP datagram #{} ({} bytes): {}; replicatedObjects={}", m_udpDatagramsReceived,
                    receivedBytes, NetworkPacketSummary(*packet), m_objects.size());
            }
        }
    }
    FlushTcpTxBuffer();

    const auto statusNow = std::chrono::steady_clock::now();
    if (m_lastNetworkStatusLog == std::chrono::steady_clock::time_point {}
        || statusNow - m_lastNetworkStatusLog >= std::chrono::seconds(5)) {
        logStatus();
    }
    (void)engine;
}

void ClientSession::PrePhysicsTick(Engine& engine, float dt) {
    if (!m_connected) return;
    m_time += std::clamp(dt, 0.0f, 0.25f);
    PollNetwork(engine);
    if (!m_connected) return;
    RefreshServiceLinks();
    RefreshPlayerLinks();
    // One view for both passes: they are two halves of the same reconcile, and
    // the second one relies on what the first just wrote.
    auto scene = MirrorState();
    m_mirror.UpdateLocalController(engine, scene);
    m_mirror.SyncSceneEntities(engine, scene);
}

// ============================================================================
// Outbound state, and the view the scene half reads
// ============================================================================

void ClientSession::SendRealtimeFrame(Engine& engine) {
    if (!m_connected || !m_snapshotComplete) return;
    auto& registry = engine.GetRegistry();
    auto input = registry.GetSingleton<Components::InputStateComponent>();
    m_lastControls = {};
    // No main camera entity means no view to report; the zeroed controls above stand.
    if (const auto cameraComp = registry.GetSingleton<Components::CameraComponent>()) {
        m_lastControls.yaw = cameraComp->camera.yaw;
    }
    if (input) {
        m_lastControls.forward  = input->IsKeyDown(static_cast<uint8_t>(KeyCode::W));
        m_lastControls.backward = input->IsKeyDown(static_cast<uint8_t>(KeyCode::S));
        m_lastControls.left     = input->IsKeyDown(static_cast<uint8_t>(KeyCode::A));
        m_lastControls.right    = input->IsKeyDown(static_cast<uint8_t>(KeyCode::D));
        m_lastControls.jump     = input->IsKeyDown(static_cast<uint8_t>(KeyCode::Space));
    }

    MsgPackValue inputs = MsgPackValue::Map();
    inputs.Set("forward", MsgPackValue::Bool(m_lastControls.forward));
    inputs.Set("backward", MsgPackValue::Bool(m_lastControls.backward));
    inputs.Set("left", MsgPackValue::Bool(m_lastControls.left));
    inputs.Set("right", MsgPackValue::Bool(m_lastControls.right));
    inputs.Set("jump", MsgPackValue::Bool(m_lastControls.jump));
    inputs.Set("yaw", MsgPackValue::Float(m_lastControls.yaw));

    MsgPackValue owned = MsgPackValue::Map();
    MsgPackValue ownedTransforms = MsgPackValue::Map();
    if (m_localController != Entity::Null() && m_localControllerBody != Physics::BodyHandle::Null()) {
        auto& physics = engine.GetPhysicsContext();
        const auto movement = registry.Get<Character::MovementComponent>(m_localController);
        if (const auto bodyState = physics.BodyState(m_localControllerBody); bodyState && bodyState->valid) {
            const auto& state = *bodyState;
            const JPH::Quat rotation = movement ? movement->orientation.Normalized() : state.currentRotation.Normalized();
            const JPH::Vec3 velocity = physics.GetCharacterVelocity(m_localControllerBody);
            owned.Set("controllerState", FloatArray({
                state.currentPosition.GetX(), state.currentPosition.GetY(), state.currentPosition.GetZ(),
                rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW(),
                velocity.GetX(), velocity.GetY(), velocity.GetZ()
            }));
            HumanoidState humanoidState = HumanoidState::Idle;
            if (movement) {
                if (!movement->isGrounded) humanoidState = m_jumpRemaining > 0.0f ? HumanoidState::Jumping : HumanoidState::Freefall;
                else if (std::abs(movement->inputX) + std::abs(movement->inputZ) > 0.05f) humanoidState = HumanoidState::Running;
            }
            owned.Set("humanoidState", MsgPackValue::Int(static_cast<int64_t>(humanoidState)));
        }
    }

    auto& physics = engine.GetPhysicsContext();
    for (auto& [uid, object]: m_objects) {
        if (m_localPlayerUid == 0 || object.removalPending || !IsPartClass(object.className)
            || object.networkOwnerUid != m_localPlayerUid) continue;
        TransformState transform {.position = object.position, .rotation = object.rotationQuat, .linearVelocity = object.linearVelocity};
        bool haveState = false;
        if (IsLocalCharacterPart(uid)) {
            haveState = true;
        } else if (const Entity entity = m_index.Find(uid); entity != Entity::Null() && registry.IsAlive(entity)) {
            const auto body = registry.Get<Components::PhysicsComponent>(entity);
            if (body) {
                if (const auto bodyState = physics.BodyState(body->physicsHandle); bodyState && bodyState->valid) {
                    const auto& snapshot = *bodyState;
                    transform.position = snapshot.currentPosition;
                    transform.rotation = snapshot.currentRotation;
                    const JPH::BodyID bodyId = Physics::GetBodyID(physics.GetInternalWorld(), body->physicsHandle);
                    if (!bodyId.IsInvalid()) transform.linearVelocity = physics.GetInternalSystem().GetBodyInterface().GetLinearVelocity(bodyId);
                    haveState = true;
                }
            }
        }
        if (!haveState) continue;

        // Keep our authoritative mirror current while we own the body. If the
        // server hands ownership back, the render-only entity can resume from
        // this pose instead of snapping back to an old remote sample.
        object.position = transform.position;
        object.rotationQuat = transform.rotation.Normalized();
        object.rotationEulerDeg = Math::QuatToEulerDegrees(object.rotationQuat);
        object.linearVelocity   = transform.linearVelocity;
        object.renderPosition   = object.position;
        object.renderRotation   = object.rotationQuat;
        object.physicsHistory.Clear();
        object.hasPhysicsSample = false;

        MsgPackValue key = MsgPackValue::UInt(uid);
        MsgPackValue value = FloatArray({
            transform.position.GetX(), transform.position.GetY(), transform.position.GetZ(),
            transform.rotation.GetX(), transform.rotation.GetY(), transform.rotation.GetZ(), transform.rotation.GetW(),
            transform.linearVelocity.GetX(), transform.linearVelocity.GetY(), transform.linearVelocity.GetZ()
        });
        ownedTransforms.mapVal.emplace_back(std::move(key), std::move(value));
    }
    owned.Set("ownedTransforms", std::move(ownedTransforms));

    MsgPackValue packet = MsgPackValue::Map();
    packet.Set("inputs", std::move(inputs));
    packet.Set("ownedState", std::move(owned));
    (void)SendRealtimePacket(packet);
}

void ClientSession::PostPhysicsTick(Engine& engine, float dt) {
    if (!m_connected) return;
    auto scene = MirrorState();
    m_mirror.UpdateLocalCharacterParts(engine, scene, dt);
    m_sendAccumulator += std::clamp(dt, 0.0f, 0.25f);
    if (m_sendAccumulator < kSendRateSeconds) return;
    m_sendAccumulator = std::fmod(m_sendAccumulator, kSendRateSeconds);
    SendRealtimeFrame(engine);
}

void ClientSession::ResetSceneBindings(Engine& engine) noexcept {
    for (auto& [uid, object]: m_objects) {
        (void)uid;
        if (object.audioHandle != AudioHandle::Invalid) {
            engine.GetAudioContext().StopVoice(object.audioHandle);
            object.audioHandle = AudioHandle::Invalid;
        }
        // The whole scene is going away, so every binding in it is stale by
        // definition; the mirror keeps its records and re-spawns from them.
        object.materialId = InvalidMaterialID;
        object.hasPhysicsBody = false;
        object.spawnedSize = JPH::Vec3::sZero();
        object.visualDirty = true;
        object.transformDirty = true;
    }
    m_index.Clear();
    if (m_localControllerBody != Physics::BodyHandle::Null()) {
        engine.GetPhysicsContext().DestroyBody(m_localControllerBody);
        m_localControllerBody = Physics::BodyHandle::Null();
    }
    m_localController = Entity::Null();
    m_controllerForRootUid = 0;
    m_playerControllerStates.clear();
}

void ClientSession::OnEngineTeardown(Engine& engine) noexcept {
    Disconnect();
    ResetSceneBindings(engine);
}

auto DescribeClient() -> Module {
    return Module{
        .name               = "ProjectLightClient",
        .registerComponents = &RegisterReplicationComponents,
        .frameSteps         = kFrameSteps,
        .sceneCleanup       = &LightClientSceneCleanup,
        .teardown           = &LightClientEngineTeardown,
    };
}

void InstallClient(Engine& engine, ClientSession& session) {
    // The session is process state the engine is not allowed to own (it is the
    // application's, and it outlives the engine on the editor's restart path),
    // so the binding is registered here and the module's hooks look it up.
    InstalledSessions().sessions[&engine] = &session;
    if (auto installed = engine.InstallModule(DescribeClient()); !installed) {
        LogError("[ProjectLight][lifecycle] module install failed: {}", ToString(installed.error()));
        return;
    }
    Log("[ProjectLight][lifecycle] client frame polling, scene-cleanup, and teardown hooks installed.");
}

} // namespace ZHLN::ProjectLight
