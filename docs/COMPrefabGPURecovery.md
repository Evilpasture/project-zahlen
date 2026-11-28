# Host-driven prefab GPU replacement and glTF recovery

`IAssetLibrary::ReplacePrefabGPUResources` accepts the fixed-layout
`PrefabGPUReplacementInfo` chain node and returns a new revision. This is a
GPU-only commit, not a new prefab registration or general asset hot reload.

- The cached ModelPrefab object, node/skeleton/animation vectors, transforms,
  bounds and collision objects stay in place. Live CPU animation bindings are
  not invalidated by replacing the cache entry.
- One `PrefabPartGPUInfo` per original part, in original order, with the same
  unique mesh/material keys and morph target count. New parts may share the
  same uploaded primitive.

> [!WARNING]
> Do not borrow buffers or material slots from old parts; successful replacement retires those resources.
- The supplied device generation must match the current engine. The expected
  revision must match the cached prefab. Revisions minted through COM are
  monotonic within that engine, including across cache clears; a stale owning
  lease cannot replace a later registration of the same path.
- All descriptor validation precedes publication/retirement. On validation
  failure, ownership remains with the caller and old aliases remain usable.
  On success, Core retires the old mesh/material resources, updates aliases,
  and takes ownership of the new resources. Call only between frames on the
  host/engine thread; this is not a cross-thread transaction API.
- Core walks its own entity storage to rebind live morph offsets/counts and
  invalidate skinning scratch buffers. Animated morph weights, animator
  handles/track/time/speed, skeleton indices and palette offsets are retained.
  No Registry or mapped column escapes to the consumer.
- Joint palettes are not copied back from the old GPU: the normal animation
  step, or explicit `IGraphicsDevice::UpdateJointMatrices`, republishes CPU pose
  matrices before the next frame. This API does not install an animation
  subsystem or snapshot GPU pose data.

Default material aliases are restored by replacement. A host that applies
material overrides must reapply them afterward and remains responsible for
resources it owns outside the prefab's default material set.

## glTF owner workflow

Link `zahlen_gltf_assets` and include `<glTF/GLTFAssets.hpp>`. Retain the CPU
Document and the owning Asset from the successful initial Upload:

```cpp
// engine is a host-held COM::Ref<COM::IEngine>.
auto document = GLTF::Read(path, options, engine->Assets());
auto asset    = GLTF::Upload(*engine->Assets(), *engine->Graphics(), *document);
// Spawn, retain entity IDs, run normal host loop.

// In the host loop, before the next animation/render step:
if (asset->deviceGeneration != engine->DeviceGeneration()) {
    auto rebuilt = GLTF::RebuildGPUResources(*engine.Get(), *document, *asset);
    if (!rebuilt) { /* stop rendering this asset and handle/retry the error */
    }
}
```

Check errors in production code; the example omits initial error handling.
`RebuildGPUResources` can also be called explicitly for a same-device refresh.
Use the retained original CPU document, not a reimport whose nodes, skeletons,
colliders or animation meaning changed. No source I/O, cache clear, new prefab
registration, or callback subscription is needed for this rebuild.

Only the initial uploader receives `Asset::ownsResources=true`. Cache-hit
Assets own no texture/replacement lease and cannot drive recovery. Keep one
logical owner. A copied stale owner is rejected after a successful replacement
by the revision check; copying is not a way to acquire another lease.

The helper uploads fresh anonymous RGBA8 textures, meshes, materials and morph
storage, then commits the GPU-only descriptor. Empty-name RGBA8 creation now
uses the renderer's anonymous upload path (including its mip-generation
option), rather than a single shared hash of the empty string. Thus a failed
replacement cannot unload/overwrite another asset's named texture or the
currently published image set.

On success the owning Asset is updated in place. Same-device old textures are
unloaded only after commit. After device recreation, old texture IDs are
renderer-scoped and can be numerically reused. Discard old IDs without unloading
them through the new renderer.

> [!WARNING]
> Do not unload an old-generation manifest through a new-generation device; reused IDs may refer to current-generation resources.

Unpublished GPU mesh and texture uploads roll back on error. Failed uploads
preserve the owner and cached prefab revision. Successful replacement retires old
default material slots and mesh buffers. Recovery leaves collision geometry
unchanged.

> [!WARNING]
> Failed attempts can consume material and morph arena space until renderer teardown.

At final shutdown: despawn users, clear the cache, then unload the current
Asset's texture manifest using its owning device generation.

> [!WARNING]
> ClearCache invalidates live animator bindings.

## Controlled renderer reset

`IEngine::RecreateGraphicsDevice()` exposes synchronous renderer recreation
for controlled resets. Invoke it between frames. Success advances
`DeviceGeneration` and clears prefab GPU readiness. The host republishes
assets/UI before the next frame. Engine-detected device loss uses the same path.
Host code polls generations without registering callbacks.

> [!WARNING]
> Failed renderer recreation leaves the device unusable without a host recovery policy.

The state/alias checks ran on VM lavapipe with live entities, renderer
teardown/recreation and `buildBLAS=false`, without a draw submission. The
`initial_skin_morph_frame` and `recovered_skin_morph_frame` tests remain enabled.

> [!WARNING]
> Repeated frame submissions intermittently SIGSEGV on this VM, including the initial-frame diagnostic with the original glTF Upload implementation and no replacement/reset. Recovered-frame rendering remains unverified; the cause remains open.

> [!WARNING]
> NVIDIA hardware device-loss and animation-playback conformance remain untested.

Native compatibility entry points remain available.

> [!WARNING]
> Do not use a native mutable loader and a COM owning lease for the same virtual path; both can mutate ownership of its resources.
