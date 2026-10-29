// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Discord/RichPresence.hpp>
#include <Socket/LocalEndpoint.hpp>
#include <Socket/LocalStream.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <json/JSONSchema.hpp>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace ZHLN::Discord {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t MaxRpcPayloadSize = 1024U * 1024U;
constexpr auto     IoTimeout         = std::chrono::milliseconds(100);
constexpr auto     InitialRetryDelay = std::chrono::seconds(1);
constexpr auto     MaximumRetryDelay = std::chrono::seconds(30);

struct HandshakePayload {
    uint32_t    v = 1;
    std::string client_id;
};

struct RpcTimestamps {
    int64_t start = 0;
};

struct RpcAssets {
    std::optional<std::string> large_image;
    std::optional<std::string> large_text;
};

struct RpcActivity {
    uint32_t                     type = 0;
    std::optional<std::string>   details;
    std::optional<std::string>   state;
    std::optional<RpcTimestamps> timestamps;
    std::optional<RpcAssets>     assets;
};

struct ActivityArguments {
    uint64_t                   pid = 0;
    std::optional<RpcActivity> activity;
};

struct SetActivityPayload {
    std::string       cmd;
    ActivityArguments args;
    std::string       nonce;
};

struct ClearActivityArguments {
    uint64_t                   pid = 0;
    std::optional<std::string> activity;
};

struct ClearActivityPayload {
    std::string            cmd;
    ClearActivityArguments args;
    std::string            nonce;
};

struct RpcErrorData {
    std::optional<int32_t>     code;
    std::optional<std::string> message;
};

struct RpcEvent {
    std::optional<std::string>  cmd;
    std::optional<std::string>  evt;
    std::optional<RpcErrorData> data;
};

enum class RpcClientError : uint8_t { IoFailure = 1, TimedOut, PeerClosed, InvalidFrame, InvalidJson, RpcRejected, Stopped };

enum class ConnectFailure : uint8_t { ClientUnavailable = 1, RpcRejected, Stopped };

enum class WorkerSignal : uint8_t { Continue, Stop };

enum class FrameAction : uint8_t { Continue, Ready, Rejected, Closed };

enum class DrainAction : uint8_t { Continue, Ready, Rejected, Closed };

struct FrameExtraction {
    std::optional<RpcFrame> frame;
    size_t                  consumedBytes = 0;
};

struct FrameDrain {
    DrainAction action        = DrainAction::Continue;
    size_t      consumedBytes = 0;
};

struct ActivityUpdate {
    std::optional<RichPresence> activity;
    uint64_t                    revision = 0;
};

[[nodiscard]] auto ReadU32LittleEndian(const std::span<const uint8_t, 4> bytes) noexcept -> uint32_t {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U) | (static_cast<uint32_t>(bytes[2]) << 16U) |
           (static_cast<uint32_t>(bytes[3]) << 24U);
}

void WriteU32LittleEndian(const std::span<uint8_t, 4> bytes, const uint32_t value) noexcept {
    bytes[0] = static_cast<uint8_t>(value & 0xFFU);
    bytes[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    bytes[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
    bytes[3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
}

[[nodiscard]] auto NonEmptyText(const std::string_view value) -> std::optional<std::string> {
    if (value.empty()) {
        return std::nullopt;
    }
    return std::string(value);
}

[[nodiscard]] auto BuildHandshakePayload(const std::string_view applicationId) -> std::string {
    return ReflectJSON::SerializeJSON(HandshakePayload {.v = 1, .client_id = std::string(applicationId)});
}

[[nodiscard]] auto BuildActivityPayload(const ActivityUpdate& update, const uint64_t nonce) -> std::string {
    const std::string nonceText = std::to_string(nonce);
    if (!update.activity) {
        return ReflectJSON::SerializeJSON(
            ClearActivityPayload {.cmd = "SET_ACTIVITY", .args = {.pid = static_cast<uint64_t>(ZHLN::GetPID()), .activity = std::nullopt}, .nonce = nonceText}
        );
    }

    const RichPresence& source = *update.activity;
    RpcActivity         activity {
        .type = 0, .details = NonEmptyText(source.details), .state = NonEmptyText(source.state), .timestamps = std::nullopt, .assets = std::nullopt
    };
    if (source.startTimestamp) {
        activity.timestamps = RpcTimestamps {.start = *source.startTimestamp};
    }

    RpcAssets assets {
        .large_image = source.largeImage ? NonEmptyText(*source.largeImage) : std::nullopt,
        .large_text  = source.largeText ? NonEmptyText(*source.largeText) : std::nullopt
    };
    if (assets.large_image || assets.large_text) {
        activity.assets = std::move(assets);
    }

    return ReflectJSON::SerializeJSON(
        SetActivityPayload {.cmd = "SET_ACTIVITY", .args = {.pid = static_cast<uint64_t>(ZHLN::GetPID()), .activity = std::move(activity)}, .nonce = nonceText},
        0, {.omitEmpty = true}
    );
}

[[nodiscard]] auto ParseRpcEvent(const std::string_view payload) noexcept -> std::expected<RpcEvent, RpcClientError> {
    auto parsed = ReflectJSON::Parse<RpcEvent>(payload, {.omitEmpty = true});
    if (!parsed) {
        return std::unexpected(RpcClientError::InvalidJson);
    }
    return std::move(*parsed);
}

[[nodiscard]] auto WriteAll(Net::LocalStream& stream, const std::span<const uint8_t> bytes, const std::chrono::milliseconds timeout) noexcept
    -> std::expected<void, RpcClientError> {
    const auto deadline = Clock::now() + std::max(timeout, std::chrono::milliseconds::zero());
    size_t     offset   = 0;
    while (offset < bytes.size()) {
        const auto now = Clock::now();
        if (now >= deadline) {
            return std::unexpected(RpcClientError::TimedOut);
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto       result    = stream.Send(bytes.subspan(offset), remaining);
        if (!result) {
            return std::unexpected(RpcClientError::IoFailure);
        }
        if (result->state == Net::SocketIOState::PeerClosed) {
            return std::unexpected(RpcClientError::PeerClosed);
        }
        if (result->state == Net::SocketIOState::WouldBlock) {
            continue;
        }
        if (result->bytes == 0) {
            return std::unexpected(RpcClientError::IoFailure);
        }
        offset += result->bytes;
    }
    return {};
}

[[nodiscard]] auto SendFrame(
    Net::LocalStream&               stream,
    const RpcOpcode                 opcode,
    const std::string_view          payload,
    const std::chrono::milliseconds timeout = std::chrono::seconds(2)
) noexcept -> std::expected<void, RpcClientError> {
    auto frame = EncodeRpcFrame(opcode, payload);
    if (!frame) {
        return std::unexpected(RpcClientError::InvalidFrame);
    }
    return WriteAll(stream, *frame, timeout);
}

[[nodiscard]] auto PopFrame(const std::span<const uint8_t> buffer) noexcept -> std::expected<FrameExtraction, RpcProtocolError> {
    if (buffer.size() < 8) {
        return FrameExtraction {};
    }

    const auto     opcodeBytes = std::span<const uint8_t, 4> {buffer.data(), 4};
    const auto     lengthBytes = std::span<const uint8_t, 4> {buffer.data() + 4, 4};
    const uint32_t opcodeValue = ReadU32LittleEndian(opcodeBytes);
    const uint32_t payloadSize = ReadU32LittleEndian(lengthBytes);
    if (payloadSize > MaxRpcPayloadSize) {
        return std::unexpected(RpcProtocolError::PayloadTooLarge);
    }
    if (buffer.size() < static_cast<size_t>(payloadSize) + 8) {
        return FrameExtraction {};
    }

    auto decoded = DecodeRpcFrame(buffer.first(static_cast<size_t>(payloadSize) + 8));
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (opcodeValue != static_cast<uint32_t>(decoded->opcode)) {
        return std::unexpected(RpcProtocolError::InvalidFrame);
    }
    return FrameExtraction {.frame = std::move(*decoded), .consumedBytes = static_cast<size_t>(payloadSize) + 8};
}

[[nodiscard]] auto ProcessFrame(Net::LocalStream& stream, const RpcFrame& frame) noexcept -> std::expected<FrameAction, RpcClientError> {
    switch (frame.opcode) {
        case RpcOpcode::Ping: {
            auto pong = SendFrame(stream, RpcOpcode::Pong, frame.payload);
            if (!pong) {
                return std::unexpected(pong.error());
            }
            return FrameAction::Continue;
        }
        case RpcOpcode::Close:
            return FrameAction::Closed;
        case RpcOpcode::Frame: {
            auto event = ParseRpcEvent(frame.payload);
            if (!event) {
                return std::unexpected(event.error());
            }
            if (event->evt && *event->evt == "ERROR") {
                return FrameAction::Rejected;
            }
            if (event->evt && *event->evt == "READY") {
                return FrameAction::Ready;
            }
            return FrameAction::Continue;
        }
        case RpcOpcode::Pong:
            return FrameAction::Continue;
        case RpcOpcode::Handshake:
            return std::unexpected(RpcClientError::InvalidFrame);
    }
    return std::unexpected(RpcClientError::InvalidFrame);
}

[[nodiscard]] auto DrainFrames(Net::LocalStream& stream, const std::span<const uint8_t> buffer) noexcept -> std::expected<FrameDrain, RpcClientError> {
    FrameDrain drained;
    while (drained.consumedBytes < buffer.size()) {
        auto extraction = PopFrame(buffer.subspan(drained.consumedBytes));
        if (!extraction) {
            return std::unexpected(RpcClientError::InvalidFrame);
        }
        if (!extraction->frame) {
            return drained;
        }

        auto action = ProcessFrame(stream, *extraction->frame);
        if (!action) {
            return std::unexpected(action.error());
        }
        drained.consumedBytes += extraction->consumedBytes;
        switch (*action) {
            case FrameAction::Continue:
                break;
            case FrameAction::Ready:
                drained.action = DrainAction::Ready;
                break;
            case FrameAction::Rejected:
                drained.action = DrainAction::Rejected;
                return drained;
            case FrameAction::Closed:
                drained.action = DrainAction::Closed;
                return drained;
        }
    }
    return drained;
}

[[nodiscard]] auto WaitForReady(Net::LocalStream& stream, const std::atomic<WorkerSignal>& workerSignal) noexcept -> std::expected<void, RpcClientError> {
    const auto                deadline = Clock::now() + std::chrono::seconds(5);
    std::array<uint8_t, 4096> scratch {};
    std::vector<uint8_t>      receiveBuffer;

    while (workerSignal.load(std::memory_order::acquire) == WorkerSignal::Continue && Clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        auto       received  = stream.Receive(scratch, std::min(IoTimeout, remaining));
        if (!received) {
            return std::unexpected(RpcClientError::IoFailure);
        }
        if (received->state == Net::SocketIOState::PeerClosed) {
            return std::unexpected(RpcClientError::PeerClosed);
        }
        if (received->state == Net::SocketIOState::WouldBlock) {
            continue;
        }

        receiveBuffer.insert(receiveBuffer.end(), scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(received->bytes));
        auto drained = DrainFrames(stream, receiveBuffer);
        if (!drained) {
            return std::unexpected(drained.error());
        }
        receiveBuffer.erase(receiveBuffer.begin(), receiveBuffer.begin() + static_cast<std::ptrdiff_t>(drained->consumedBytes));
        if (drained->action == DrainAction::Ready) {
            return {};
        }
        if (drained->action == DrainAction::Rejected) {
            return std::unexpected(RpcClientError::RpcRejected);
        }
        if (drained->action == DrainAction::Closed) {
            return std::unexpected(RpcClientError::PeerClosed);
        }
    }
    if (workerSignal.load(std::memory_order::acquire) == WorkerSignal::Stop) {
        return std::unexpected(RpcClientError::Stopped);
    }
    return std::unexpected(RpcClientError::TimedOut);
}

[[nodiscard]] auto ConnectToDiscord(const std::string_view applicationId, const std::atomic<WorkerSignal>& workerSignal) noexcept
    -> std::expected<Net::LocalStream, ConnectFailure> {
    // Clean, declarative endpoint discovery:
    const auto candidateEndpoints =
        Net::EnumerateLocalEndpoints({.serviceName = "discord-ipc", .scope = Net::IpcScope::UserSession, .maxInstances = 10, .indexed = true});

    ConnectFailure latestFailure = ConnectFailure::ClientUnavailable;

    for (const std::string& endpoint: candidateEndpoints) {
        if (workerSignal.load(std::memory_order::acquire) == WorkerSignal::Stop) {
            return std::unexpected(ConnectFailure::Stopped);
        }

        auto stream = Net::ConnectLocalStream(endpoint, std::chrono::milliseconds(250));
        if (!stream) {
            continue;
        }

        const auto handshakePayload = BuildHandshakePayload(applicationId);
        auto       sent             = SendFrame(*stream, RpcOpcode::Handshake, handshakePayload);
        if (!sent) {
            continue;
        }
        auto ready = WaitForReady(*stream, workerSignal);
        if (ready) {
            return std::move(*stream);
        }
        if (ready.error() == RpcClientError::RpcRejected) {
            latestFailure = ConnectFailure::RpcRejected;
        } else if (ready.error() == RpcClientError::Stopped) {
            return std::unexpected(ConnectFailure::Stopped);
        }
    }
    if (workerSignal.load(std::memory_order::acquire) == WorkerSignal::Stop) {
        return std::unexpected(ConnectFailure::Stopped);
    }
    return std::unexpected(latestFailure);
}

[[nodiscard]] auto BuildUpdate(std::optional<RichPresence> activity, const uint64_t revision) -> ActivityUpdate {
    return ActivityUpdate {.activity = std::move(activity), .revision = revision};
}

} // namespace

auto ValidateApplicationId(const std::string_view applicationId) noexcept -> std::expected<void, PresenceError> {
    if (applicationId.empty()) {
        return std::unexpected(PresenceError::InvalidApplicationId);
    }
    for (const char digit: applicationId) {
        if (digit < '0' || digit > '9') {
            return std::unexpected(PresenceError::InvalidApplicationId);
        }
    }
    return {};
}

auto EncodeRpcFrame(const RpcOpcode opcode, const std::string_view payload) noexcept -> std::expected<std::vector<uint8_t>, RpcProtocolError> {
    if (payload.size() > MaxRpcPayloadSize || payload.size() > std::numeric_limits<uint32_t>::max()) {
        return std::unexpected(RpcProtocolError::PayloadTooLarge);
    }

    switch (opcode) {
        case RpcOpcode::Handshake:
        case RpcOpcode::Frame:
        case RpcOpcode::Close:
        case RpcOpcode::Ping:
        case RpcOpcode::Pong:
            break;
        default:
            return std::unexpected(RpcProtocolError::UnsupportedOpcode);
    }

    std::vector<uint8_t> frame(8 + payload.size());
    WriteU32LittleEndian(std::span<uint8_t, 4> {frame.data(), 4}, static_cast<uint32_t>(opcode));
    WriteU32LittleEndian(std::span<uint8_t, 4> {frame.data() + 4, 4}, static_cast<uint32_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), frame.begin() + 8);
    return frame;
}

auto DecodeRpcFrame(const std::span<const uint8_t> bytes) noexcept -> std::expected<RpcFrame, RpcProtocolError> {
    if (bytes.size() < 8) {
        return std::unexpected(RpcProtocolError::InvalidFrame);
    }

    const uint32_t opcodeValue = ReadU32LittleEndian(std::span<const uint8_t, 4> {bytes.data(), 4});
    const uint32_t payloadSize = ReadU32LittleEndian(std::span<const uint8_t, 4> {bytes.data() + 4, 4});
    if (payloadSize > MaxRpcPayloadSize) {
        return std::unexpected(RpcProtocolError::PayloadTooLarge);
    }
    if (bytes.size() != static_cast<size_t>(payloadSize) + 8) {
        return std::unexpected(RpcProtocolError::InvalidFrame);
    }

    const auto opcode = static_cast<RpcOpcode>(opcodeValue);
    switch (opcode) {
        case RpcOpcode::Handshake:
        case RpcOpcode::Frame:
        case RpcOpcode::Close:
        case RpcOpcode::Ping:
        case RpcOpcode::Pong:
            break;
        default:
            return std::unexpected(RpcProtocolError::UnsupportedOpcode);
    }

    return RpcFrame {.opcode = opcode, .payload = std::string(reinterpret_cast<const char*>(bytes.data() + 8), payloadSize)};
}

class DiscordPresenceClient::Impl {
  public:
    explicit Impl(std::string applicationId): applicationId(std::move(applicationId)) {
    }

    ~Impl() noexcept {
        Stop();
        TaskSystem::Wait(&workerCompletion);
    }

    [[nodiscard]] auto Start() noexcept -> std::expected<void, PresenceError> {
        auto validApplicationId = ValidateApplicationId(applicationId);
        if (!validApplicationId) {
            return std::unexpected(validApplicationId.error());
        }

        std::lock_guard lifecycleLock(lifecycleMutex);
        if (lifecycle == Lifecycle::Running && workerCompletion.value.load(std::memory_order::acquire) != 0) {
            return std::unexpected(PresenceError::AlreadyStarted);
        }

        workerSignal.store(WorkerSignal::Continue, std::memory_order::release);
        state.store(PresenceState::Connecting, std::memory_order::release);
        lifecycle = Lifecycle::Running;
        const TaskSystem::Task task {.func = &RunTask, .arg = this};
        auto                   scheduled = TaskSystem::DispatchBackground(task, &workerCompletion);
        if (!scheduled) {
            lifecycle = Lifecycle::Stopped;
            workerSignal.store(WorkerSignal::Stop, std::memory_order::release);
            state.store(PresenceState::Stopped, std::memory_order::release);
            if (scheduled.error() == TaskSystem::BackgroundDispatchError::NoAvailableFiber ||
                scheduled.error() == TaskSystem::BackgroundDispatchError::QueueFull) {
                return std::unexpected(PresenceError::TaskCapacityExhausted);
            }
            return std::unexpected(PresenceError::TaskSystemUnavailable);
        }
        return {};
    }

    void SetActivity(RichPresence activity) noexcept {
        std::lock_guard lock(mutex);
        desiredUpdate = BuildUpdate(std::move(activity), ++revision);
    }

    void ClearActivity() noexcept {
        std::lock_guard lock(mutex);
        desiredUpdate = BuildUpdate(std::nullopt, ++revision);
    }

    void Stop() noexcept {
        {
            std::lock_guard lifecycleLock(lifecycleMutex);
            if (lifecycle == Lifecycle::Stopped || workerCompletion.value.load(std::memory_order::acquire) == 0) {
                lifecycle = Lifecycle::Stopped;
                workerSignal.store(WorkerSignal::Stop, std::memory_order::release);
                state.store(PresenceState::Stopped, std::memory_order::release);
                return;
            }
            if (state.load(std::memory_order::acquire) == PresenceState::Stopped) {
                return;
            }
            workerSignal.store(WorkerSignal::Stop, std::memory_order::release);
            state.store(PresenceState::Stopping, std::memory_order::release);
        }
        retryCondition.notify_all();
    }

    [[nodiscard]] auto State() const noexcept -> PresenceState {
        return state.load(std::memory_order::acquire);
    }

  private:
    enum class Lifecycle : uint8_t { Stopped, Running };

    static void RunTask(void* context) noexcept {
        auto& client = *static_cast<Impl*>(context);
        client.Run();
    }

    void WaitToRetry(const std::chrono::seconds delay) noexcept {
        std::unique_lock waitLock(retryMutex);
        retryCondition.wait_for(waitLock, delay, [this] { return workerSignal.load(std::memory_order::acquire) == WorkerSignal::Stop; });
    }

    [[nodiscard]] auto CurrentUpdate() -> std::optional<ActivityUpdate> {
        std::lock_guard lock(mutex);
        return desiredUpdate;
    }

    [[nodiscard]] auto RunSession(Net::LocalStream& stream) noexcept -> std::expected<void, RpcClientError> {
        uint64_t                  lastSentRevision = std::numeric_limits<uint64_t>::max();
        uint64_t                  nonce            = 1;
        std::vector<uint8_t>      receiveBuffer;
        std::array<uint8_t, 4096> scratch {};

        while (workerSignal.load(std::memory_order::acquire) == WorkerSignal::Continue) {
            auto pendingUpdate = CurrentUpdate();
            if (pendingUpdate && pendingUpdate->revision != lastSentRevision) {
                const std::string payload = BuildActivityPayload(*pendingUpdate, nonce++);
                auto              sent    = SendFrame(stream, RpcOpcode::Frame, payload);
                if (!sent) {
                    return std::unexpected(sent.error());
                }
                lastSentRevision = pendingUpdate->revision;
            }

            auto received = stream.Receive(scratch, IoTimeout);
            if (!received) {
                return std::unexpected(RpcClientError::IoFailure);
            }
            if (received->state == Net::SocketIOState::PeerClosed) {
                return std::unexpected(RpcClientError::PeerClosed);
            }
            if (received->state == Net::SocketIOState::WouldBlock) {
                continue;
            }

            receiveBuffer.insert(receiveBuffer.end(), scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(received->bytes));
            auto drained = DrainFrames(stream, receiveBuffer);
            if (!drained) {
                return std::unexpected(drained.error());
            }
            receiveBuffer.erase(receiveBuffer.begin(), receiveBuffer.begin() + static_cast<std::ptrdiff_t>(drained->consumedBytes));
            if (drained->action == DrainAction::Rejected) {
                return std::unexpected(RpcClientError::RpcRejected);
            }
            if (drained->action == DrainAction::Closed) {
                return std::unexpected(RpcClientError::PeerClosed);
            }
        }

        auto clear = SendFrame(stream, RpcOpcode::Frame, BuildActivityPayload(BuildUpdate(std::nullopt, 0), nonce), std::chrono::milliseconds(250));
        if (!clear) {
            stream.Close();
        }
        return std::unexpected(RpcClientError::Stopped);
    }

    void Run() noexcept {
        auto retryDelay = InitialRetryDelay;
        while (workerSignal.load(std::memory_order::acquire) == WorkerSignal::Continue) {
            state.store(PresenceState::Connecting, std::memory_order::release);
            auto connection = ConnectToDiscord(applicationId, workerSignal);
            if (!connection) {
                if (connection.error() == ConnectFailure::Stopped) {
                    break;
                }
                const auto failure = connection.error();
                state.store(failure == ConnectFailure::RpcRejected ? PresenceState::RpcRejected : PresenceState::Disconnected, std::memory_order::release);
                WaitToRetry(failure == ConnectFailure::RpcRejected ? MaximumRetryDelay : retryDelay);
                retryDelay = std::min(retryDelay * 2, MaximumRetryDelay);
                continue;
            }

            state.store(PresenceState::Connected, std::memory_order::release);
            retryDelay   = InitialRetryDelay;
            auto session = RunSession(*connection);
            if (!session && session.error() == RpcClientError::Stopped && workerSignal.load(std::memory_order::acquire) == WorkerSignal::Stop) {
                break;
            }
            if (!session && session.error() == RpcClientError::RpcRejected) {
                state.store(PresenceState::RpcRejected, std::memory_order::release);
                WaitToRetry(MaximumRetryDelay);
            } else {
                state.store(PresenceState::Disconnected, std::memory_order::release);
                WaitToRetry(retryDelay);
                retryDelay = std::min(retryDelay * 2, MaximumRetryDelay);
            }
        }
        state.store(PresenceState::Stopped, std::memory_order::release);
    }

    std::string                   applicationId;
    mutable std::mutex            lifecycleMutex;
    std::mutex                    mutex;
    std::mutex                    retryMutex;
    std::condition_variable       retryCondition;
    std::optional<ActivityUpdate> desiredUpdate;
    uint64_t                      revision = 0;
    std::atomic<WorkerSignal>     workerSignal {WorkerSignal::Continue};
    std::atomic<PresenceState>    state {PresenceState::Stopped};
    Lifecycle                     lifecycle = Lifecycle::Stopped;
    TaskSystem::Counter           workerCompletion {};
};

DiscordPresenceClient::DiscordPresenceClient(std::string applicationId): _impl(std::make_unique<Impl>(std::move(applicationId))) {
}

DiscordPresenceClient::~DiscordPresenceClient() noexcept = default;

auto DiscordPresenceClient::Start() noexcept -> std::expected<void, PresenceError> {
    return _impl->Start();
}

void DiscordPresenceClient::SetActivity(RichPresence activity) noexcept {
    _impl->SetActivity(std::move(activity));
}

void DiscordPresenceClient::ClearActivity() noexcept {
    _impl->ClearActivity();
}

void DiscordPresenceClient::Stop() noexcept {
    _impl->Stop();
}

auto DiscordPresenceClient::State() const noexcept -> PresenceState {
    return _impl->State();
}

} // namespace ZHLN::Discord
