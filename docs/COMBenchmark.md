# COM logic world and real Benchmark hosts

## Boundary

`CreateLogicWorld` / `CreateLogicWorldRef` creates a reference-counted,
engine-owned standalone runtime. It acquires the existing World/Jolt lifetime,
but **does not create Engine, KernelSession, GPU, window, audio or input**.
Task dispatch uses the existing inline fallback when no host scheduler exists.
The host still owns any ProcessScope and must release runtimes before retiring
its scheduler. `ILogicWorld` exposes borrowed `IEntities` and `ISimulation`;
Query/Ref::As retains either capability independently. No native World, registry,
physics context, Jolt shape, STL owner or callback crosses the interface.

The factory accepts the reflected PhysicsConfig chain (or defaults), rejects
unsupported/malformed chains and zero capacities. LogicWorldInfo is a reflected
output chain carrying effective capacities and active body count. Advance
rejects non-finite/non-positive dt, flushes deferred hierarchy deletion and steps
physics once. It does not run a game loop or synchronize transforms.
Direct ISimulation::Step retains the existing simulation contract.

The logic component publication is restricted to Transform, WorldTransform,
Hierarchy, Name, PBR, Physics and PendingDestroy. Physics replacement,
detach, deferred hierarchical despawn and Clear release bodies before storage.
Clear removes entity-owned bodies/components; unattached simulation bodies
remain explicitly caller-managed until DestroyBody or world teardown. Canonical
names for **all** native components are reserved, including unpublished GPU/audio
owners, so the dynamic POD path cannot spoof one. Dynamic queries cannot fetch
native typed storage. Dead entities and layout mismatches are refused.
All operations are synchronous and require an isolated single host thread.

The standalone world supports ISimulation's existing body descriptors, inline
mesh geometry, heightfields, compounds, characters, queries and mutations.
Asset-ID-only collision meshes are not available without an asset library and
return AssetNotCached; supply an inline mesh with Invalid asset instead.

## Actual workload

LogicBenchmark now owns a COM Ref, not World or Registry. The same falling cube
lattice, floor slab, growing batches, body ceiling, fixed physics ticks, scalar
per-body position readback and writes into engine-owned Transform storage remain.
The entity iteration is spawn-order host IDs, not a borrowed dense storage map.
The measurement includes scalar COM overhead. Report that cost when comparing it
with native query measurements. No bulk mapping was added without a profiling
case. History stays oldest-first and bounded to 256 ticks; min/max,
online average and monotonic tick timer retain their semantics. Reset drops
ownership and counters; a separately retained capability may keep its world alive.

Sizing preserves caller capacities or raises them. Initial sizing also budgets
the floor when the requested capacity equals the body count.
The no-display `zahlen-benchmark` CLI still measures real physics without renderer
initialization. ZHLN_BENCH_ENTITIES and ZHLN_BENCH_TICKS are unchanged.

## Host-owned viewer

Viewer accepts COM::IEngine and draws panels into the host's active
GUI::Frame. The host owns context, atlas lifetime, input, BeginFrame, End and
SubmitUI. No Run/UI callback inversion or hidden UI compositor is installed.
SetFontAtlas borrows a host-owned atlas until replaced/destruction, or null uses
conservative text metrics. Tick is idempotent per host frame; hidden benchmark
panels leave physics unchanged, while explicitly requested telemetry still samples.
Attaching resets the host-frame identity/device hint and creates a fresh real
logic benchmark. Pause, fixed-step budget, spawn/reset UI, hardware telemetry,
stage histories and CSV capture remain. The first telemetry Tick seeds its clock,
so three fresh host ticks produce two measured capture intervals.

`zahlen-benchmark-viewer` is the real COM composition root and owns its GUI frame
and submission loop. `--headless` requires `--frames N`; common EngineConfig CLI
flags remain host policy. Example:

```
zahlen-benchmark-viewer --headless --frames 5 --validation-mode Off
```

All benchmark hosts and samples consume `zahlen_benchmark_viewer` exclusively
through COM; the transitional native benchmark viewer target has been removed.

## Verification and packaging

- Extras_LogicWorld: five CPU cases covering actual motion, retained capability
  lifetime, typed cleanup/hierarchy, layout/dynamic isolation, sizing, ceiling,
  transform synchronization, 270 ticks/history rollover and reinitialization.
- Extras_BenchmarkHost: real headless COM host, nonempty GUI geometry, submission,
  screenshot lit pixels, telemetry deduplication/CSV output and reattachment.
- Actual headless workload and bounded viewer CLIs run on the existing VM.
- Installed C++26 external find_package(ZahlenBenchmarkViewer) consumer links and
  runs the real Benchmark and constructs the viewer. Packages: Zahlen::Benchmark,
  Zahlen::Viewer and supporting Zahlen::Widgets; Engine exports factory.
  Native viewer header is not installed.

Console / ScriptECSBridge and Option B engine-owned Animator/CharacterMotor are
still pending. This slice makes no claim that those or native compatibility
bridges have been migrated. The accepted independent Image package is unchanged.
