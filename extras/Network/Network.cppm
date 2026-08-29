// extras/Network/Network.cppm
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// --- Global Module Fragment: External non-modular includes only ---
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Common.h>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

export module ZHLN.Network;

export namespace ZHLN::Net {

// ============================================================================
// Protocol & Quantization Constants
// ============================================================================

inline constexpr uint16_t DEFAULT_GAME_PORT    = 5555;
inline constexpr size_t   MAX_SAFE_UDP_PAYLOAD = 1200;
inline constexpr float    VEC3_SCALE           = 32767.0f;
inline constexpr float    COLOR3_SCALE         = 255.0f;
inline constexpr float    INV_VEC3_SCALE       = 1.0f / 32767.0f;
inline constexpr float    INV_COLOR3_SCALE     = 1.0f / 255.0f;

enum class PropertyChannel : uint8_t {
    Name         = 0,
    ClassName    = 1,
    Parent       = 2,
    Position     = 3,
    Rotation     = 4,
    Size         = 5,
    Color        = 6,
    Anchored     = 7,
    CanCollide   = 8,
    Transparency = 9,
    Shape        = 10
};

enum class NetworkError : uint8_t {
    SocketError[[= ZHLN::Reflect::Description<"Socket operation failed"> {}]] = 1,
    ConnectionFailed[[= ZHLN::Reflect::Description<"Failed to establish server connection"> {}]],
    HandshakeFailed[[= ZHLN::Reflect::Description<"Server handshake or token verification failed"> {}]],
    DecompressionFailed[[= ZHLN::Reflect::Description<"zlib decompression failed or corrupted payload"> {}]],
    UnpackFailed[[= ZHLN::Reflect::Description<"MessagePack deserialization failed"> {}]],
    AllocationFailed[[= ZHLN::Reflect::Description<"Network buffer allocation failed"> {}]],
    InvalidPayload[[= ZHLN::Reflect::Description<"Payload structure does not match expected protocol schema"> {}]]
};

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

class ClientReplicator;

class ZHLN_API NetworkClient {
  public:
    NetworkClient();
    ~NetworkClient();

    NetworkClient(const NetworkClient&)                    = delete;
    auto operator=(const NetworkClient&) -> NetworkClient& = delete;
    NetworkClient(NetworkClient&&) noexcept;
    auto operator=(NetworkClient&&) noexcept -> NetworkClient&;

    [[nodiscard]] auto Connect(std::string_view host, uint16_t port, uint64_t userId, std::string_view token) noexcept -> std::expected<void, Error>;
    void               Disconnect() noexcept;

    [[nodiscard]] auto PollEvents(Engine& engine) noexcept -> std::expected<void, Error>;
    void               SendInputs(bool forward, bool backward, bool left, bool right, bool jump, float yaw) noexcept;

    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] bool IsRealtimeReady() const noexcept;

    [[nodiscard]] auto GetReplicator() noexcept -> ClientReplicator&;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

void RegisterNetworkSubsystem(Engine& engine);

} // namespace ZHLN::Net
