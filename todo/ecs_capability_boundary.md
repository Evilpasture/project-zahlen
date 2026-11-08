# ECS capability boundary: full semantic migration (STAGED, NOT STARTED)

## Status

Patch 7 removed `Engine::GetRegistry()` from the public SDK surface and added
the `Entities` capability facade, `Commands()` and the read-only `Steps()` view.
In-tree code that still needs arbitrary `ECS::Registry` operations reaches it
through the build-tree-only `EngineAccess::RegistryOf` seam. That is a boundary
change, **not** the end state: the raw registry is still visible to roughly 300
in-tree call sites even though an SDK consumer can no longer name it.

This roadmap is Option 3 from the patch 7 review -- the full semantic rewrite.
It is deliberately staged: converting a system to signature-driven queries
changes what it can see, and doing that across every subsystem in one patch
would make a behaviour regression indistinguishable from a migration mistake.

## Why the seam is not the destination

`EngineAccess` says *who* may reach the registry, not *what* they may do with
it. A caller holding `ECS::Registry&` can still `Destroy()` an entity without
`PendingDestroy`, `Add<T>()` over a component that owns a Jolt body, or iterate
`GetRawArray<T>()` from a frame step. Those are exactly the leaks `Entities`
closes. The end state has three legitimate raw-registry seams:

1. `SystemGraph` during compilation (family IDs and hazard checks).
2. `GameModule::ComponentRegistrar` (`void (*)(ECS::Registry&)`) at cold startup.
3. Cold-boot scene deserializers (`Scene::Instantiate`).

Everything else is expressed as a query, a command, or a capability operation.

## Phase 1 -- frame steps stop taking `Registry&` (NOT STARTED)

- [ ] `src/engine/system/*` (`PhysicsSystem`, `RenderSystem`, `CullingSystem`,
      `CameraSystem`, `LODSystem`, `GraphicsSettingsSync`, `SceneCleanupSystem`):
      replace `EngineAccess::RegistryOf(engine)` with `ECS::Query<...>` and
      `ECS::EntityCommandBuffer&` parameters. `SystemGraph.hpp` already has
      `ParameterResolver<EntityCommandBuffer&>` resolving from `carrier.ecb`,
      so no graph plumbing is needed.
- [ ] `src/engine/SystemWiring.cpp`: core frame steps become thin adapters that
      execute the graph; no step function touches the registry directly.
- [ ] `SceneCleanup` becomes the only place `Registry::Destroy` is called.

## Phase 2 -- optional layers move to `Entities()` / `Commands()` (NOT STARTED)

- [ ] `extensions/**`, `plugins/**`, `gameplay/**`: split each call site into
      read-only (`Entities().Get<T>` / `GetSingleton<T>`), lifecycle
      (`Entities().Create/Spawn/Despawn/IsAlive`), owned-resource replacement
      (`Entities().Attach<T>` / `Detach<T>`) or deferred mutation
      (`Commands()` / an `EntityCommandBuffer&` system parameter).
- [ ] Where a system genuinely needs a bulk iteration over a component array,
      add a named, read-only operation to `Entities` (e.g. a `ForEach<T>` that
      yields `const T&`) rather than exposing `GetRawArray<T>()`. Do **not**
      grow `Entities` into a 1:1 `Registry` mirror.
- [ ] Remove `EngineAccess::RegistryOf` from every file in these trees.

## Phase 3 -- tests, samples and tooling (NOT STARTED)

- [ ] `tests/**`, `samples/**`, `app/`, `utils/editor/`: replace the seam with
      `Entities()` / `Commands()` assertions. Fixture helpers that seed a scene
      become cold-start deserializers (the third sanctioned seam) or use
      `Entities().Spawn(...)`.
- [ ] `tests/render/TestApplication.cpp`: the frame-failure probe currently
      clears the plan through `EngineAccess::FramePlanOf` to avoid running GPU
      steps in a headless frame. Replace it with a `Module` installed through
      `InstallModule` whose failing step is anchored after `HostUICallback`,
      and sets `FrameContext::error` (patch 8 added the `FramePlan::Execute`
      circuit breaker). Then delete `FramePlanOf` entirely.
- [ ] Delete `EngineAccess::FramePlanOf`; the plan is then mutated **only** by
      `InstallModule(const Module&)`, with no exceptions.

## Phase 4 -- seal it (NOT STARTED)

- [ ] Remove `EngineAccess::RegistryOf` from the three cold-start consumers by
      giving them explicit, narrow entry points (`ComponentRegistrar` keeps its
      `Registry&` by contract; `Scene::Instantiate` takes `Entities`).
- [ ] `Engine::GetInternalRegistry()` becomes unnecessary and is deleted.
- [ ] `include/Zahlen/Engine/World.hpp` is installed but still publishes
      `GetRegistry()`, `GetMainECB()` and `GetArticulationSystem()`. In practice
      it is reachable only via `EngineAccess::WorldOf`, but the installed header
      should demote those to a build-tree-only seam too.
- [ ] Add a configure-time check that no file outside the engine implementation
      and the three sanctioned seams names `ECS::Registry` -- a *provenance*
      check on a semantic boundary, not a textual ban on `Optional<T&>`
      returns (that ban was removed in patch 6).

## Acceptance

Every public `Engine` accessor is an operation or a read-only view; no in-tree
frame step holds a mutable `ECS::Registry&`; `SceneCleanup` is the single
destruction path; the full `./build/bin/ztest --test-dir build -V` suite stays
green after each phase.
