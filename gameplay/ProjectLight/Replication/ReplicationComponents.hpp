// gameplay/ProjectLight/Replication/ReplicationComponents.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The ECS half of the project-light client/server bridge.
//
// What lives here, and what does not
// ----------------------------------
// A replicated instance has three things the scene side needs to ask about it:
// which instance it is, who the server says owns it, and whether the server has
// asked for it to go away. Those are components on the entity that carries the
// instance, because that is the thing the rest of the frame walks.
//
// The mirrored record (ReplicatedObject in ProjectLightClient/LightProtocol.hpp)
// stays where it is: it is the deserializer's wire state, "what the server last
// said", keyed by instance uid and only meaningful to the snapshot applier and
// the passes that turn it into components. It is a cache in front of the entity,
// never a second place to look up where an entity is. The uid -> entity binding
// that used to be a field on that record is a ReplicationIndex, one owner that
// survives spawning, despawning and re-spawning of the same instance.
//
// Why an index and not a component: the binding has to exist for the whole life
// of the instance, including the window between "the snapshot named it" and "an
// entity exists for it", and it has to be looked up by uid (the server's
// vocabulary) rather than by entity (ours). A per-entity component can't answer
// "which entity is instance 17" without a scan; an index can, and it is the one
// place that answer lives.
//
// Scaling note: ReplicationIndex models one client's view of one server's
// DataModel, which is a single flat namespace of ids the client does not
// disjoint. A map is correct here. If the client ever hosts several servers at
// once, this becomes a per-connection index rather than a bigger map.

#include <DataModel/DataModel.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>

namespace ZHLN::ProjectLight {

// The instance this entity was created for. Written once, at spawn, from the
// snapshot's uid, so a system holding only an entity can name what it is looking
// at without reaching back into the client's mirror.
struct ProjectLightInstanceComponent {
    InstanceId uid = 0;
};

// The uid the server reports as the network owner for this instance. Kept
// separate from ProjectLightInstanceComponent because ownership changes while
// the instance identity does not.
struct ProjectLightNetworkOwnerComponent {
    uint64_t ownerUid = 0u;
};

// Marks an entity created by the replication bridge rather than authored in the
// scene. Systems that must not touch replicated content (and the tests that
// check what the bridge produced) filter on this instead of on a uid range.
struct ProjectLightReplicaTag {};

// The server asked for this instance to be removed. It is set when the removal
// is applied and consumed when the entity is despawned, so a removal that
// arrives before the entity exists is still honoured.
struct ProjectLightRemovalPending {};

// uid -> entity for every live replicated instance.
//
// Bind on spawn, Take on despawn. Find on a uid that has no entity returns
// Entity::Null(); that is the normal answer for an instance the mirror knows
// about whose entity has not been created yet (or has just been destroyed).
class ReplicationIndex {
  public:
    ReplicationIndex() noexcept                        = default;
    ~ReplicationIndex() noexcept                       = default;
    ReplicationIndex(const ReplicationIndex&)          = delete;
    auto operator=(const ReplicationIndex&) -> ReplicationIndex& = delete;
    ReplicationIndex(ReplicationIndex&&) noexcept      = default;
    auto operator=(ReplicationIndex&&) noexcept -> ReplicationIndex& = default;

    void Bind(InstanceId uid, Entity entity) noexcept {
        m_entities[uid] = entity;
    }

    [[nodiscard]] auto Find(InstanceId uid) const noexcept -> Entity {
        const auto it = m_entities.find(uid);
        return it == m_entities.end() ? Entity::Null() : it->second;
    }

    // Unbind and hand back what was bound, so the caller can despawn the entity
    // it just removed from the index without a second lookup.
    [[nodiscard]] auto Take(InstanceId uid) noexcept -> Entity {
        const auto it = m_entities.find(uid);
        if (it == m_entities.end()) {
            return Entity::Null();
        }
        const Entity entity = it->second;
        m_entities.erase(it);
        return entity;
    }

    void Unbind(InstanceId uid) noexcept {
        m_entities.erase(uid);
    }

    void Clear() noexcept {
        m_entities.clear();
    }

    [[nodiscard]] auto Count() const noexcept -> std::size_t {
        return m_entities.size();
    }

    [[nodiscard]] auto Empty() const noexcept -> bool {
        return m_entities.empty();
    }

    [[nodiscard]] auto Entries() const noexcept -> const std::unordered_map<InstanceId, Entity>& {
        return m_entities;
    }

  private:
    std::unordered_map<InstanceId, Entity> m_entities;
};

// A module registers the components it owns; this is the client's half of that
// contract. The scene graph needs uids (a find-by-uid pass and a debug overlay
// both walk the registry, not the mirror), and an unregistered family can be
// written but never read back through Registry::Get.
inline void RegisterReplicationComponents(ECS::Registry& registry) {
    registry.RegisterComponent<ProjectLightInstanceComponent>();
    registry.RegisterComponent<ProjectLightNetworkOwnerComponent>();
    registry.RegisterComponent<ProjectLightReplicaTag>();
    registry.RegisterComponent<ProjectLightRemovalPending>();
}

} // namespace ZHLN::ProjectLight
