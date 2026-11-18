// extensions/net/Network/NetworkReplicator.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
// ClientReplicator — applies ZHLN.Wire-decoded server state over COM::IEntities.
// No engine-internal ECS or physics dependencies cross this layer.
// ============================================================================

module;
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/Profiler.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <algorithm>
#include <cstdint>
#include <expected>
#include <span>

module ZHLN.Network;

namespace ZHLN::Net {

auto ClientReplicator::ApplyInitialObjects(COM::IEntities& entities, std::span<const uint8_t> payload) noexcept
    -> std::expected<void, ErrorCode> {
    auto message = DecodeInitialSnapshot(payload);
    if (!message) {
        Log("Net: dropping initial snapshot — {}", message.error().Format());
        return std::unexpected(ErrorCode(Error::ReplicationFailed));
    }

    for (const ObjectSnapshot& object : message->objects) {
        const Entity e        = GetOrCreateEntity(entities, object.uid);
        const FVec3  position = object.position;
        const FVec3  scale    = object.size;
        const auto   worldMat = ComposeTransform(position, Quat::Identity(), scale);

        (void)entities.Attach(e, Components::NameComponent {.name = String64("ReplicatedObject")});
        (void)entities.Attach(e, Components::TransformComponent {.position = position, .scale = scale});
        (void)entities.Attach(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});
        (void)entities.Attach(e, IdentityComponent {.serverUID = object.uid, .isLocalOwner = false});
        (void)entities.Attach(
            e, InterpolationComponent {.targetPosition = position, .targetRotation = Quat::Identity()});
    }
    return {};
}

auto ClientReplicator::ApplyPhysicsBatch(COM::IEntities& entities, std::span<const uint8_t> payload) noexcept
    -> std::expected<void, ErrorCode> {
    auto message = DecodePhysicsBatch(payload);
    if (!message) {
        Log("Net: dropping physics batch — {}", message.error().Format());
        return std::unexpected(ErrorCode(Error::ReplicationFailed));
    }

    for (const PhysicsBodyState& body : message->bodies) {
        if (const auto e = uidToEntityMap.Find(body.uid)) {
            (void)entities.Patch<InterpolationComponent>(*e, [&](InterpolationComponent& interp) {
                interp.targetPosition = body.position;
                interp.targetRotation = body.rotation;
                interp.linearVelocity = body.velocity;
            });
        }
    }
    return {};
}

auto ClientReplicator::GetOrCreateEntity(COM::IEntities& entities, uint64_t uid) -> Entity {
    if (const auto found = uidToEntityMap.Find(uid)) {
        if (entities.IsAlive(*found)) {
            return *found;
        }
    }
    const Entity newEntity = entities.CreateEntity();
    uidToEntityMap.Insert(uid, newEntity);
    return newEntity;
}

void Step(COM::IEntities& entities, float dt) noexcept {
    ZHLN::ScopedTimer timer("Network: Interpolation");

    const float clampedDt = std::max(0.0f, dt);
    for (const Entity e : entities.EntitiesWith<InterpolationComponent>()) {
        if (const auto* ident = entities.Get<IdentityComponent>(e); ident != nullptr && ident->isLocalOwner) {
            continue;
        }
        const auto* interp = entities.Get<InterpolationComponent>(e);
        auto*       trans  = entities.Get<Components::TransformComponent>(e);
        if (trans == nullptr || interp == nullptr) {
            continue;
        }
        const float t   = std::min(1.0f, interp->interpolationSpeed * clampedDt);
        trans->position = Lerp(trans->position, interp->targetPosition, t);
        trans->rotation = Slerp(trans->rotation, interp->targetRotation, t);
    }
}

void Step(COM::IEngine& engine, float dt) noexcept {
    if (auto* entities = engine.Entities()) {
        Step(*entities, dt);
    }
}

} // namespace ZHLN::Net
