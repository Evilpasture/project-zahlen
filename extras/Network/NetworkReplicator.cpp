// extras/Network/NetworkReplicator.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <msgpack.hpp>
#include <zlib.h>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

module ZHLN.Network;

namespace ZHLN::Net {

namespace {

constexpr uint8_t FLAG_COMPRESSED        = 0x01;
constexpr size_t  MAX_STREAM_FRAME_BYTES = 128 * 1024 * 1024;
constexpr size_t  COMPRESSION_MIN_BYTES  = 1024;

// Module-internal framing: Encodes msgpack bytes with 4-byte BE length + compression flag
auto EncodePackedFrame(std::span<const uint8_t> packed) noexcept -> std::expected<std::vector<uint8_t>, Error> {
    try {
        uint8_t              flags = 0;
        std::vector<uint8_t> body;

        if (packed.size() >= COMPRESSION_MIN_BYTES) {
            uLongf               destLen = compressBound(static_cast<uLong>(packed.size()));
            std::vector<uint8_t> compressed(destLen);
            if (compress(compressed.data(), &destLen, packed.data(), static_cast<uLong>(packed.size())) == Z_OK) {
                if (destLen + 32 < packed.size()) {
                    compressed.resize(destLen);
                    body = std::move(compressed);
                    flags |= FLAG_COMPRESSED;
                }
            }
        }

        if (body.empty()) {
            body.assign(packed.begin(), packed.end());
        }

        const uint32_t totalBodyLen = static_cast<uint32_t>(body.size() + 1);
        const uint32_t netLen       = htonl(totalBodyLen);

        std::vector<uint8_t> frame;
        frame.reserve(4 + totalBodyLen);

        const auto* lenBytes = reinterpret_cast<const uint8_t*>(&netLen);
        frame.insert(frame.end(), lenBytes, lenBytes + 4);
        frame.push_back(flags);
        frame.insert(frame.end(), body.begin(), body.end());

        return frame;
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error(NetworkError::AllocationFailed));
    } catch (...) {
        return std::unexpected(Error(NetworkError::UnpackFailed));
    }
}

// Module-internal framing: Decompresses zlib payload
auto DecodePayloadBody(std::span<const uint8_t> framedBody) noexcept -> std::expected<std::vector<uint8_t>, Error> {
    if (framedBody.empty()) {
        return std::unexpected(Error(NetworkError::InvalidPayload));
    }

    try {
        const uint8_t flags        = framedBody[0];
        auto          payloadBytes = framedBody.subspan(1);

        if ((flags & FLAG_COMPRESSED) != 0) {
            z_stream stream {};
            if (inflateInit(&stream) != Z_OK) {
                return std::unexpected(Error(NetworkError::DecompressionFailed));
            }

            stream.avail_in = static_cast<uInt>(payloadBytes.size());
            stream.next_in  = const_cast<Bytef*>(payloadBytes.data());

            std::vector<uint8_t> decompressed(64 * 1024);
            int                  ret = Z_OK;
            while (ret == Z_OK) {
                if (stream.total_out >= decompressed.size()) {
                    decompressed.resize(decompressed.size() * 2);
                }
                stream.avail_out = static_cast<uInt>(decompressed.size() - stream.total_out);
                stream.next_out  = decompressed.data() + stream.total_out;
                ret              = inflate(&stream, Z_NO_FLUSH);
            }
            inflateEnd(&stream);

            if (ret != Z_STREAM_END) {
                return std::unexpected(Error(NetworkError::DecompressionFailed));
            }

            decompressed.resize(stream.total_out);
            return decompressed;
        }

        return std::vector<uint8_t>(payloadBytes.begin(), payloadBytes.end());
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error(NetworkError::AllocationFailed));
    } catch (...) {
        return std::unexpected(Error(NetworkError::DecompressionFailed));
    }
}

// Module-internal MessagePack sandboxing
auto SafeUnpack(std::span<const uint8_t> data) noexcept -> std::expected<msgpack::object_handle, Error> {
    try {
        return msgpack::unpack(reinterpret_cast<const char*>(data.data()), data.size());
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error(NetworkError::AllocationFailed));
    } catch (...) {
        return std::unexpected(Error(NetworkError::UnpackFailed));
    }
}

} // namespace

// ============================================================================
// ClientReplicator Definition
// ============================================================================

class ClientReplicator {
  public:
    HashMap<uint64_t, Entity> uidToEntityMap;

    auto ApplyInitialObjects(Engine& engine, std::span<const uint8_t> rawMsgPack) noexcept -> std::expected<void, Error> {
        auto unpacked = SafeUnpack(rawMsgPack);
        if (!unpacked) {
            return std::unexpected(unpacked.error());
        }

        try {
            const msgpack::object obj = unpacked->get();
            if (obj.type != msgpack::type::MAP) {
                return std::unexpected(Error(NetworkError::InvalidPayload));
            }

            const auto snapshot = obj.via.map;
            auto&      reg      = engine.GetRegistry();

            for (uint32_t i = 0; i < snapshot.size; ++i) {
                const uint64_t uid   = std::stoull(snapshot.ptr[i].key.as<std::string>());
                const auto     props = snapshot.ptr[i].val.as<std::map<uint8_t, msgpack::object>>();

                const Entity e = GetOrCreateEntity(reg, uid);

                JPH::Vec3 pos   = JPH::Vec3::sZero();
                JPH::Vec3 scale = JPH::Vec3::sReplicate(1.0f);

                if (props.contains(static_cast<uint8_t>(PropertyChannel::Position))) {
                    auto v = props.at(static_cast<uint8_t>(PropertyChannel::Position)).as<std::vector<float>>();
                    pos    = JPH::Vec3(v[0] * INV_VEC3_SCALE, v[1] * INV_VEC3_SCALE, v[2] * INV_VEC3_SCALE);
                }
                if (props.contains(static_cast<uint8_t>(PropertyChannel::Size))) {
                    auto s = props.at(static_cast<uint8_t>(PropertyChannel::Size)).as<std::vector<float>>();
                    scale  = JPH::Vec3(s[0] * INV_VEC3_SCALE, s[1] * INV_VEC3_SCALE, s[2] * INV_VEC3_SCALE);
                }

                const JPH::Mat44 worldMat = Math::CreateTransform(pos, JPH::Quat::sIdentity(), scale);

                reg.Add(
                    e, Components::NameComponent {.name = String64("ReplicatedObject")}, Components::TransformComponent {.position = pos, .scale = scale},
                    Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
                    NetworkIdentityComponent {.serverUID = uid, .isLocalOwner = false},
                    NetworkInterpolationComponent {.targetPosition = pos, .targetRotation = JPH::Quat::sIdentity()}
                );
            }
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error(NetworkError::AllocationFailed));
        } catch (...) {
            return std::unexpected(Error(NetworkError::UnpackFailed));
        }
    }

    auto ApplyPhysicsBatch(Engine& engine, std::span<const uint8_t> rawMsgPack) noexcept -> std::expected<void, Error> {
        auto unpacked = SafeUnpack(rawMsgPack);
        if (!unpacked) {
            return std::unexpected(unpacked.error());
        }

        try {
            const msgpack::object obj = unpacked->get();
            if (obj.type != msgpack::type::MAP) {
                return std::unexpected(Error(NetworkError::InvalidPayload));
            }

            const auto physicsMap = obj.via.map;
            auto&      reg        = engine.GetRegistry();

            for (uint32_t i = 0; i < physicsMap.size; ++i) {
                const uint64_t uid = std::stoull(physicsMap.ptr[i].key.as<std::string>());
                const auto     raw = physicsMap.ptr[i].val.as<std::vector<int32_t>>();
                if (raw.size() < 10) {
                    continue;
                }

                const JPH::Vec3 pos(raw[0] * INV_VEC3_SCALE, raw[1] * INV_VEC3_SCALE, raw[2] * INV_VEC3_SCALE);
                const JPH::Quat rot(raw[3] * INV_VEC3_SCALE, raw[4] * INV_VEC3_SCALE, raw[5] * INV_VEC3_SCALE, raw[6] * INV_VEC3_SCALE);
                const JPH::Vec3 vel(raw[7] * INV_VEC3_SCALE, raw[8] * INV_VEC3_SCALE, raw[9] * INV_VEC3_SCALE);

                if (const auto* e = uidToEntityMap.Find(uid)) {
                    reg.Patch<NetworkInterpolationComponent>(*e, [&](auto& interp) {
                        interp.targetPosition = pos;
                        interp.targetRotation = rot.Normalized();
                        interp.linearVelocity = vel;
                    });
                }
            }
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(Error(NetworkError::AllocationFailed));
        } catch (...) {
            return std::unexpected(Error(NetworkError::UnpackFailed));
        }
    }

    Entity GetOrCreateEntity(ECS::Registry& reg, uint64_t uid) {
        if (const auto* found = uidToEntityMap.Find(uid)) {
            if (reg.IsAlive(*found)) {
                return *found;
            }
        }
        const Entity newEntity = reg.Create();
        uidToEntityMap.Insert(uid, newEntity);
        return newEntity;
    }
};

// ============================================================================
// ECS Subsystem Registration
// ============================================================================

void NetworkInterpolationSystem(Engine& engine, float dt) {
    ZHLN::ScopedTimer timer("ECS System: Network Interpolation");
    auto&             reg = engine.GetRegistry();

    for (Entity e: reg.GetEntitiesWith<NetworkInterpolationComponent>()) {
        const auto* ident = reg.Get<NetworkIdentityComponent>(e);
        if (ident != nullptr && ident->isLocalOwner) {
            continue;
        }

        reg.Patch<Components::TransformComponent, NetworkInterpolationComponent>(
            e, [&](Components::TransformComponent& trans, const NetworkInterpolationComponent& interp) {
                const float t  = std::min(1.0f, interp.interpolationSpeed * dt);
                trans.position = trans.position + (interp.targetPosition - trans.position) * t;
                trans.rotation = trans.rotation.SLERP(interp.targetRotation, t).Normalized();
            }
        );
    }
}

void RegisterNetworkSubsystem(Engine& engine) {
    auto& graph = engine.GetUpdateGraph();

    graph.AddSystem(
        {.update_func    = [](Engine& eng, float dt) { NetworkInterpolationSystem(eng, dt); },
         .name           = "NetworkInterpolationSystem",
         .access_pattern = {ECS::Write<Components::TransformComponent>(), ECS::Read<NetworkInterpolationComponent>()},
         .enabled        = true}
    );
}

} // namespace ZHLN::Net
