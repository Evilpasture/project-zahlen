// extras/Network/NetworkReplicator.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
// ClientReplicator — applies ZHLN.Wire-decoded server state to the local ECS.
// No third-party serialization: every payload goes through the annotated
// reflection codecs exported by ZHLN.Network, and every failure is an
// std::expected with a fully formatted, annotated diagnostic.
// ============================================================================

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
    -> std::expected<void, Error> {
    auto message = DecodeInitialSnapshot(payload);
    if (!message) {
        Log("Net: dropping initial snapshot — {}", message.error().Format());
        return std::unexpected(Error(NetworkError::ReplicationFailed));
    }

    auto& reg = engine.GetRegistry();

    for (const ObjectSnapshot& object: message->objects) {
        const Entity e = GetOrCreateEntity(reg, object.uid);

        const JPH::Vec3  position = object.position;
        const JPH::Vec3  scale    = object.size;
        const JPH::Mat44 worldMat = Math::CreateTransform(position, JPH::Quat::sIdentity(), scale);

        reg.Add(
            e, Components::NameComponent {.name = String64("ReplicatedObject")}, Components::TransformComponent {.position = position, .scale = scale},
            Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
            NetworkIdentityComponent {.serverUID = object.uid, .isLocalOwner = false},
            NetworkInterpolationComponent {.targetPosition = position, .targetRotation = JPH::Quat::sIdentity()}
        );
    }
    return {};
}

auto ClientReplicator::ApplyPhysicsBatch(Engine& engine, std::span<const uint8_t> payload) noexcept
    -> std::expected<void, Error> {
    auto message = DecodePhysicsBatch(payload);
    if (!message) {
        Log("Net: dropping physics batch — {}", message.error().Format());
        return std::unexpected(Error(NetworkError::ReplicationFailed));
    }

    auto& reg = engine.GetRegistry();

    for (const PhysicsBodyState& body: message->bodies) {
        if (const auto* e = uidToEntityMap.Find(body.uid); e != nullptr) {
            reg.Patch<NetworkInterpolationComponent>(*e, [&](NetworkInterpolationComponent& interp) {
                interp.targetPosition = body.position;
                interp.targetRotation = body.rotation;
                interp.linearVelocity = body.velocity;
            });
        }
    }
    return {};
}

auto ClientReplicator::GetOrCreateEntity(ECS::Registry& reg, uint64_t uid) -> Entity {
    if (const auto* found = uidToEntityMap.Find(uid); found != nullptr) {
        if (reg.IsAlive(*found)) {
            return *found;
        }
    }
    const Entity newEntity = reg.Create();
    uidToEntityMap.Insert(uid, newEntity);
    return newEntity;
}

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
