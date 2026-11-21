# Scene documents and orchestration belong to the host

Link `zahlen_scene` and include `<Scene/Scene.hpp>`. The consumer library owns
scene storage, instantiation, validation, and extraction.

## COM path

`Instantiate` accepts either `COM::IEngine&` or the four explicit capabilities:
IEntities, IAssetLibrary, IGraphicsDevice and ISimulation. No Registry, native
Engine, ModelPrefab, renderer or importer callback is exposed by this path.
The library's STL return values are ordinary consumer-side C++; they do not
cross a Core shared-library symbol or COM vtable.

The host must decode/upload all prefab sources first using its chosen glTF/BSP
asset library, and retain their ownership manifests for recovery/texture
retirement. Naming a source in a document does not load a file. `Validate`
checks shape/body kinds, light types and cached GPU readiness before mutation.

```cpp
// A host phase boundary, not concurrent with Tick or structural ECS writes.
auto document = ZHLN::Tools::ReadSceneDocument(scenePath); // optional tools library
if (!document) return;
// Host policy here: prepare each Prefab source through glTF/BSP capabilities,
// retaining its Document + owning Asset as appropriate. No implicit import.
if (!ZHLN::Scene::Validate(*engine.Assets(), *document)) return;
engine.ClearScene();
auto instance = ZHLN::Scene::Instantiate(engine, *document);
// Later, at another phase boundary:
auto saved = ZHLN::Scene::Extract(engine);
// Host chooses the saved document's name/path and serializes via TOML/Lua.
```

Boxes/planes use IAssetLibrary::SpawnPrimitive. Camera and environment settings
are applied through components/graphics. Lights retain spot cones, pinned and
baked ownership flags. Prefabs use cached identity and ordinary SpawnParams;
kinematic prefabs receive one root box body, not a body per visual part.
`GetPrefabBounds(hash, OutChain(PrefabBoundsInfo))` copies the union of part-local
AABBs needed to preserve that existing collider policy. No prefab is borrowed.

`Instance` owns entity-ID vectors, not GPU resources. Core still owns scene
cleanup. On an instantiation failure, newly recorded entities are Despawned
through IEntities (deferred cleanup), not erased through raw registry access.
PendingDestroy is published so extraction excludes retiring entities. Runtime
mesh/light entities without scene provenance remain excluded.

## Extraction fidelity

The host library stores a trivially-copyable `Scene::AuthoringComponent` through
the consumer-component path. This retains material/body/start-asleep/draw-layer/
ambient-cube authoring values on prefab roots that have no MeshComponent.
Extraction includes logic-only entities and both spot-cone angles.
Mesh-bearing entities still read live PBR/material values through capabilities.

The document is not a lossless arbitrary ECS snapshot: runtime-only entities,
per-part custom material policies, animation playback and arbitrary consumer
components need their own host persistence. Prefab roots preserve authored
material policy rather than attempting to collapse distinct per-part edits.
Core's bounded Name/SceneSource strings retain their existing length limits.

## Explicit primitive BLAS policy

`IAssetLibrary::SpawnPrimitive` and `Scene::InstantiateOptions` default to
`buildBLAS = true`. `IGraphicsDevice::CreateMesh` defaults to false. Pass
`{.buildBLAS = false}` explicitly for raster-only scenes. The ownership component
records this policy for renderer recreation; the host chooses it rather than
relying on driver detection.

## Document parsing and replacement

Optional `zahlen_scene_documents` exposes engine-free `ReadSceneDocument(path)`
(TOML and Lua when enabled), plus COM `LoadSceneDocument(engine, path, options)`.
Read/parse errors and failed preflight leave the existing world intact. After
successful preflight, Load explicitly clears the old world and instantiates;
a later allocation failure clears the partial replacement to an empty default.
This convenience is **not** transactional restoration of the previous world.
Hosts needing staged replacement should orchestrate it themselves.

Material/morph arenas still do not support general individual release. An
allocation failure can consume material-arena space until renderer teardown;
entity rollback does not reclaim those arena allocations. Instantiate's
settings/camera commit is not a general transaction across arbitrary decorators.

## Limitations

Intermittent skin/morph frame failures remain unresolved.
