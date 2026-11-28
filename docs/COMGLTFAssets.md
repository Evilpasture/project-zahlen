# glTF CPU documents and capability authoring

Link `zahlen_gltf_assets`, include `<glTF/GLTFAssets.hpp>`. The archive does
not contain the native inspector module, `GLTFImporter.cpp`, or native
Engine/RenderContext/AssetManager calls. `zahlen_gltf` remains the native
compatibility/inspector archive and now uses the same CPU decoder.

```cpp
auto document = ZHLN::GLTF::Read(path, options, engine->Assets());
if (!document) return document.error();
auto asset = ZHLN::GLTF::Upload(*engine->Assets(), *engine->Graphics(),
                              *document, {.buildBLAS = true});
if (!asset) return asset.error();
auto entities = engine->Assets()->Instantiate(asset->hash);
```

`engine` above is a host-held `COM::Ref<COM::IEngine>`. Instantiation uses
Core's capability boundary; the document and orchestration remain host data.
This example omits application-specific error/texture lifetime handling.

## Data and source ownership

`Decode(bytes, virtualPath, bytesPath, options, assets)` consumes borrowed
bytes synchronously. The result owns node hierarchy, geometry, material
policy, image RGBA bytes, skeletons, animation tracks, morph deltas and CSG
metadata. It retains no cgltf pointers or input-buffer borrows. Texture
bindings use typed host-side `TextureID MaterialDesc::*` members plus CPU image
indices, not fabricated GPU handles. No member pointer crosses COM.

The source reader tries the asset VFS before disk. Relative external buffers
and images resolve against `bytesPath` (the source `.gltf` filename), not the
virtual prefab cache key. `Read` also retains the historical
`resources/assets/` fallback. Embedded GLB images and base64 image data URIs
are decoded to owned pixels. Missing/undecodable images retain the native
best-effort fallback policy; malformed containers/buffer structure fail.
Unsupported extensions remain advisory, including required extensions.

Material mappings, sampler/UV transforms, emissive scaling, texture resizing
and lights match the native loader. Morph weights use node-over-mesh precedence;
only the first four position morph targets are used.

Linear easing is supported. Cubic animation support is incomplete. Cubic weight
data uses compact value-only storage; cubic TRS tracks store
tangent/value/tangent triples.

## Upload and ownership

Upload authors unique source primitives once, with separate node-instance
parts. Geometry, skin streams, materials, textures and morph storage go
through `IGraphicsDevice`; collision geometry and flat prefab metadata go
through `IAssetLibrary`. There is no native shape or registry escape.

After successful registration, the cache owns mesh buffers. `Asset::textures`
is an explicit caller-owned manifest: despawn all users, clear the cache, and
only then unload those textures. A cache hit owns no new texture resources.
Different import presentation options require different virtual paths;
conflicting options on a cache hit are rejected. Clear/reload must also wait
until users no longer hold prefab/animation state.

An unpublished upload destroys its newly acquired meshes/textures if a later
operation fails. Material instances and morph allocations use engine-owned
arenas without individual release APIs; their arena space is not reclaimed by
this rollback. Collision entries remain cache-owned until cache clear.
Registration is not an atomic transaction across those arenas/caches.

## GPU recovery

GPU replacement and glTF host recovery use
`IAssetLibrary::ReplacePrefabGPUResources` and `GLTF::RebuildGPUResources`.
Retain the owning Asset and original Document, poll DeviceGeneration in the
host loop, and rebuild before the next frame. The operation keeps CPU prefab
identity and live animator state; it rebinds morph offsets and skinning scratch
without clearing the cache or subscribing a callback. Palette matrices are
republished by the normal animation step/explicit graphics upload.

See [COMPrefabGPURecovery.md](COMPrefabGPURecovery.md) for generation/revision
validation, texture ownership, controlled resets and failure limitations.
RGBA8 glTF capability textures use independent anonymous uploads.

Do not mix native recovery entry points with a COM owning lease for the same
prefab path.
