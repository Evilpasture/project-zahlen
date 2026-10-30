# ProjectLight Replication: ECS as the System of Record (STAGED, NOT STARTED)

## Overview

`gameplay/ProjectLight/ProjectLightClient/` currently keeps the authoritative
replicated scene in `ClientSession::m_objects` -- an
`std::unordered_map<uint64_t, ReplicatedObject>` -- and re-derives the engine's
view of it every frame in `SyncSceneEntities` by copying each `ReplicatedObject`
into `Components::TransformComponent`, `PhysicsComponent` and the render
handles. Two representations of the same scene therefore exist, and the client
is the middleman between them.

The alternative this roadmap moves toward is the one ARCHITECTURE.md 1.1 already
mandates for engine state: **the ECS is the system of record, and
`ReplicatedObject` shrinks to what is genuinely wire-side** (the interpolation
history and the protocol-visible fields a snapshot delivers).

It is staged rather than done in one step on purpose. Both halves of the
migration are unverifiable without a running ProjectLight server and a live
snapshot stream, and the intermediate states are the states that break: a
half-moved field is a field two systems believe they own.

Landed already, and depended on by everything below: the module/composition
seam (`Zahlen/Engine/GameModule.hpp`, `Zahlen/Engine/FramePlan.hpp`). Modules are values the
application installs, so the client's mirror is now something the composition
names and can see, instead of a static callback replayed by the engine.

---

## Phase 1: Give replicated Instances an ECS identity (NOT STARTED)

- [ ] `gameplay/ProjectLight/Replication/ReplicationComponents.hpp`:
      - `ProjectLightInstanceComponent { InstanceId uid; }` -- which replicated
        Instance this entity mirrors. The identity lives with the entity, not
        with the client's map.
      - `ProjectLightNetworkOwnerComponent { uint64_t ownerUid; }` -- the
        authority field `ShouldCreatePhysics` currently reads off the domain
        object.
      - `ProjectLightReplicaTag` / `ProjectLightRemovalPending` -- the two
        states the sync loop currently infers from `removalPending` and class
        name.
- [ ] `ReplicationIndex`: the `InstanceId -> Entity` binding as an *index* the
      mirror maintains, not as per-object state. `ReplicatedObject::entity`
      (42 read/write sites) is what this replaces: the same information, on the
      side that owns the entity.
- [ ] `Descriptor` for the module: `ProjectLight::DescribeClient()` gains
      `registerComponents`, so the components above are registered with the
      module that owns them.

## Phase 2: Split the wire record from the scene state (NOT STARTED)

- [ ] `ReplicatedObject` keeps only what a snapshot delivers and what the
      protocol needs between snapshots: `uid`, `className`, server pose, the
      interpolation history, `removalPending`. Everything the renderer or
      physics reads moves to a component.
- [ ] `LightProtocol.cpp` writes components directly for the fields that have
      one, instead of filling a struct that is later copied into one.
- [ ] `SyncSceneEntities` becomes a reconcile pass over the components that
      changed -- a snapshot-delta walk, not a full-scene copy every frame.

## Phase 3: Make the mirror a system, not a session method (NOT STARTED)

- [ ] `ProjectLight::SceneMirror` -- spawned/despawned entity bindings
      (currently `SpawnOrUpdatePartEntity`, `SpawnOrUpdateLightEntity`,
      `UpdateRemoteCharacterParts`, `SyncEnvironment`, `UpdateSoundVoices`)
      moves out of `ClientSession`, which keeps protocol state and stops
      touching `PrefabFactory` and `TransformComponent`.
- [ ] `ClientSession` keeps a socket, a snapshot buffer and the session state;
      the mirror owns the engine-facing half. The dependency then points one
      way: the client tells the mirror what the server said, and never what the
      engine should look like.

## Phase 4: Retire the duplicated fields (NOT STARTED)

- [ ] Delete every `ReplicatedObject` field that a component now owns, and let
      the compiler find the remaining readers.
- [ ] Extend `tests/extras/` with a mirror test that drives a synthetic snapshot
      through the protocol parser into a registry, so the reconcile pass is
      covered without a server.

---

## What this buys

* One owner per field. Today `position`, `rotationQuat`, `renderPosition`,
  `renderRotation`, `transformDirty` and `TransformComponent` all describe one
  object's pose, and the sync loop is what keeps them agreeing.
* The client stops being a renderer. `ProjectLightClient.cpp` is 2,200 lines
  largely because it is both a protocol implementation and a scene builder.
* Interpolation state, which is genuinely not engine state, stops looking like
  it is: it stays in the wire record where it belongs, and nothing renders from
  it directly.
