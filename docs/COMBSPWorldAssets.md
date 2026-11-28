# Capability world BSP authoring

`zahlen_bsp_assets` provides world and moving-brush authoring.

## Host-owned import documents

`BSPAssets.hpp` exposes:

- `ReadAssets`: read a Source VFS/disk file, parse and prepare it.
- `DecodeAssets`: parse supplied bytes, retain embedded pak data and prepare.
- `PrepareAssets`: prepare an already parsed Map.
- `UploadAsset`: upload/register one prepared prefab through IAssetLibrary and
  IGraphicsDevice, returning its caller-owned SourceAsset texture manifest.

`PreparedAssets` is a consumer-side document containing a SourceVFS, world and
moving-brush geometry documents, and deduplicated StudioModel import requests.
Neither the documents nor their strings/vectors enter Core. Their decoded
geometry uses FVec3 bounds rather than vendor JPH::Float3 values.

```cpp
BSP::AssetOptions options;
BSP::SourceVFS source(*engine.Assets());
auto prepared = BSP::ReadAssets(std::move(source), "maps/level.bsp", options.import);
if (!prepared) return std::unexpected(prepared.error());
for (const auto& document: prepared->prefabs) {
    auto uploaded = BSP::UploadAsset(*engine.Assets(), *engine.Graphics(),
                                      prepared->source, document, options);
    if (!uploaded) return std::unexpected(uploaded.error());
    session.importedAssets.push_back(std::move(*uploaded)); // persistent ownership ledger
}
for (const auto& model: prepared->studioModels) {
    auto uploaded = BSP::LoadStudioAsset(*engine.Assets(), *engine.Graphics(),
        prepared->source, model, BSP::StudioAssetOptions {.import = options.import, .buildBLAS = options.buildBLAS});
    if (!uploaded) return std::unexpected(uploaded.error());
    session.importedAssets.push_back(std::move(*uploaded));
}
```

`session.importedAssets` retains ownership of previously uploaded textures after
a later submodel failure. `UploadAsset` registers one prefab at a time. Despawn users and
clear the prefab cache before unloading successful texture manifests.
A cache hit returns no newly owned texture handles. As before, virtual path is
cache identity; use another path for incompatible import options.

## Preserved behavior

The CPU importer still supplies displacement geometry, skybox placement, UVs,
VRAD decoding, collision-only tool streams and lighting diagnostics. Material
resolution, VMT includes, shader/emission/mask rules and normal conversion are
shared with the legacy importer rather than reimplemented in the COM path.

Moving door/button submodels are omitted from the world mesh and collider,
cached under `<map>#model_N`, and rebased only when their vertices were authored
in world space. Already-local geometry stays local. Moving brushes do not get
static VRAD atlas pages. Authored lights are not gathered into the world prefab
in addition to the scene description; existing optional lighting-floor behavior
is retained. Static/dynamic Studio prop model requests remain explicit host work.

World uploads retain:

- linear RGBA16F VRAD pages, uploaded once per document;
- dedicated baked-irradiance material binding and Source-unit scale, without
  forcing it onto unlit/blended materials;
- meshlets and default-enabled BLAS, with an explicit raster-only option;
- skybox Background draw flags and no skybox physics;
- triangle colliders, centered box alternatives and box fallback for degenerate
  triangle streams;
- invisible-but-solid tool faces as an additional child of the first solid
  part's mesh collider, without changing its separate box-collider choice.

`PrefabPartInfo::additionalMeshCollision` is the narrow fixed-layout descriptor
addition for that final case. Core resolves the copied collision ID and builds
its own compound shape. A missing base shape or uncached extra collision is
rejected before prefab registration. No native physics shape type, registry
or renderer escapes. The reflected descriptor layout changes; rebuild both sides.
No Core/COM protocol or capability vtable changes are involved.

Each upload validates streams before GPU allocation. On later upload/material/
registration failure, its guard retires unpublished meshes and textures.
Collision cache entries are copied engine data, owned until ClearCache; they
are not rolled back if a later prefab-registration step fails. No success is
reported for that failed prefab.

## Limitations

Embedded pak files support stored ZIP entries, not compressed entries.
BLAS is enabled by default; select `buildBLAS = false` for raster-only uploads.
The lavapipe BLAS crash remains unresolved. Device-loss recovery requires
explicit host orchestration.
