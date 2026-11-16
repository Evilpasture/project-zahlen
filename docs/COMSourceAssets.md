# Source/StudioModel capability authoring

This is the next plugin sub-cut after the COM host migration, not a declaration
that all BSP/glTF consumers have been ported.

## New consumer surface

Link `zahlen_bsp_assets` and include `<BSP/StudioModelAssets.hpp>`. This archive
contains Source CPU formats and the capability-only StudioModel authoring path;
it does not link `zahlen_bsp`, Interaction, NativeHostUI or any native host.

```cpp
BSP::SourceVFS source(*engine.Assets()); // borrowed capability
source.AddSearchPath("import/source"); // optional disk fallback
BSP::StudioAssetOptions options;         // native import defaults, BLAS enabled
auto asset = BSP::LoadStudioAsset(*engine.Assets(), *engine.Graphics(), source,
                                 "models/props/chair.mdl", options);
if (!asset) return std::unexpected(asset.error());
auto entities = engine.Assets()->Instantiate(asset->hash,
    PrefabFactory::SpawnParams {.createPhysics = true});
```

`LoadStudioAssetFromMemory` accepts caller byte spans; `UploadStudioAsset`
accepts parsed `StudioModelData`. No native ModelPrefab, Engine, GraphicsDevice,
RenderContext, AssetManager, Jolt object or registry crosses this path. CPU
bounds now use FVec3 rather than JPH::Float3. Calls to COM capabilities are
ordinary public calls, not a native Engine unwrap or a parallel dispatch table.

SourceVFS optionally borrows IAssetLibrary. Reads prefer the embedded map pak,
then that capability's VFS, then the existing case-insensitive disk search.
Mounted Source models, their VVD/VTX/optional PHY companions, VMT includes and
texture bytes therefore work without a native asset-cache borrow. The supplied
capability must outlive SourceVFS and all of its copies. Empty and missing files
remain distinct. Existing disk-only clients keep the default constructor.

The shared VMT lighting policy moved out of BSPImporter.cpp into VMTLighting;
the native brush importer and both Studio paths call the same implementation.
Studio material parsing/decoding is likewise shared through an upload callable,
so the COM path does not copy a second material parser or import native graphics.
The legacy BSP header includes the new lighting header for source compatibility.

## Ownership and errors

- Upload validates CPU stream sizes, indices and bounds before the first GPU
  allocation. Mesh, texture and material capability errors propagate unchanged.
- A pre-registration guard destroys uploaded meshes and unloads textures on any
  later failure. Successful RegisterPrefab transfers mesh ownership to the
  engine cache. The returned StudioAsset carries the newly acquired texture
  handles: keep the manifest while users exist; unload only after despawning
  users and clearing the prefab cache. It has no automatic destructive teardown.
- A cache hit acquires no resources and returns an empty texture manifest. The
  original owner must keep its textures alive; cache identity is the virtual
  path, as in the legacy Studio importer. Use a distinct path for other options.
- Source parse/read failures retain the existing box-placeholder policy;
  capability errors are not silently replaced by placeholders.
- PrefabPartInfo adds `useBoxForMeshCollider`, preserving the Studio importer's
  intentional box shape for BOTH mesh/static and box/dynamic choices. It is
  rejected without hasBoxCollider or together with a triangle collision asset.
  This avoids passing Physics::AnyShape or substituting a triangle-mesh box.
  The reflected layout changes, so rebuild both sides. No Core/COM protocol or
  vtable changes are needed.
- BLAS defaults on. Raster-only callers can explicitly select buildBLAS=false.
  VM positive-upload tests use that option because the historical lavapipe BLAS
  crash is not fixed by this migration; hardware BLAS is not claimed tested.

## Verification and remaining work

Extras_BSPAssets links only the new archive plus Engine, Signal and CommandLine,
not the legacy BSP/Interaction libraries. It exercises real MDL/VVD/VTX imports
from mounted VFS and memory, PNG decode/upload, meshlets, prefab instantiation,
cache hits, both physics collider choices, typed failure rollback after actual
mesh/texture allocations, malformed/missing placeholders and invalid policy.
The native BSP suites remain enabled and passing against the shared helpers.

Legacy LoadStudioModelPrefab(Engine&, ...) remains solely for the unported BSP
world importer and its native consumers. It is not counted as a migrated entry
point. World BSP authoring, glTF authoring/inspector/recovery, Scene document and
orchestration relocation, then host-driven extensions are still required.
This sub-cut does not add transparent device-loss reimport or installed-package
integration; neither is implied by its runtime tests.
