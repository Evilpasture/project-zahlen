# Scene documents and orchestration belong to the host

Link `zahlen_scene`, include `<Scene/Scene.hpp>`. The old
`<Zahlen/Engine/Scene.hpp>` and Core Scene.cpp are removed. All STL scene
storage, Instantiate, Validate and Extract live in the consumer library.
Core keeps only the small `Scene::ShapeKind`/bake ownership constants used by
published components, in `<Zahlen/ECS/SceneValues.hpp>`.

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
It does not add a document, string or vector to Core. Extract also captures
logic-only entities and both spot-cone angles, which the old extractor omitted.
Mesh-bearing entities still read live PBR/material values through capabilities.

The document is not a lossless arbitrary ECS snapshot: runtime-only entities,
per-part custom material policies, animation playback and arbitrary consumer
components need their own host persistence. Prefab roots preserve authored
material policy rather than attempting to collapse distinct per-part edits.
Core's bounded Name/SceneSource strings retain their existing length limits.

## Explicit primitive BLAS policy

The old primitive builders always built BLAS even when CreateMesh's
MeshBuildOptions requested raster-only geometry. The shared private builders
now honour the option for all six geometry families. Public native mesh-builder
symbols remain wrappers with their original BLAS-enabled behaviour.

IAssetLibrary::SpawnPrimitive accepts optional MeshBuildOptions. Its default is
**true**, preserving existing native spawn behaviour. IGraphicsDevice::CreateMesh
keeps its existing MeshBuildOptions default **false**, now honoured for primitive
geometry as well as raw streams. Scene::InstantiateOptions defaults to true;
pass `{.buildBLAS = false}` explicitly for raster-only scenes/tests.
OwnedMeshComponent records the policy so explicit renderer recreation does not
silently re-enable BLAS. Rebuild the engine and consumers together: the POD
component gained that field and layout checks reject stale callers.

This is not a driver workaround hidden behind environment detection. The
host chooses the policy; the NVIDIA BLAS path is not disabled. VM tests use
explicit raster-only geometry because of the previously reported lavapipe issue.

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
entity rollback does not claim to undo those arena allocations. Instantiate's
settings/camera commit is not a general transaction across arbitrary decorators.

## Core fixes exercised by the migration

- A process-global dynamic family name now initializes separate storage in each
  registry. Global byte-layout metadata validates repeated registration and
  prevents an empty second registry from converting a known typed family to raw
  storage. Typed ownership guards and size/alignment refusal remain enforced.
- A prefab part's valid no-node sentinel (`nodeIndex == -1`) uses identity rather
  than indexing an empty node vector during instantiation.

## COM-only surface

The transitional `zahlen_native_scene` and `zahlen_native_scene_documents`
adapters have been removed; all scene hosts, editors, and document loaders
consume `zahlen_scene` and `zahlen_scene_documents` exclusively over COM.

BSP DescribeScene moved into zahlen_bsp_assets and consumes the host document;
a COM host no longer needs the legacy importer archive just to describe a map.
TOML/Lua, fallback, editor/sample and tests have updated includes/link targets.
The world editor, diagnostic scene host and fallback module are still native
consumers behind those explicitly named adapters; this slice does not claim
that their host loops, inspector or extension modules have been ported.
FreeCam/Camera/Terrain/VFX/editor/Console/Benchmark host-loop ports and the
CharacterMotor/Animator runtime cut remain subsequent work.

The Scene suite validates real GPU allocation, physics, document round-trips and
renderer-recreation **state**, without submitting scene draw frames. It does
not resolve the separately reported intermittent skin/morph frame failure or
claim pixel-conformance acceptance.
