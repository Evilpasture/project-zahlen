// extensions/net/Network/NetworkReplicator.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
// ClientReplicator — applies ZHLN.Wire-decoded server state to the local ECS.
// No third-party serialization: every payload goes through the annotated
// reflection codecs exported by ZHLN.Network, and every failure is an
// std::expected with a fully formatted, annotated diagnostic.
// ============================================================================

module;
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Math/Vec3.hpp>

// clang-format off
#include <Zahlen/Physics/Jolt.hpp>
// clang-format on
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Core/Profiler.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <algorithm>
#include <cstdint>
#include <expected>
#include <span>

module ZHLN.Network;

namespace ZHLN::Net {

// ============================================================================
// ClientReplicator — method bodies (class declared in the module interface)
// ============================================================================

auto ClientReplicator::ApplyInitialObjects(Engine& engine, std::span<const uint8_t> payload) noexcept
    -> std::expected<void, ErrorCode> {
    auto message = DecodeInitialSnapshot(payload);
    if (!message) {
        Log("Net: dropping initial snapshot — {}", message.error().Format());
        return std::unexpected(ErrorCode(NetworkError::ReplicationFailed));
    }

    auto entities = engine.Entities();

    for (const ObjectSnapshot& object: message->objects) {
        const Entity e = GetOrCreateEntity(entities, object.uid);

        const FVec3  position = object.position;
        const FVec3  scale    = object.size;
        const auto worldMat = ComposeTransform(position, Quat::Identity(), scale);

        entities.Attach(
            e, Components::NameComponent {.name = String64("ReplicatedObject")}, Components::TransformComponent {.position = position, .scale = scale},
            Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
            NetworkIdentityComponent {.serverUID = object.uid, .isLocalOwner = false},
            NetworkInterpolationComponent {.targetPosition = position, .targetRotation = {0.0f, 0.0f, 0.0f, 1.0f}}
        );
    }
    return {};
}

auto ClientReplicator::ApplyPhysicsBatch(Engine& engine, std::span<const uint8_t> payload) noexcept
    -> std::expected<void, ErrorCode> {
    auto message = DecodePhysicsBatch(payload);
    if (!message) {
        Log("Net: dropping physics batch — {}", message.error().Format());
        return std::unexpected(ErrorCode(NetworkError::ReplicationFailed));
    }

    auto entities = engine.Entities();

    for (const PhysicsBodyState& body: message->bodies) {
        if (const auto e = uidToEntityMap.Find(body.uid)) {
            entities.Patch<NetworkInterpolationComponent>(*e, [&](NetworkInterpolationComponent& interp) {
                interp.targetPosition = body.position;
                interp.targetRotation = body.rotation;
                interp.linearVelocity = body.velocity;
            });
        }
    }
    return {};
}

auto ClientReplicator::GetOrCreateEntity(Entities& entities, uint64_t uid) -> Entity {
    if (const auto found = uidToEntityMap.Find(uid)) {
        if (entities.IsAlive(*found)) {
            return *found;
        }
    }
    const Entity newEntity = entities.Create();
    uidToEntityMap.Insert(uid, newEntity);
    return newEntity;
}

// ============================================================================
// ECS Subsystem Registration
// ============================================================================

void NetworkInterpolationSystem(
    ECS::Query<const NetworkInterpolationComponent, const NetworkIdentityComponent, Components::TransformComponent&> query,
    FrameDt dt
) {
    ZHLN::ScopedTimer timer("ECS System: Network Interpolation");

    for (Entity e: query.Entities<NetworkInterpolationComponent>()) {
        const auto ident = query.Get<NetworkIdentityComponent>(e);
        if (ident && ident->isLocalOwner) {
            continue;
        }
        const auto interp = query.Get<NetworkInterpolationComponent>(e);
        auto       trans  = query.Get<Components::TransformComponent>(e);
        if (!trans || !interp) {
            continue;
        }
        const float t  = std::min(1.0f, interp->interpolationSpeed * dt.value);
        trans->position = Lerp(trans->position, interp->targetPosition, t);
        trans->rotation = Slerp(trans->rotation, interp->targetRotation, t);
    }
}

void RegisterNetworkSubsystem(Engine& engine) {
    auto& graph = engine.GetUpdateGraph();

    graph.AddSystem<&NetworkInterpolationSystem>();
}

} // namespace ZHLN::Net
