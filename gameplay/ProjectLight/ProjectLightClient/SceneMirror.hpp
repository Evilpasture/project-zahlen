// gameplay/ProjectLight/ProjectLightClient/SceneMirror.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The scene half of the client/server bridge.
//
// ClientSession used to be one class doing two jobs: talking to the server, and
// turning what the server said into entities, lights, character rigs, sky and
// audio. This is the second job. It has no socket, no packet buffer and no
// session state of its own; it walks the mirrored records and writes components.
//
// The two halves meet exactly twice:
//
//   * the applier fills ReplicatedObject records and pushes removals;
//   * the mirror, once a frame, turns those records into a scene.
//
// Everything the mirror needs is passed in as a view (SceneMirrorState) built at
// the call site from the session's own members, so there is still one owner for
// each of them and no mirror-side copy to keep in sync.
//
// Ordering, once a frame, is: removals, then the pose pass, then the spawn/update
// pass, then remote rigs, environment and audio. A pass that needs something from
// an earlier one (a spawn needs the sampled pose) relies on that order and on
// nothing else.

#include <Replication/ReplicaState.hpp>
#include <Replication/ReplicationComponents.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/physics/PhysicsHandles.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ZHLN {
class Engine;
} // namespace ZHLN

namespace ZHLN::ProjectLight {

// A view over the session's state, built per call. Members are references
// because the mirror is a visitor: it must see the session's current objects and
// write back the controller it creates, not a snapshot of either.
struct SceneMirrorState {
    std::unordered_map<InstanceId, ReplicatedObject>&       objects;
    std::unordered_map<InstanceId, NetworkTransformHistory>& controllerStates;
    std::vector<InstanceId>&                                pendingRemovals;
    ReplicationIndex&                                       index;

    const uint64_t& localUserId;
    const uint64_t& localPlayerUid;
    const uint64_t& localCharacterUid;
    const uint64_t& localHumanoidUid;
    const uint64_t& localRootPartUid;
    const uint64_t& lightingUid;
    const uint64_t& workspaceUid;
    const bool&     serverAuthority;
    const bool&     snapshotComplete;
    const float&    time;

    Entity&              localController;
    Physics::BodyHandle& localControllerBody;
    uint64_t&            controllerForRootUid;
    float&               jumpRemaining;
    bool&                wasJumpDown;

    // The one-shot "here is what the first snapshot built" summary. It lives on
    // the session because a scene reset is what should log it again.
    bool& summaryLogged;
};

class SceneMirror {
  public:
    SceneMirror() noexcept  = default;
    ~SceneMirror() noexcept = default;

    // The whole reconcile: removals, poses, spawns/updates, then rigs,
    // environment and audio voices.
    void SyncSceneEntities(Engine& engine, SceneMirrorState& state);

    // Prepares, repairs or tears down the local character controller to match
    // what the server says about this player, and points the camera at it.
    void UpdateLocalController(Engine& engine, SceneMirrorState& state);

    // Drives this client's own character parts from its controller.
    void UpdateLocalCharacterParts(Engine& engine, SceneMirrorState& state, float dt);
};

} // namespace ZHLN::ProjectLight
