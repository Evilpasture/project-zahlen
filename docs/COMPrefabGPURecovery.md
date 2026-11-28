# Host-driven prefab GPU replacement and glTF recovery

`IAssetLibrary::ReplacePrefabGPUResources` accepts the fixed-layout
`PrefabGPUReplacementInfo` chain node and returns a new revision. This is a
GPU-only commit, not a new prefab registration or general asset hot reload.

- The cached ModelPrefab object, node/skeleton/animation vectors, transforms,
  bounds and collision objects stay in place. Live CPU animation bindings are
  not invalidated by replacing the cache entry.
- One `PrefabPartGPUInfo` per original part, in original order, with the same
  unique mesh/material keys and morph target count. New parts may share the
  same uploaded primitive. They must not borrow buffers/material slots from
  any of the old parts: old resources are retired on success.
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
renderer-scoped and can be numerically reused. Discard old texture IDs
without unloading them through the new renderer. Never independently unload an
old-generation manifest on a new-generation device.

Unpublished GPU mesh/texture uploads roll back on error; the owner and cached
prefab revision do not change. Material instances and morph data still use
engine arenas without a general individual-free API: failed attempts may
consume arena space until renderer teardown. This is not an atomic transaction
across all renderer arenas. Successful replacement retires old default material
slots as well as mesh buffers. Recovery does not rebuild collision geometry.

At final shutdown: despawn users, clear the cache, then unload the current
Asset's texture manifest using its owning device generation. ClearCache is not
a recovery operation and must not run underneath live animator bindings.

## Controlled renderer reset

`IEngine::RecreateGraphicsDevice()` exposes the existing synchronous renderer
recreation path for explicit controlled resets. Invoke between frames; on
success `DeviceGeneration` advances, prefab GPU readiness becomes false, and
host assets/UI can republish resources before the next frame. A failed renderer
recreation is a fatal/unusable-device condition unless the host has its own
well-defined recovery policy. Normal engine-detected device loss already uses
the same recreation path. Host code can poll generations without registering
callbacks.

The state/alias checks were exercised on VM lavapipe with live entities,
actual renderer teardown/recreation and `buildBLAS=false`, without a draw
submission. Separate `initial_skin_morph_frame` and
`recovered_skin_morph_frame` integration tests remain enabled in the suite.
Repeated frame submissions intermittently SIGSEGV on this VM, including an
initial-frame diagnostic using the original upstream glTF Upload implementation
and no replacement/reset. Recovered-frame rendering remains unverified, and the
precise underlying cause remains open. No injected NVIDIA hardware-device-loss
or animation-playback conformance is claimed. Native compatibility entry points remain available; do not mix a
native mutable loader and a COM owning lease for the same virtual path.
