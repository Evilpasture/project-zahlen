# Source/StudioModel capability authoring

## Usage

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
accepts parsed `StudioModelData`. CPU bounds use `FVec3`.

SourceVFS optionally borrows IAssetLibrary. Reads prefer the embedded map pak,
then that capability's VFS, then the existing case-insensitive disk search.
Mounted Source models, their VVD/VTX/optional PHY companions, VMT includes and
texture bytes therefore work without a native asset-cache borrow. The supplied
capability must outlive SourceVFS and all of its copies. Empty and missing files
remain distinct. Existing disk-only clients keep the default constructor.

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
- `PrefabPartInfo::useBoxForMeshCollider` selects a box for mesh/static and
  box/dynamic collision. It requires `hasBoxCollider` and cannot be combined
  with a triangle collision asset.
- BLAS defaults on. Set `buildBLAS = false` for raster-only uploads. The
  lavapipe BLAS crash remains unresolved.

See [world BSP authoring](COMBSPWorldAssets.md) for map import.
Device-loss reimport requires explicit host orchestration.
