// gameplay/ProjectLight/Replication/ReplicaState.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The wire side of the replication bridge: what the server last said about an
// instance, and the short history that is sampled to smooth it.
//
// This header owns the mirrored record and nothing else. It has no socket, no
// MsgPack and no ECS dependency, so the snapshot applier and the scene passes can
// both include it without either of them owning the other. The entity a record
// belongs to is not here: that is ReplicationIndex's job (see
// ReplicationComponents.hpp), and neither this struct nor anything that reads it
// may keep a second copy of the answer.

#include <DataModel/DataModel.hpp>
#include <Replication/ReplicationComponents.hpp>
#include <Zahlen/Audio/AudioTypes.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Physics/Jolt.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ZHLN::ProjectLight {

struct TransformState {
    JPH::Vec3 position       = JPH::Vec3::sZero();
    JPH::Quat rotation       = JPH::Quat::sIdentity();
    JPH::Vec3 linearVelocity = JPH::Vec3::sZero();
};

struct NetworkTransformSample {
    JPH::Vec3                             position = JPH::Vec3::sZero();
    JPH::Quat                             rotation = JPH::Quat::sIdentity();
    std::chrono::steady_clock::time_point receivedAt;
};

// A small fixed ring keeps enough 60 Hz samples for a short, jitter-tolerant
// 20 ms playout target without imposing the Python client's longer delay.
struct NetworkTransformHistory {
    static constexpr std::size_t Capacity = 8;

    void Push(const NetworkTransformSample& sample) noexcept {
        m_samples[m_next] = sample;
        m_next            = (m_next + 1) % Capacity;
        m_count           = std::min(m_count + 1, Capacity);
    }

    void Clear() noexcept {
        m_next  = 0;
        m_count = 0;
    }

    [[nodiscard]] auto Empty() const noexcept -> bool {
        return m_count == 0;
    }

    // Position and rotation at `renderTime`, interpolated between the two
    // samples that straddle it. Returns false only when there is nothing to
    // sample; the caller decides whether to fall back to the authoritative pose.
    [[nodiscard]] auto Sample(std::chrono::steady_clock::time_point renderTime, JPH::Vec3& position, JPH::Quat& rotation) const noexcept -> bool {
        if (Empty()) return false;

        const std::size_t oldest = (m_next + Capacity - m_count) % Capacity;
        auto sampleAt = [&](std::size_t offset) -> const NetworkTransformSample& {
            return m_samples[(oldest + offset) % Capacity];
        };

        const NetworkTransformSample& first = sampleAt(0);
        const NetworkTransformSample& last  = sampleAt(m_count - 1);
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
            const auto                    span     = current.receivedAt - previous.receivedAt;
            float                         alpha    = 0.0f;
            if (span > std::chrono::steady_clock::duration::zero()) {
                alpha = std::chrono::duration<float>(renderTime - previous.receivedAt).count() / std::chrono::duration<float>(span).count();
            }
            alpha    = std::clamp(alpha, 0.0f, 1.0f);
            position = previous.position + (current.position - previous.position) * alpha;
            rotation = previous.rotation.SLERP(current.rotation, alpha).Normalized();
            return true;
        }

        position = last.position;
        rotation = last.rotation;
        return true;
    }

  private:
    std::array<NetworkTransformSample, Capacity> m_samples {};
    std::size_t                                  m_next  = 0;
    std::size_t                                  m_count = 0;
};

// One instance as the server last described it, plus this client's playout of
// the pose it has been given.
//
// Fields here are wire fields: the applier writes them from snapshot packets and
// nothing else does. Two groups are worth naming because they are easy to
// confuse with scene state:
//
//   * position/rotationQuat/linearVelocity are the authoritative sample. For an
//     instance this client owns they are refreshed from its Jolt body before
//     being sent back out (see SendRealtimeFrame).
//   * renderPosition/renderRotation are the delayed sample handed to the
//     renderer for instances the server drives. They are derived, never sent,
//     and they exist only because the scene write happens a few statements after
//     the sampling that produced it.
//
// What used to be a third group -- the entity, the material id, the spawn
// bookkeeping, the audio handle -- has either moved onto the entity or is
// mirror-side cache with the component as the authority. If a field here
// disagrees with the entity, the entity is right.
struct ReplicatedObject : ProjectLightInstanceComponent {
    std::string className;
    std::string name      = "Instance";
    uint64_t    parentUid = 0u;

    JPH::Vec3               position         = JPH::Vec3::sZero();
    JPH::Vec3               rotationEulerDeg = JPH::Vec3::sZero();
    JPH::Quat               rotationQuat     = JPH::Quat::sIdentity();
    JPH::Vec3               linearVelocity   = JPH::Vec3::sZero();
    JPH::Vec3               renderPosition   = JPH::Vec3::sZero();
    JPH::Quat               renderRotation   = JPH::Quat::sIdentity();
    NetworkTransformHistory physicsHistory {};
    bool                    hasPhysicsSample = false;
    JPH::Vec3               size             = JPH::Vec3(4.0f, 1.0f, 2.0f);
    JPH::Vec3               color            = JPH::Vec3(0.75f, 0.75f, 0.75f);
    float                   transparency     = 0.0f;
    float                   metallic         = 0.0f;
    float                   roughness        = 0.5f;
    float                   emission         = 0.0f;
    bool                    anchored         = false;
    bool                    canCollide       = true;
    uint64_t                networkOwnerUid  = 0u;
    PartShape               shape            = PartShape::Cube;
    std::string             meshId;
    JPH::Vec3               meshScale = JPH::Vec3::sReplicate(1.0f);

    uint64_t      primaryPartUid = 0u;
    uint64_t      rootPartUid    = 0u;
    uint64_t      characterUid   = 0u;
    uint64_t      userId         = 0u;
    HumanoidState humanoidState  = HumanoidState::Idle;
    float         walkSpeed      = 16.0f;
    float         jumpPower      = 50.0f;
    float         health         = 100.0f;
    float         maxHealth      = 100.0f;

    uint64_t  part1Uid        = 0u;
    uint64_t  part2Uid        = 0u;
    JPH::Vec3 offset1         = JPH::Vec3::sZero();
    JPH::Vec3 offset2         = JPH::Vec3::sZero();
    JPH::Vec3 rotationOffset1 = JPH::Vec3::sZero();
    JPH::Vec3 rotationOffset2 = JPH::Vec3::sZero();
    float     currentAngle    = 0.0f;
    float     desiredAngle    = 0.0f;
    float     maxVelocity     = 30.0f;

    bool  enabled      = true;
    float intensity    = 1.0f;
    bool  shadows      = true;
    float sourceRadius = 0.0f;
    // ProjectLight directional vectors describe ray travel; the renderer-facing
    // conversion to a toward-source vector is done at the ECS boundary.
    JPH::Vec3 direction       = JPH::Vec3(0.0f, -1.0f, 0.0f);
    JPH::Vec3 nightColor      = JPH::Vec3(0.45f, 0.55f, 0.8f);
    float     nightIntensity  = 0.35f;
    bool      isMain          = false;
    float     range           = 16.0f;
    float     ambient         = 0.01f;
    float     timeOfDay       = 14.0f;
    float     latitude        = 0.0f;
    bool      proceduralSky   = true;
    bool      serverAuthority = false;

    std::string soundId;
    float       volume  = 0.5f;
    bool        loops   = false;
    bool        playing = false;

    // Mirror-side cache of what the entity was last built with. The entity's
    // components are the scene state; these exist so a pass can tell "the server
    // changed something that needs a rebuild" from "nothing to do", and they are
    // refreshed from the entity on every spawn.
    MaterialID  materialId        = InvalidMaterialID;
    JPH::Vec3   spawnedSize       = JPH::Vec3::sZero();
    PartShape   spawnedShape      = PartShape::Cube;
    uint64_t    spawnedOwnerUid   = 0u;
    bool        hasPhysicsBody    = false;
    bool        spawnedAnchored   = false;
    bool        spawnedCanCollide = true;
    bool        spawnedBlend      = false;
    bool        visualDirty       = true;
    bool        transformDirty    = true;
    bool        removalPending    = false;
    AudioHandle audioHandle       = AudioHandle::Invalid;
};

// Walks the parent chain from a part to a character. Depth-capped because the
// chain comes off the wire and a cycle in it would otherwise hang the frame.
[[nodiscard]] inline auto IsPartOfCharacter(const std::unordered_map<InstanceId, ReplicatedObject>& objects, uint64_t partUid, uint64_t characterUid) noexcept -> bool {
    if (partUid == 0 || characterUid == 0) return false;
    uint64_t parentUid = partUid;
    size_t   depth     = 0;
    while (parentUid != 0 && depth++ < 64) {
        const auto found = objects.find(parentUid);
        if (found == objects.end()) return false;
        parentUid = found->second.parentUid;
        if (parentUid == characterUid) return true;
    }
    return false;
}

[[nodiscard]] inline auto IsLocalCharacterPart(const std::unordered_map<InstanceId, ReplicatedObject>& objects, uint64_t uid, uint64_t localCharacterUid) noexcept -> bool {
    return localCharacterUid != 0 && IsPartOfCharacter(objects, uid, localCharacterUid);
}

} // namespace ZHLN::ProjectLight
