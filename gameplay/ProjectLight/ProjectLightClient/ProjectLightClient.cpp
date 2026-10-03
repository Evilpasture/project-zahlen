// gameplay/ProjectLight/ProjectLightClient/ProjectLightClient.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(_WIN32)
#include <cstddef>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <ProjectLightClient/LightProtocol.hpp>

#include <CharacterController/CharacterComponents.hpp>
#include <Camera/TargetCamera.hpp>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/FrameScheduler.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ZHLN::ProjectLight {
namespace {

constexpr int kConnectTimeoutMs = 5000;
constexpr size_t kReceiveChunkBytes = 64 * 1024;
constexpr float kSendRateSeconds = 1.0f / 60.0f;
// Bias toward responsiveness: keep a short remote playout delay to smooth
// 60 Hz arrivals without making moving props feel as far behind as 40 ms.
constexpr auto kNetworkSmoothDelay = std::chrono::milliseconds(20);
constexpr float kLocalJumpScale = 0.40435f; // sqrt(32 / project-light's 196.2 studs/s^2)

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

auto AsNativeSocket(intptr_t socket) noexcept -> NativeSocket { return static_cast<NativeSocket>(socket); }
auto LastSocketError() noexcept -> int { return WSAGetLastError(); }
auto IsInterrupted(int error) noexcept -> bool { return error == WSAEINTR; }
auto IsWouldBlock(int error) noexcept -> bool { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
auto IsConnectInProgress(int error) noexcept -> bool { return IsWouldBlock(error); }
void CloseNativeSocket(NativeSocket socket) noexcept { if (socket != kInvalidSocket) closesocket(socket); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

auto AsNativeSocket(intptr_t socket) noexcept -> NativeSocket { return static_cast<NativeSocket>(socket); }
auto LastSocketError() noexcept -> int { return errno; }
auto IsInterrupted(int error) noexcept -> bool { return error == EINTR; }
auto IsWouldBlock(int error) noexcept -> bool {
#if EAGAIN == EWOULDBLOCK
    return error == EAGAIN;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}
auto IsConnectInProgress(int error) noexcept -> bool { return error == EINPROGRESS || IsWouldBlock(error); }
void CloseNativeSocket(NativeSocket socket) noexcept { if (socket != kInvalidSocket) close(socket); }
#endif

auto SetNonBlocking(NativeSocket socket) noexcept -> bool {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

auto WaitWritable(NativeSocket socket, int timeoutMs) noexcept -> bool {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket, &writable);
    timeval timeout {.tv_sec = timeoutMs / 1000, .tv_usec = (timeoutMs % 1000) * 1000};
#if defined(_WIN32)
    return select(0, nullptr, &writable, nullptr, &timeout) > 0;
#else
    if (socket >= FD_SETSIZE) return false;
    return select(socket + 1, nullptr, &writable, nullptr, &timeout) > 0;
#endif
}

auto SocketErrorText(std::string_view operation, int error) -> std::string {
    return std::format("{} failed (socket error {})", operation, error);
}

auto AsSocketAddress(const std::array<uint8_t, 128>& storage) noexcept -> const sockaddr* {
    return reinterpret_cast<const sockaddr*>(storage.data());
}

auto AsSocketAddress(std::array<uint8_t, 128>& storage) noexcept -> sockaddr* {
    return reinterpret_cast<sockaddr*>(storage.data());
}

auto ReadReference(const MsgPackValue& value) noexcept -> uint64_t {
    return value.IsNil() ? 0u : value.AsUid();
}

auto IsPartClass(std::string_view className) noexcept -> bool {
    return className == "Part" || className == "MeshPart";
}

auto SameVec3(JPH::Vec3Arg lhs, JPH::Vec3Arg rhs) noexcept -> bool {
    return lhs.GetX() == rhs.GetX() && lhs.GetY() == rhs.GetY() && lhs.GetZ() == rhs.GetZ();
}

auto IsLightClass(std::string_view className) noexcept -> bool {
    return className == "DirectionalLight" || className == "PointLight";
}

auto PartShapeFromValue(int64_t value) noexcept -> PartShape {
    if (value < 0 || value > static_cast<int64_t>(PartShape::Head)) return PartShape::Cube;
    return static_cast<PartShape>(value);
}

auto StateFromValue(int64_t value) noexcept -> HumanoidState {
    if (value < 0 || value > static_cast<int64_t>(HumanoidState::GettingUp)) return HumanoidState::Idle;
    return static_cast<HumanoidState>(value);
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

constexpr std::array<std::pair<uint16_t, std::string_view>, 22> kObservedRemotePropertyChannels {{
    {5u, "Position"}, {18u, "UserId"}, {19u, "Character"}, {20u, "Rotation"}, {21u, "Size"},
    {22u, "Anchored"}, {23u, "CanCollide"}, {24u, "Color"}, {25u, "Transparency"}, {32u, "NetworkOwner"},
    {33u, "Shape"}, {49u, "RootPart"}, {50u, "State"}, {56u, "Enabled"}, {57u, "Intensity"},
    {58u, "Shadows"}, {59u, "ShadowRadius"}, {60u, "SourceRadius"}, {61u, "Direction"}, {62u, "IsMain"},
    {63u, "Range"}, {64u, "Falloff"}
}};

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
    const std::array<std::string_view, PROPERTY_CHANNEL_COUNT>& remotePropertyNameOverrides
) noexcept -> std::string_view {
    if (key.kind == MsgPackValue::Kind::String) return key.strVal;
    const int64_t index = key.AsInt(-1);
    if (index < 0 || index >= static_cast<int64_t>(PROPERTY_CHANNEL_COUNT)) return {};
    const uint16_t channel = static_cast<uint16_t>(index);
    if (remoteClassNameChannel && channel == *remoteClassNameChannel) return "ClassName";
    if (!remotePropertyNameOverrides[channel].empty()) return remotePropertyNameOverrides[channel];
    return PropertyChannelName(channel);
}

auto CharacterOffsetForName(std::string_view name) noexcept -> JPH::Vec3 {
    if (name == "Head") return JPH::Vec3(0.0f, 1.5f, 0.0f);
    if (name == "Left Arm") return JPH::Vec3(-1.5f, 0.0f, 0.0f);
    if (name == "Right Arm") return JPH::Vec3(1.5f, 0.0f, 0.0f);
    if (name == "Left Leg") return JPH::Vec3(-0.5f, -2.0f, 0.0f);
    if (name == "Right Leg") return JPH::Vec3(0.5f, -2.0f, 0.0f);
    return JPH::Vec3::sZero();
}

auto IsCharacterLimb(std::string_view name) noexcept -> bool {
    return name == "Right Arm" || name == "Left Arm" || name == "Right Leg" || name == "Left Leg";
}

auto LimbPoseAngle(std::string_view partName, HumanoidState state, float time, float walkSpeed) noexcept -> float {
    if (partName == "Head" || partName == "Torso") return 0.0f;
    if (state == HumanoidState::Jumping || state == HumanoidState::Freefall) {
        return (partName == "Right Arm" || partName == "Left Arm") ? -JPH::JPH_PI : 0.0f;
    }
    if (state == HumanoidState::FallingDown) {
        return (partName == "Right Arm" || partName == "Left Arm") ? -2.2f : 0.0f;
    }
    if (state == HumanoidState::GettingUp) {
        return (partName == "Right Arm" || partName == "Left Arm") ? -0.5f : 0.0f;
    }
    const float swing = state == HumanoidState::Running ? std::sin(time * (walkSpeed * 0.6f)) * 0.6f : std::sin(time * 0.75f) * 0.1f;
    if (partName == "Right Arm" || partName == "Left Leg") return swing;
    if (partName == "Left Arm" || partName == "Right Leg") return -swing;
    return 0.0f;
}

void WriteTransform(Engine& engine, ReplicatedObject& object, JPH::Vec3 position, JPH::Quat rotation) {
    object.position = position;
    object.rotationQuat = rotation.Normalized();
    object.renderPosition = object.position;
    object.renderRotation = object.rotationQuat;
    object.rotationEulerDeg = Math::QuatToEulerDegrees(object.rotationQuat);
    object.transformDirty = true;
    if (object.entity == Entity::Null()) return;
    auto& registry = engine.GetRegistry();
    if (auto transform = registry.Get<Components::TransformComponent>(object.entity)) {
        transform->position = object.position;
        transform->rotation = object.rotationQuat;
    }
}

void WriteInterpolatedTransform(Engine& engine, ReplicatedObject& object, JPH::Vec3 position, JPH::Quat rotation) {
    // Keep the authoritative network pose intact; only the render pose is
    // delayed/interpolated. Physics snapshots use object.position/rotationQuat.
    object.renderPosition = position;
    object.renderRotation = rotation.Normalized();
    object.transformDirty = true;
    if (object.entity == Entity::Null()) return;
    auto& registry = engine.GetRegistry();
    if (auto transform = registry.Get<Components::TransformComponent>(object.entity)) {
        transform->position = object.renderPosition;
        transform->rotation = object.renderRotation;
        object.transformDirty = false;
    }
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

void AddLightClientFrameSteps(FrameScheduler& scheduler) {
    if (!scheduler.InsertAfter("ScriptAndShaderReload", FramePhase::Input, "ProjectLightNetworkPoll", &LightClientPrePhysicsStep)) {
        scheduler.Add(FramePhase::Input, "ProjectLightNetworkPoll", &LightClientPrePhysicsStep);
    }
    if (!scheduler.InsertAfter("PhysicsSystem", FramePhase::Physics, "ProjectLightNetworkSend", &LightClientPostPhysicsStep)) {
        scheduler.Add(FramePhase::Physics, "ProjectLightNetworkSend", &LightClientPostPhysicsStep);
    }
}

} // namespace

void NetworkTransformHistory::Push(const NetworkTransformSample& sample) noexcept {
    m_samples[m_next] = sample;
    m_next = (m_next + 1) % Capacity;
    m_count = std::min(m_count + 1, Capacity);
}

auto NetworkTransformHistory::Sample(
    std::chrono::steady_clock::time_point renderTime,
    JPH::Vec3& position,
    JPH::Quat& rotation
) const noexcept -> bool {
    if (Empty()) return false;

    const std::size_t oldest = (m_next + Capacity - m_count) % Capacity;
    auto sampleAt = [&](std::size_t offset) -> const NetworkTransformSample& {
        return m_samples[(oldest + offset) % Capacity];
    };

    const NetworkTransformSample& first = sampleAt(0);
    const NetworkTransformSample& last = sampleAt(m_count - 1);
    if (renderTime <= first.receivedAt) {
        position = first.position;
        rotation = first.rotation;
        return true;
    }
    if (renderTime >= last.receivedAt) {
        position = last.position;
        rotation = last.rotation;
        return true;
    }

    for (std::size_t i = 1; i < m_count; ++i) {
        const NetworkTransformSample& current = sampleAt(i);
        if (renderTime > current.receivedAt) continue;

        const NetworkTransformSample& previous = sampleAt(i - 1);
        const auto span = current.receivedAt - previous.receivedAt;
        float alpha = 0.0f;
        if (span > std::chrono::steady_clock::duration::zero()) {
            alpha = std::chrono::duration<float>(renderTime - previous.receivedAt).count()
                    / std::chrono::duration<float>(span).count();
        }
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        position = previous.position + (current.position - previous.position) * alpha;
        rotation = previous.rotation.SLERP(current.rotation, alpha).Normalized();
        return true;
    }

    position = last.position;
    rotation = last.rotation;
    return true;
}

// ============================================================================
// TCP / UDP session lifecycle
// ============================================================================

ClientSession::~ClientSession() noexcept {
    Disconnect();
}

void ClientSession::CloseSockets() noexcept {
    const NativeSocket tcp = AsNativeSocket(m_tcpSocket);
    const NativeSocket udp = AsNativeSocket(m_udpSocket);
    CloseNativeSocket(tcp);
    CloseNativeSocket(udp);
    m_tcpSocket = -1;
    m_udpSocket = -1;
    m_connected = false;
    m_snapshotComplete = false;
    m_realtimeOverTcp = false;
    m_realtimeModeReceived = false;
    m_localUdpPort = 0;
    m_tcpRxBuffer.clear();
    m_tcpTxBuffer.clear();
    m_tcpTxOffset = 0;
#if defined(_WIN32)
    if (m_winsockStarted) {
        WSACleanup();
        m_winsockStarted = false;
    }
#endif
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

#if defined(_WIN32)
    WSADATA wsaData {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return std::unexpected("WSAStartup failed");
    m_winsockStarted = true;
#endif

    addrinfo hints {};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const std::string portText = std::to_string(config.port);
    addrinfo* addresses = nullptr;
    const int resolveResult = getaddrinfo(config.host.c_str(), portText.c_str(), &hints, &addresses);
    if (resolveResult != 0 || addresses == nullptr) {
        CloseSockets();
#if defined(_WIN32)
        return std::unexpected(std::format("could not resolve server host '{}' (error {})", config.host, resolveResult));
#else
        return std::unexpected(std::format("could not resolve server host '{}': {}", config.host, gai_strerror(resolveResult)));
#endif
    }

    NativeSocket tcp = kInvalidSocket;
    sockaddr_in serverAddress {};
    std::string connectError = "no IPv4 address was returned";
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        if (address->ai_family != AF_INET || address->ai_addrlen > sizeof serverAddress) continue;
        NativeSocket candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (candidate == kInvalidSocket) {
            connectError = SocketErrorText("TCP socket", LastSocketError());
            continue;
        }
        if (!SetNonBlocking(candidate)) {
            connectError = SocketErrorText("set nonblocking TCP", LastSocketError());
            CloseNativeSocket(candidate);
            continue;
        }

        int rc = connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if (rc != 0) {
            const int error = LastSocketError();
            if (!IsConnectInProgress(error) || !WaitWritable(candidate, kConnectTimeoutMs)) {
                connectError = SocketErrorText("connect", error);
                CloseNativeSocket(candidate);
                continue;
            }
            int socketError = 0;
#if defined(_WIN32)
            int errorLength = sizeof socketError;
#else
            socklen_t errorLength = sizeof socketError;
#endif
            if (getsockopt(candidate, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &errorLength) != 0 || socketError != 0) {
                connectError = SocketErrorText("connect", socketError == 0 ? LastSocketError() : socketError);
                CloseNativeSocket(candidate);
                continue;
            }
        }

        tcp = candidate;
        std::memcpy(&serverAddress, address->ai_addr, sizeof serverAddress);
        break;
    }
    freeaddrinfo(addresses);
    if (tcp == kInvalidSocket) {
        CloseSockets();
        return std::unexpected(connectError);
    }

    int noDelay = 1;
    setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof noDelay);

    NativeSocket udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == kInvalidSocket || !SetNonBlocking(udp)) {
        const std::string error = SocketErrorText("create UDP socket", LastSocketError());
        CloseNativeSocket(tcp);
        CloseNativeSocket(udp);
        CloseSockets();
        return std::unexpected(error);
    }

    sockaddr_in bindAddress {};
    bindAddress.sin_family      = AF_INET;
    bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddress.sin_port        = htons(0);
    if (bind(udp, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof bindAddress) != 0) {
        const std::string error = SocketErrorText("bind UDP socket", LastSocketError());
        CloseNativeSocket(tcp);
        CloseNativeSocket(udp);
        CloseSockets();
        return std::unexpected(error);
    }

    sockaddr_in localAddress {};
#if defined(_WIN32)
    int localAddressLength = sizeof localAddress;
#else
    socklen_t localAddressLength = sizeof localAddress;
#endif
    if (getsockname(udp, reinterpret_cast<sockaddr*>(&localAddress), &localAddressLength) != 0) {
        const std::string error = SocketErrorText("query UDP port", LastSocketError());
        CloseNativeSocket(tcp);
        CloseNativeSocket(udp);
        CloseSockets();
        return std::unexpected(error);
    }

    m_tcpSocket = static_cast<intptr_t>(tcp);
    m_udpSocket = static_cast<intptr_t>(udp);
    m_serverAddress.fill(0);
    std::memcpy(m_serverAddress.data(), &serverAddress, sizeof serverAddress);
    m_serverAddressBytes = sizeof serverAddress;
    m_localUdpPort = ntohs(localAddress.sin_port);
    m_connected = true;

    Log("[ProjectLight][net] TCP transport connected to {}:{}; local UDP port {}.", config.host, config.port, m_localUdpPort);
    if (!SendTcpPacket(BuildConnectPacket(config, m_localUdpPort))) {
        const std::string error = SocketErrorText("send connect packet", LastSocketError());
        CloseSockets();
        return std::unexpected(error);
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
    if (!m_connected || m_tcpSocket == -1) return false;
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
    if (!m_connected || m_tcpSocket == -1) return;
    const NativeSocket tcp = AsNativeSocket(m_tcpSocket);
    while (m_tcpTxOffset < m_tcpTxBuffer.size()) {
        const size_t remaining = m_tcpTxBuffer.size() - m_tcpTxOffset;
#if defined(_WIN32)
        const int sent = send(tcp, reinterpret_cast<const char*>(m_tcpTxBuffer.data() + m_tcpTxOffset), static_cast<int>(remaining), 0);
#else
        const ssize_t sent = send(tcp, m_tcpTxBuffer.data() + m_tcpTxOffset, remaining, 0);
#endif
        if (sent > 0) {
            m_tcpTxOffset += static_cast<size_t>(sent);
            m_tcpBytesSent += static_cast<uint64_t>(sent);
            continue;
        }
        if (sent < 0 && IsInterrupted(LastSocketError())) continue;
        if (sent < 0 && IsWouldBlock(LastSocketError())) break;
        Log("[ProjectLight] TCP send failed; disconnecting.");
        CloseSockets();
        return;
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
    if (m_udpSocket == -1 || m_serverAddressBytes == 0) return false;
    const NativeSocket udp = AsNativeSocket(m_udpSocket);
#if defined(_WIN32)
    const int sent = sendto(udp, reinterpret_cast<const char*>(packed.data()), static_cast<int>(packed.size()), 0,
                           AsSocketAddress(m_serverAddress), static_cast<int>(m_serverAddressBytes));
#else
    const ssize_t sent = sendto(udp, packed.data(), packed.size(), 0, AsSocketAddress(m_serverAddress), static_cast<socklen_t>(m_serverAddressBytes));
#endif
    if (sent < 0 && !IsWouldBlock(LastSocketError()) && !IsInterrupted(LastSocketError())) {
        Log("[ProjectLight] UDP send failed (socket error {}).", LastSocketError());
        return false;
    }
    const bool complete = sent >= 0 && static_cast<size_t>(sent) == packed.size();
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
            object.rotationQuat = Math::EulerDegreesToQuat(object.rotationEulerDeg).Normalized();
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
        } else if (property == "IsMain") {
            object.isMain = value.AsBool(object.isMain);
            object.visualDirty = true;
        } else if (property == "Range") {
            object.range = value.AsFloat(object.range);
            object.visualDirty = true;
        } else if (property == "Ambient") {
            object.ambient = value.AsVec3(COLOR3_SCALE, object.ambient);
        } else if (property == "TimeOfDay") {
            object.timeOfDay = value.AsFloat(object.timeOfDay);
        } else if (property == "Latitude") {
            object.latitude = value.AsFloat(object.latitude);
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
        object->linearVelocity = JPH::Vec3(scaled(data[7]), scaled(data[8]), scaled(data[9]));
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

auto ClientSession::IsPartOfCharacter(uint64_t partUid, uint64_t characterUid) const noexcept -> bool {
    if (partUid == 0 || characterUid == 0) return false;
    uint64_t parentUid = partUid;
    size_t depth = 0;
    while (parentUid != 0 && depth++ < 64) {
        const auto found = m_objects.find(parentUid);
        if (found == m_objects.end()) return false;
        parentUid = found->second.parentUid;
        if (parentUid == characterUid) return true;
    }
    return false;
}

auto ClientSession::IsLocalCharacterPart(uint64_t uid) const noexcept -> bool {
    return m_localCharacterUid != 0 && IsPartOfCharacter(uid, m_localCharacterUid);
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

    const NativeSocket tcp = AsNativeSocket(m_tcpSocket);
    std::array<uint8_t, kReceiveChunkBytes> buffer {};
    auto logStatus = [&]() {
        size_t partCount = 0;
        size_t lightCount = 0;
        size_t boundEntityCount = 0;
        for (const auto& [uid, object]: m_objects) {
            (void)uid;
            if (IsPartClass(object.className)) ++partCount;
            if (IsLightClass(object.className)) ++lightCount;
            if (object.entity != Entity::Null()) ++boundEntityCount;
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
#if defined(_WIN32)
        const int received = recv(tcp, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
        const ssize_t received = recv(tcp, buffer.data(), buffer.size(), 0);
#endif
        if (received > 0) {
            m_tcpBytesReceived += static_cast<uint64_t>(received);
            m_tcpRxBuffer.insert(m_tcpRxBuffer.end(), buffer.begin(), buffer.begin() + received);
            if (m_tcpRxBuffer.size() > MAX_STREAM_FRAME_BYTES + 4 + kReceiveChunkBytes || !drainFrames()) {
                if (m_connected) {
                    Log("[ProjectLight] incoming TCP frame buffer exceeds the configured limit; disconnecting.");
                    CloseSockets();
                }
                return;
            }
            continue;
        }
        if (received == 0) {
            if (!m_streamErrorLogged) Log("[ProjectLight] server closed the TCP connection.");
            m_streamErrorLogged = true;
            CloseSockets();
            return;
        }
        const int error = LastSocketError();
        if (IsInterrupted(error)) continue;
        if (IsWouldBlock(error)) break;
        if (!m_streamErrorLogged) Log("[ProjectLight] TCP receive failed (socket error {}).", error);
        m_streamErrorLogged = true;
        CloseSockets();
        return;
    }
    if (!drainFrames()) return;

    if (m_udpSocket != -1) {
        const NativeSocket udp = AsNativeSocket(m_udpSocket);
        std::array<uint8_t, 65536> datagram {};
        while (true) {
#if defined(_WIN32)
            const int received = recvfrom(udp, reinterpret_cast<char*>(datagram.data()), static_cast<int>(datagram.size()), 0, nullptr, nullptr);
#else
            const ssize_t received = recvfrom(udp, datagram.data(), datagram.size(), 0, nullptr, nullptr);
#endif
            if (received > 0) {
                ++m_udpDatagramsReceived;
                m_udpBytesReceived += static_cast<uint64_t>(received);
                auto packet = DecodeMsgPack(std::span<const uint8_t>(datagram.data(), static_cast<size_t>(received)));
                if (!packet) {
                    ++m_udpDecodeErrors;
                    if (m_udpDecodeErrors <= 4u) {
                        LogWarning("[ProjectLight][net] malformed UDP datagram #{} ({} bytes): {}", m_udpDatagramsReceived,
                                   received, packet.error());
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
                        received, NetworkPacketSummary(*packet), m_objects.size());
                }
                continue;
            }
            if (received == 0) break;
            const int error = LastSocketError();
            if (IsInterrupted(error)) continue;
            if (IsWouldBlock(error)) break;
            Log("[ProjectLight] UDP receive failed (socket error {}).", error);
            break;
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
    UpdateLocalController(engine);
    SyncSceneEntities(engine);
}

// ============================================================================
// ECS object and service synchronization
// ============================================================================

auto ClientSession::ShouldCreatePhysics(const ReplicatedObject& object) const noexcept -> bool {
    if (!IsPartClass(object.className) || IsLocalCharacterPart(object.uid)) return false;
    if (object.anchored) return object.canCollide;
    return !m_serverAuthority && m_localPlayerUid != 0 && object.networkOwnerUid == m_localPlayerUid;
}

void ClientSession::SyncSceneEntities(Engine& engine) {
    auto& registry = engine.GetRegistry();

    for (uint64_t uid: m_pendingRemovals) {
        auto found = m_objects.find(uid);
        if (found == m_objects.end()) continue;
        ReplicatedObject& object = found->second;
        if (object.audioHandle != AudioHandle::Invalid) {
            engine.GetAudioContext().StopVoice(object.audioHandle);
            object.audioHandle = AudioHandle::Invalid;
        }
        if (object.entity != Entity::Null() && registry.IsAlive(object.entity)) {
            DespawnEntity(engine, object.entity);
        }
        m_playerControllerStates.erase(object.userId);
        m_objects.erase(found);
    }
    m_pendingRemovals.clear();

    const auto renderTime = std::chrono::steady_clock::now() - kNetworkSmoothDelay;
    for (auto& [uid, object]: m_objects) {
        (void)uid;
        object.renderPosition = object.position;
        object.renderRotation = object.rotationQuat;
        const bool locallyOwned = m_localPlayerUid != 0 && object.networkOwnerUid == m_localPlayerUid;
        if (locallyOwned) {
            object.physicsHistory.Clear();
            object.hasPhysicsSample = false;
        } else if (object.hasPhysicsSample) {
            (void)object.physicsHistory.Sample(renderTime, object.renderPosition, object.renderRotation);
        }
        if (object.hasPhysicsSample) object.transformDirty = true;
    }
    for (auto& [uid, object]: m_objects) {
        (void)uid;
        if (object.removalPending) continue;
        if (IsPartClass(object.className)) SpawnOrUpdatePartEntity(engine, object);
        else if (IsLightClass(object.className)) SpawnOrUpdateLightEntity(engine, object);
    }

    UpdateRemoteCharacterParts(engine, 0.0f);
    SyncEnvironment(engine);
    UpdateSoundVoices(engine);

    if (m_snapshotComplete && !m_sceneSyncSummaryLogged) {
        size_t partCount = 0;
        size_t lightCount = 0;
        size_t boundEntityCount = 0;
        size_t liveEntityCount = 0;
        for (const auto& [uid, object]: m_objects) {
            (void)uid;
            if (IsPartClass(object.className)) ++partCount;
            if (IsLightClass(object.className)) ++lightCount;
            if (object.entity != Entity::Null()) {
                ++boundEntityCount;
                if (registry.IsAlive(object.entity)) ++liveEntityCount;
            }
        }
        Log("[ProjectLight][scene] initial sync after snapshot: objects={} parts/meshParts={} lights={} boundEntities={} liveEntities={} workspaceUid={} localPlayerUid={} localRootPartUid={}",
            m_objects.size(), partCount, lightCount, boundEntityCount, liveEntityCount, m_workspaceUid, m_localPlayerUid, m_localRootPartUid);
        std::unordered_map<std::string, size_t> classCounts;
        for (const auto& [uid, object]: m_objects) {
            (void)uid;
            ++classCounts[object.className.empty() ? "<empty>" : object.className];
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
        Log("[ProjectLight][scene] parsed ClassName distribution: {}", classSummary.empty() ? "<none>" : classSummary);
        if (partCount == 0 && lightCount == 0) {
            LogWarning("[ProjectLight][scene] snapshot completed but contains no renderable Part/MeshPart or light objects.");
        } else if (liveEntityCount == 0) {
            LogWarning("[ProjectLight][scene] replicated renderables exist, but no live ECS render entities were created.");
        }
        m_sceneSyncSummaryLogged = true;
    }
}

void ClientSession::SpawnOrUpdatePartEntity(Engine& engine, ReplicatedObject& object) {
    auto& registry = engine.GetRegistry();
    const JPH::Vec3 size = JPH::Vec3(
        std::clamp(std::abs(object.size.GetX()), 0.001f, 100000.0f),
        std::clamp(std::abs(object.size.GetY()), 0.001f, 100000.0f),
        std::clamp(std::abs(object.size.GetZ()), 0.001f, 100000.0f)
    );
    const bool createPhysics = ShouldCreatePhysics(object);
    const bool blendMaterial = object.transparency > 0.001f;
    const bool liveEntity = object.entity != Entity::Null() && registry.IsAlive(object.entity)
                            && !registry.Get<Components::PendingDestroy>(object.entity).has_value();
    if (!liveEntity && object.entity != Entity::Null()) object.entity = Entity::Null();

    const bool rebuild = !liveEntity || !SameVec3(object.spawnedSize, size) || object.spawnedShape != object.shape
                         || object.spawnedAnchored != object.anchored || object.spawnedCanCollide != object.canCollide
                         || object.spawnedOwnerUid != object.networkOwnerUid || object.hasPhysicsBody != createPhysics
                         || object.spawnedBlend != blendMaterial;
    if (rebuild) {
        if (liveEntity) {
            DespawnEntity(engine, object.entity);
            object.entity = Entity::Null();
        }

        PrefabFactory::SpawnParams params;
        // Physics bodies must start from the latest authoritative pose, not a
        // delayed render sample. This matters when NetworkOwner changes.
        params.position         = JPH::RVec3(createPhysics ? object.position : object.renderPosition);
        params.rotation         = (createPhysics ? object.rotationQuat : object.renderRotation).Normalized();
        params.scale            = size;
        params.createPhysics    = createPhysics;
        params.isStaticPhysics  = object.anchored;
        params.physicsMask      = object.canCollide ? 0xFFFFFFFFu : 0u;
        params.color            = JPH::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
        params.roughness        = object.roughness;
        params.metallic         = object.metallic;

        auto material = engine.GetRenderContext().CreateBasicMaterial(false, blendMaterial, false, false);
        if (material) {
            material->baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f};
            material->roughnessFactor = object.roughness;
            material->metallicFactor  = object.metallic;
            params.materialOverride   = *material;
        } else {
            Log("[ProjectLight] Could not create material for '{}': {}", object.name, material.error());
        }

        const JPH::Vec3 halfExtents = JPH::Vec3::sReplicate(0.5f);
        switch (object.shape) {
            case PartShape::Sphere:
            case PartShape::Head:
                object.entity = PrefabFactory::CreateSphere(engine, 0.5f, params);
                break;
            case PartShape::Cylinder:
                object.entity = PrefabFactory::CreateCylinder(engine, 0.5f, 1.0f, params);
                break;
            case PartShape::Cone:
                object.entity = PrefabFactory::CreateCone(engine, 0.5f, 1.0f, params);
                break;
            case PartShape::Cube:
            case PartShape::Wedge:
            case PartShape::CornerWedge:
            case PartShape::Capsule:
            default:
                // Wedge, CornerWedge, Capsule and MeshPart asset substitution are
                // intentionally represented by the nearest built-in primitive.
                object.entity = PrefabFactory::CreateBox(engine, halfExtents, params);
                break;
        }

        if (object.entity == Entity::Null() || !registry.IsAlive(object.entity)) {
            object.materialId = InvalidMaterialID;
            object.hasPhysicsBody = false;
            return;
        }

        registry.Patch<Components::NameComponent>(object.entity, [&](auto& name) { name.name = String64(object.name); });
        if (auto transform = registry.Get<Components::TransformComponent>(object.entity)) {
            transform->position = object.renderPosition;
            transform->rotation = object.renderRotation.Normalized();
            transform->scale    = size;
        }
        if (auto world = registry.Get<Components::WorldTransformComponent>(object.entity)) {
            const JPH::Mat44 matrix = Math::CreateTransform(object.renderPosition, object.renderRotation.Normalized(), size);
            world->world    = matrix;
            world->previous = matrix;
        }
        if (auto mesh = registry.Get<Components::MeshComponent>(object.entity)) object.materialId = mesh->materialAsset;
        object.hasPhysicsBody  = registry.Get<Components::PhysicsComponent>(object.entity).has_value();
        object.spawnedSize     = size;
        object.spawnedShape    = object.shape;
        object.spawnedAnchored = object.anchored;
        object.spawnedCanCollide = object.canCollide;
        object.spawnedOwnerUid = object.networkOwnerUid;
        object.spawnedBlend    = blendMaterial;
        object.visualDirty     = true;
        object.transformDirty  = false;
    }

    if (object.entity == Entity::Null() || !registry.IsAlive(object.entity)) return;

    if (object.transformDirty) {
        if (auto transform = registry.Get<Components::TransformComponent>(object.entity)) {
            transform->position = object.renderPosition;
            transform->rotation = object.renderRotation.Normalized();
            transform->scale    = size;
        }
        object.transformDirty = false;
    }

    if (object.visualDirty && object.materialId != InvalidMaterialID) {
        if (auto material = engine.GetRenderContext().GetGPUMaterial(object.materialId)) {
            material->baseColorFactor = {object.color.GetX(), object.color.GetY(), object.color.GetZ(), 1.0f - object.transparency};
            material->emissiveFactor  = {object.color.GetX() * object.emission, object.color.GetY() * object.emission,
                                         object.color.GetZ() * object.emission, 1.0f};
            material->roughnessFactor = object.roughness;
            material->metallicFactor  = object.metallic;
            engine.GetRenderContext().RegisterGPUMaterial(object.materialId, *material);
        }
        registry.Patch<Components::PBRComponent>(object.entity, [&](auto& pbr) {
            pbr.roughness = object.roughness;
            pbr.metallic  = object.metallic;
        });
        registry.Patch<Components::NameComponent>(object.entity, [&](auto& name) { name.name = String64(object.name); });
        object.visualDirty = false;
    }
}

void ClientSession::SpawnOrUpdateLightEntity(Engine& engine, ReplicatedObject& object) {
    auto& registry = engine.GetRegistry();
    const bool directional = object.className == "DirectionalLight";
    if (object.entity == Entity::Null() || !registry.IsAlive(object.entity)
        || registry.Get<Components::PendingDestroy>(object.entity).has_value()) {
        object.entity = registry.Create();
        const JPH::Mat44 matrix = Math::CreateTransform(object.position, object.rotationQuat, JPH::Vec3::sReplicate(1.0f));
        registry.Add(
            object.entity,
            Components::NameComponent {.name = String64(object.name)},
            Components::TransformComponent {.position = object.position, .rotation = object.rotationQuat, .scale = JPH::Vec3::sReplicate(1.0f)},
            Components::WorldTransformComponent {.world = matrix, .previous = matrix},
            Components::LightComponent {
                .type = directional ? LightType::Sun : LightType::Point,
                .color = object.color,
                .intensity = object.enabled ? object.intensity * (directional ? 180.0f : 1.0f) : 0.0f,
                .radius = object.sourceRadius,
                .direction = object.direction,
                .range = object.range,
                .shadowLayer = -1
            },
            Components::SceneLightTagComponent {}
        );
        object.visualDirty = true;
    }

    if (directional && object.isMain && !registry.Get<Components::SunTagComponent>(object.entity)) {
        registry.Add(object.entity, Components::SunTagComponent {});
    } else if (directional && !object.isMain && registry.Get<Components::SunTagComponent>(object.entity)) {
        registry.Remove<Components::SunTagComponent>(object.entity);
    }

    if (auto name = registry.Get<Components::NameComponent>(object.entity)) name->name = String64(object.name);
    if (auto transform = registry.Get<Components::TransformComponent>(object.entity)) {
        transform->position = object.position;
        transform->rotation = object.rotationQuat;
    }
    if (auto light = registry.Get<Components::LightComponent>(object.entity)) {
        light->type      = directional ? LightType::Sun : LightType::Point;
        light->color     = object.color;
        light->intensity = object.enabled ? object.intensity * (directional ? 180.0f : 1.0f) : 0.0f;
        light->radius    = object.sourceRadius;
        light->direction = object.direction;
        light->range     = object.range;
    }
    object.transformDirty = false;
    object.visualDirty = false;
}

void ClientSession::SyncEnvironment(Engine& engine) {
    const ReplicatedObject* lighting = FindObject(m_lightingUid);
    if (lighting == nullptr) return;
    auto& registry = engine.GetRegistry();
    const JPH::Vec3 ambient = lighting->ambient;
    for (Entity settings: registry.GetEntitiesWith<Components::GlobalSettingsTagComponent>()) {
        registry.Patch<Components::PostProcessSettingsComponent>(settings, [&](auto& post) {
            post.skyZenith  = JPH::Vec4(ambient.GetX(), ambient.GetY(), ambient.GetZ(), 1.0f);
            post.skyHorizon = JPH::Vec4(ambient.GetX(), ambient.GetY(), ambient.GetZ(), 1.0f);
            post.skyGround  = JPH::Vec4(ambient.GetX() * 0.5f, ambient.GetY() * 0.5f, ambient.GetZ() * 0.5f, 1.0f);
        });
    }
}

void ClientSession::UpdateLocalController(Engine& engine) {
    auto& registry = engine.GetRegistry();
    auto bindCamera = [&](Entity target) -> void {
        if (target == Entity::Null()) return;
        for (Entity camera: registry.GetEntitiesWith<Components::MainCameraTagComponent>()) {
            registry.Remove<Components::FreeCamTagComponent>(camera);
            registry.Patch<CameraRig::TargetCameraComponent>(camera, [&](auto& rig) {
                if (rig.target != target) {
                    rig.target = target;
                    rig.hasInitSmoothTarget = 0;
                }
            });
        }
    };
    auto destroyController = [&]() -> void {
        if (m_localControllerBody != Physics::BodyHandle::Null()) {
            engine.GetPhysicsContext().DestroyBody(m_localControllerBody);
            m_localControllerBody = Physics::BodyHandle::Null();
        }
        if (m_localController != Entity::Null() && registry.IsAlive(m_localController)) {
            DespawnEntity(engine, m_localController);
        }
        m_localController = Entity::Null();
        m_controllerForRootUid = 0;
    };

    if (m_serverAuthority) {
        if (m_localController != Entity::Null() || m_localControllerBody != Physics::BodyHandle::Null()) destroyController();
        const ReplicatedObject* rootPart = FindObject(m_localRootPartUid);
        if (rootPart != nullptr && rootPart->entity != Entity::Null() && registry.IsAlive(rootPart->entity)) bindCamera(rootPart->entity);
        return;
    }
    if (m_localRootPartUid == 0) {
        if (m_localController != Entity::Null() || m_localControllerBody != Physics::BodyHandle::Null()) destroyController();
        return;
    }
    if (m_controllerForRootUid == m_localRootPartUid && m_localController != Entity::Null()
        && registry.IsAlive(m_localController) && m_localControllerBody != Physics::BodyHandle::Null()) {
        const ReplicatedObject* humanoid = FindObject(m_localHumanoidUid);
        if (auto move = registry.Get<Character::MovementComponent>(m_localController); move && humanoid != nullptr) {
            move->speed = std::max(0.0f, humanoid->walkSpeed);
            move->jumpForce = std::max(0.0f, humanoid->jumpPower) * kLocalJumpScale;
        }
        bindCamera(m_localController);
        return;
    }
    destroyController();

    const ReplicatedObject* rootPart = FindObject(m_localRootPartUid);
    if (rootPart == nullptr) return;

    auto& physics = engine.GetPhysicsContext();
    Physics::CharacterParams params;
    params.shape = physics.GetOrCreateShape(Physics::ShapeType::Capsule, 2.0f, 0.5f);
    params.maxSlopeAngle = JPH::DegreesToRadians(45.0f);
    m_localControllerBody = physics.CreateCharacter(JPH::RVec3(rootPart->position), params);
    if (m_localControllerBody == Physics::BodyHandle::Null()) {
        Log("[ProjectLight] Could not create the local player CharacterVirtual body.");
        return;
    }

    registry.RegisterComponent<Character::MovementComponent>();
    registry.RegisterComponent<Character::InputComponent>();
    const Entity controller = registry.Create();
    const JPH::Mat44 matrix = Math::CreateTransform(rootPart->position, rootPart->rotationQuat, JPH::Vec3::sReplicate(1.0f));
    registry.Add(
        controller,
        Components::NameComponent {.name = String64("ProjectLightLocalController")},
        Components::TransformComponent {.position = rootPart->position, .rotation = rootPart->rotationQuat, .scale = JPH::Vec3::sReplicate(1.0f)},
        Components::WorldTransformComponent {.world = matrix, .previous = matrix},
        Components::PhysicsComponent {.physicsHandle = m_localControllerBody, .isStatic = false},
        Character::MovementComponent {.orientation = rootPart->rotationQuat, .prevOrientation = rootPart->rotationQuat},
        Character::InputComponent {}
    );
    m_localController = controller;
    m_controllerForRootUid = m_localRootPartUid;

    const ReplicatedObject* humanoid = FindObject(m_localHumanoidUid);
    if (auto move = registry.Get<Character::MovementComponent>(controller); move && humanoid != nullptr) {
        move->speed = std::max(0.0f, humanoid->walkSpeed);
        move->jumpForce = std::max(0.0f, humanoid->jumpPower) * kLocalJumpScale;
    }

    bindCamera(controller);
}

void ClientSession::UpdateRemoteCharacterParts(Engine& engine, float dt) {
    (void)dt;
    const auto renderTime = std::chrono::steady_clock::now() - kNetworkSmoothDelay;
    for (const auto& [userId, history]: m_playerControllerStates) {
        if (userId == m_config.userId || history.Empty()) continue;
        const ReplicatedObject* player = nullptr;
        for (const auto& [uid, candidate]: m_objects) {
            (void)uid;
            if (candidate.className == "Player" && candidate.userId == userId) { player = &candidate; break; }
        }
        if (player == nullptr || player->characterUid == 0) continue;

        JPH::Vec3 rootPosition {};
        JPH::Quat rootRotation {};
        if (!history.Sample(renderTime, rootPosition, rootRotation)) continue;
        const ReplicatedObject* humanoid = nullptr;
        for (const auto& [uid, candidate]: m_objects) {
            (void)uid;
            if (candidate.parentUid == player->characterUid && candidate.className == "Humanoid") { humanoid = &candidate; break; }
        }
        const HumanoidState stateValue = humanoid ? humanoid->humanoidState : HumanoidState::Idle;
        const float walkSpeed = humanoid ? humanoid->walkSpeed : 16.0f;

        for (auto& [uid, object]: m_objects) {
            if (!IsPartClass(object.className) || !IsPartOfCharacter(uid, player->characterUid)) continue;
            const JPH::Vec3 offset = CharacterOffsetForName(object.name);
            const JPH::Vec3 position = rootPosition + rootRotation * offset;
            const float poseAngle = IsCharacterLimb(object.name) ? LimbPoseAngle(object.name, stateValue, m_time, walkSpeed) : 0.0f;
            const JPH::Quat limbRotation = poseAngle == 0.0f ? rootRotation : rootRotation * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), poseAngle);
            WriteInterpolatedTransform(engine, object, position, limbRotation);
        }
    }
}

void ClientSession::UpdateLocalCharacterParts(Engine& engine, float dt) {
    if (m_localController == Entity::Null() || m_localControllerBody == Physics::BodyHandle::Null()) return;
    auto& registry = engine.GetRegistry();
    auto movement = registry.Get<Character::MovementComponent>(m_localController);
    if (!movement) return;

    Physics::BodyStateSnapshot state {};
    auto& physics = engine.GetPhysicsContext();
    if (!physics.TryGetBodyState(m_localControllerBody, state) || !state.valid) return;
    const JPH::Vec3 rootPosition = state.currentPosition;
    const JPH::Quat rootRotation = movement->orientation.Normalized();
    const JPH::Vec3 velocity = physics.GetCharacterVelocity(m_localControllerBody);
    (void)dt;

    if (auto input = registry.GetSingleton<Components::InputStateComponent>()) {
        const bool jumpDown = input->IsKeyDown(static_cast<uint8_t>(KeyCode::Space));
        if (jumpDown && !m_wasJumpDown && movement->isGrounded) m_jumpRemaining = 0.20f;
        m_wasJumpDown = jumpDown;
    }
    m_jumpRemaining = std::max(0.0f, m_jumpRemaining - std::max(0.0f, dt));
    HumanoidState stateValue = HumanoidState::Idle;
    if (!movement->isGrounded) stateValue = m_jumpRemaining > 0.0f ? HumanoidState::Jumping : HumanoidState::Freefall;
    else if (std::abs(movement->inputX) + std::abs(movement->inputZ) > 0.05f) stateValue = HumanoidState::Running;

    const ReplicatedObject* humanoid = FindObject(m_localHumanoidUid);
    const float walkSpeed = humanoid ? humanoid->walkSpeed : movement->speed;
    for (auto& [uid, object]: m_objects) {
        if (!IsPartClass(object.className) || !IsPartOfCharacter(uid, m_localCharacterUid)) continue;
        const JPH::Vec3 position = rootPosition + rootRotation * CharacterOffsetForName(object.name);
        const float poseAngle = IsCharacterLimb(object.name) ? LimbPoseAngle(object.name, stateValue, m_time, walkSpeed) : 0.0f;
        const JPH::Quat limbRotation = poseAngle == 0.0f ? rootRotation : rootRotation * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), poseAngle);
        object.linearVelocity = velocity;
        object.humanoidState = stateValue;
        WriteTransform(engine, object, position, limbRotation);
    }
}

void ClientSession::UpdateSoundVoices(Engine& engine) {
    for (auto& [uid, object]: m_objects) {
        (void)uid;
        if (object.className != "Sound") continue;
        auto& audio = engine.GetAudioContext();
        if (object.playing) {
            if (object.audioHandle == AudioHandle::Invalid || !audio.IsVoiceValid(object.audioHandle)) {
                if (object.soundId.empty()) continue;
                object.audioHandle = audio.CreateVoice(object.soundId, true, object.loops, object.volume);
                if (object.audioHandle == AudioHandle::Invalid) {
                    continue; // asset may not be present in the local Zahlen installation
                }
                audio.PlayVoice(object.audioHandle);
            } else {
                audio.SetVoiceVolume(object.audioHandle, object.volume);
                audio.SetVoiceLooping(object.audioHandle, object.loops);
            }
            const ReplicatedObject* parent = FindObject(object.parentUid);
            if (parent != nullptr) audio.SetVoicePosition(object.audioHandle, parent->position);
        } else if (object.audioHandle != AudioHandle::Invalid) {
            audio.StopVoice(object.audioHandle);
            object.audioHandle = AudioHandle::Invalid;
        }
    }
}

void ClientSession::SendRealtimeFrame(Engine& engine) {
    if (!m_connected || !m_snapshotComplete) return;
    auto& registry = engine.GetRegistry();
    auto input = registry.GetSingleton<Components::InputStateComponent>();
    m_lastControls = {};
    m_lastControls.yaw = engine.GetCamera().yaw;
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
        Physics::BodyStateSnapshot state {};
        auto& physics = engine.GetPhysicsContext();
        const auto movement = registry.Get<Character::MovementComponent>(m_localController);
        if (physics.TryGetBodyState(m_localControllerBody, state) && state.valid) {
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
        } else if (object.entity != Entity::Null() && registry.IsAlive(object.entity)) {
            const auto body = registry.Get<Components::PhysicsComponent>(object.entity);
            if (body) {
                Physics::BodyStateSnapshot snapshot {};
                if (physics.TryGetBodyState(body->physicsHandle, snapshot) && snapshot.valid) {
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
        object.linearVelocity = transform.linearVelocity;
        object.renderPosition = object.position;
        object.renderRotation = object.rotationQuat;
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
    UpdateLocalCharacterParts(engine, dt);
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
        object.entity = Entity::Null();
        object.materialId = InvalidMaterialID;
        object.hasPhysicsBody = false;
        object.spawnedSize = JPH::Vec3::sZero();
        object.visualDirty = true;
        object.transformDirty = true;
    }
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

void InstallClient(Engine& engine, ClientSession& session) {
    InstalledSessions().sessions[&engine] = &session;
    engine.AddFrameSchedulerExtension(&AddLightClientFrameSteps);
    engine.AddSceneCleanupPass(&LightClientSceneCleanup);
    engine.AddTeardownHook(&LightClientEngineTeardown);
    Log("[ProjectLight][lifecycle] client frame polling, scene-cleanup, and teardown hooks installed.");
}

} // namespace ZHLN::ProjectLight
