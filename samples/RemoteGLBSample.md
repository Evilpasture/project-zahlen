# Remote glTF Sample — a crawled list of models, lazily fetched

At start-up `RemoteGLBSample` does two things in parallel, each on its own
worker while the backdrop and the HUD come up:

1. it fetches the asset the default points at — Khronos' Damaged Helmet from
   [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets),
   the model every other glTF viewer measures itself against;
2. it crawls that repository's `Models/` tree into the HUD's model dropdown.

Picking a row in the dropdown fetches that one file — nothing is downloaded
until it is picked — caches it on disk, imports it through `extras/glTF`, and
renders it on a studio turntable. A second run (or a second pick of the same
row) reads it from the cache.

It is the reference for:

* `extras/HTTP` (`zahlen_http`) — `ZHLN::HTTP::Fetch(Request)` returning
  `std::expected<Response, ErrorCode>`, redirect following, `User-Agent` /
  `Accept` headers, timeout.
* `extras/glTF` (`zahlen_gltf`) — `LoadGLBPrefabFromMemory` from a byte span,
  `InstantiatePrefab`, `InstallDeviceLostHandler` for device-lost rebuild.
* `extras/json` (`zahlen_serialization`) — `ZHLN.ReflectJSON::Document` for
  the crawl's tree listing.
* `FS::Paths::CacheDir()` — build/cache in a dev tree, per-user cache
  otherwise, `ZHLN_CACHE_DIR` overrides both. The same location the pipeline
  cache uses.
* a production orbit camera (orbit / pan / zoom) and a hand-tuned studio look
  that survives quality-tier switches.

## The model list (the crawl)

The crawl is **one** call to the GitHub git trees API,
`GET https://api.github.com/repos/<repo>/<branch>/git/trees/<branch>?recursive=1`
— the unauthenticated API budget is 60 calls/hour, and the crawl is exactly
one call — with `User-Agent` and `Accept: application/vnd.github+json`, plus
`Authorization: Bearer $GITHUB_TOKEN` when the variable is set. The response
is the recursive tree of one branch: a `tree` array of `{path, type, size, …}`
and a `truncated` flag. The frame filters it: `type == "blob"`, path under
`Models/`, name ending in `.glb` — in the default repository that is 121
files, from 121 model folders.

Each survivor becomes one dropdown row:

* **label** — the model folder name, with the size in brackets
  (`DamagedHelmet (3.6 MB)`). Where a folder ships more than one `.glb`
  (ABeautifulGame's Draco/KTX sibling, CarConcept's second cut), the path
  under `Models/` with only the `.glb` dropped is what separates the rows —
  subfolder names alone could collide across models.
* **url** — `https://raw.githubusercontent.com/<repo>/<branch>/<path>`. The
  bytes come from the raw host, which is not API-metered, and the path is the
  tree's own spelling, so no percent-encoding is needed.

The crawl is a listing, not a download: nothing in the list is fetched until
its row is picked. The `truncated` flag (GitHub splits very large listings)
would leave the list short — it is reported in the HUD rather than silently
ignored.

The crawl runs on its own `std::jthread`, in parallel with the first asset's
download. Its state is handed to the frame with one release/acquire pair on
`std::atomic<CatalogPhase>`; when the list arrives the frame moves it into a
display copy it owns, so the crawl worker and `DrawHUD` never share a vector.
`C` re-crawls (ignored while one is running). A failed crawl (offline, 404
repo, not JSON) does not block anything: the dropdown keeps the rows it has,
the status line says why, and the current asset stays on screen. A 2xx that
is not JSON is an interstitial — GitHub's rate-limit / anti-abuse HTML page,
or a middlebox in front of the API — and the failure line carries the
response's content type and the head of the body, so the page can be
recognised in the log instead of guessed at (a token, `GITHUB_TOKEN`, or a
different network is usually the fix).

## What it fetches

The default is
`https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/DamagedHelmet/glTF-Binary/DamagedHelmet.glb`.
The URL shown in a browser is GitHub's HTML page about the file (`/blob/`);
the file itself lives at `/raw/` and at `raw.githubusercontent.com`.
`FetchCandidates` rewrites the pasted `/blob/` URL into both spellings, in
order:

1. the URL as given,
2. `.../raw/...` on `github.com`,
3. `https://raw.githubusercontent.com/...`.

`ZHLN::HTTP` follows the 302 that the second spelling returns. The default
asset is ~3.7 MiB; the list's largest file is ~66 MiB — all well under
`kMaxBodyBytes` (256 MiB).

Validation is a GLB container check: magic `glTF`, version 2, total length
equals file length. A cached file that fails it is deleted and re-fetched;
a network body that fails it is treated as the next candidate's problem
(e.g. an HTML error page). A pick whose import fails (the Draco/KTX variant
of ABeautifulGame needs a decoder this importer does not ship) keeps the
previous subject on screen instead of an empty turntable.

## Cache

```
<CacheDir>/http/<sanitized-stem>-<hash8>.glb
```

* `CacheDir()` — `ZHLN::FS::Paths::CacheDir()`.
* stem — last path segment without extension, sanitized to `[A-Za-z0-9_-]`,
  truncated to 48 chars.
* hash — lower 32 bits of `ZHLN::Hash64(url)` as 8 hex digits, so two URLs
  ending in the same name cannot collide.
* atomic write — `*.tmp` sibling + `rename`, the same pattern
  `PipelineCache.cpp` uses. A crash leaves no half-written hit.

## A download

Cache first, network second: the cache read stays on the calling thread
(local disk, a few MiB, first frame on screen). Only the transfer goes to a
worker, because that is the part that can take a minute.

A download is a `FetchJob` — a `std::thread` (not a jthread: a superseded
download has to finish in the background, and a jthread's destructor joins),
a `stop_source`, and a `finished` flag — owned by a heap
`unique_ptr`, because the job vector may reallocate under the worker's feet.
Picking another model while one is in flight does not wait: the old job's
stop is requested and its generation is retired, so when it finishes it keeps
the cache file it earned (the next pick of that model is a hit) but publishes
nothing. The generation check and the retarget both happen under one mutex,
so a superseded worker's result cannot land between the retarget and the new
transfer. The frame reaps finished jobs each frame — a join of a thread that
is already done takes microseconds; blocking joins happen only at shutdown.
`std::atomic<FetchPhase>` is the lock-free fast path the frame polls:
terminal stores are releases, the frame's load is an acquire.

Because two downloads can be in flight at once, the cache file of a
superseded transfer may appear on disk while a newer one is still going; that
file belongs to the pick it served, and nothing reads it until that model is
picked again.

## The subject

Import is `GLTF::LoadGLBPrefabFromMemory(ctx, assetMgr, bytes, virtualPath)`
followed by `PrefabFactory::InstantiatePrefab`. The virtual path is the
cache file's filename, so a second run and a device-lost rebuild hit the same
prefab-cache entry.

Bounds are not `localMin/localMax` unioned in place. Each part's node chain
is accumulated (`NodeModelTransform`) and applied first — for the Damaged
Helmet that chain is a -90° turn about X that stands the model up — then the
oriented box's world AABB is `center ± Σ|axis_i * halfExtent_i|`.

HUD reports: parts, nodes, triangles (`indexCount/3` or `vertexCount/3`),
distinct `TextureHandle`s (deduped, because several parts may share one GPU
image), centre, radius, instance count.

## The studio

Key plus two fills plus a floor, laid out around the subject's bounds:

* sun — `LightType::Sun`, 60, `(1.0, 0.97, 0.92)`, direction
  `(0.45, 1.0, 0.30).Normalized()`. No position falloff. Engine now respects
  explicit `direction` for Sun (was overwritten by transform).
* fill — `Point`, cool `(0.55, 0.72, 1.0)` from front-left,
  intensity `100 * r²` (so 25 at `r=0.5 m`), radius `r*0.15`, range `r*6`.
* rim — `Point`, warm `(1.0, 0.72, 0.45)` from behind-right,
  `160 * r²` (40 at `r=0.5 m`), radius `r*0.15`, range `r*6`.
* floor — `CreatePlane(r * kFloorExtent, white, SpawnParams{pos = (cx, minY,
  cz), materialOverride})`, `kFloorExtent 5.0` — wide enough that oblique
  angles catch the floor in reflections instead of void. `CreatePlane`
  hardcodes 0.35/0.15 unless a material arrives, so the finish (0.45
  roughness, `(0.12,0.13,0.15)`) comes via override. Colour lives in the
  material only: `basic.slang` multiplies vertex colour by base-colour
  factor, so two tints would square. Extent was 8*r which made helmet look
  tiny; 3.5*r tightened it; 5.0*r widened it again to catch reflections at
  oblique angles.

`maxPunctualShadows = 0` — neither fill casts, no cube map rendered.

## Render settings

`MakeStudioSettings(radius, rayTraced)` builds `GraphicsSettings` from the
canonical model (`GraphicsSettings.hpp`). Signature fields (AA mode/feedback,
shadow resolution, GI samples, SSR/RTR, RT budget) come from a preset
(`ApplyPreset(Ultra)` then RTR off), so tier and hand-tuned look differ only
there.

Final values (the ones that matter):

* grade — `blit.slang`: `hdr * exposure` → ACES → filter/contrast/saturation.
  `exposure 0.08`, sun 60 for that exposure, `bloomStrength 0.15` (bright pass
  thresholds at 1.0 HDR), `glowIntensity 0.22` for emissive decals, `contrast
  1.03`, `saturation 1.05`, `vignette 0.35/1.20` (was 0.60/1.60 which was almost
  black on RTX 3050 corners).
* ambient — `ambientExposure 4.0` scales baked SH fill *and* sky gradient.
  Sky is a softbox: zenith `(0.35,0.42,0.55)`, horizon `(0.90,0.92,0.96)`,
  ground `(0.06,0.06,0.07)`. SH cube is baked once at renderer init from the
  engine's default sky, so metal's environment comes from what it can see:
  SSR reads the lit buffer, and this gradient is what fills it (and what a
  ray-traced reflection paints on miss).
* AO — GTAO half-res, depth-weighted upsampled (`giMode 3`), radius
  `clamp(r*0.5, 0.02, 4)` so search stays inside features, bias 0.02, power
  1.5.
* shadows — 4096, `sunSize 0.035` (PCSS penumbra), width
  `clamp(r*16, 4, 64)` initially, then `UpdateShadowExtent` keeps
  `ShadowBoxExtent(d, maxZoom, r) = clamp(2*(d + r*5), 4,
  max(400, 2*(maxZoom + r*5)))` because `CullingSystem` culls casters with an
  ortho box centred on camera. The ceiling is the formula's own value at the
  far end of the zoom range — flat 400 used to clip the box as soon as the
  subject's radius exceeded ~3.1 m.
* camera — `fov 45`, `nearZ clamp(d*0.02, 0.01, 0.5)`, `farZ
  FrameFarPlane(d, maxZoom, r) = clamp(d*12 + r*24, 20, max(2000,
  maxZoom*12 + r*24))`. Narrow depth range → texel density. The ceiling is
  again the formula at max zoom — flat 2000 clipped the floor's far edge (and
  eventually the subject) at far zoom for any radius over ~2.7 m; Fox is the
  asset that hit it (authored in centimetres, so its 87.775 m radius is really
  0.88 m of geometry, but the turntable frames whatever scale it is given).
* RTR off by default — with `enableRTR`, `lighting.slang` takes sun shadow
  from a 1-spp RT ray below 80 m and only blends back to cascades beyond.
  A turntable subject always lives inside 80 m, so that trades analytic PCSS
  for a noisy ray behind A-Trous. SSR stays on.

Write-back: `PostProcess/Shadow/RayTracing` on
`GlobalSettingsTagComponent` singleton, AA on `MainCameraTagComponent`
singleton. `Registry::Add` replaces, so whole components are built from the
model. AA is the exception: `CameraSystem` advances `jitterX/Y`,
`prevJitterX/Y`, `frameIndex` every frame (TAA history), so tier changes use
`Patch` for knobs only, with fallback `Add` if absent.

Fidelity governor (`GraphicsSettingsSync.cpp`) steps quality down when present
margin <2 ms for 90 frames, never up, never touches `Custom`. Studio look
reports as `Custom`, so governor stays off.

## Camera

Orbit/pan/zoom, turntable style:

* `OrbitDirection(yaw, pitch)` — same convention as `Camera::GetViewMatrix`.
* right = `forward.Cross(Y)`, up = `right.Cross(forward)` (Jolt `Cross` is a
  member, not free).
* LMB/MMB drag — orbit, `kOrbitSpeed 0.30`.
* RMB drag — pan in view plane, grab-the-world (subject follows cursor),
  `scale = distance * 0.0015`.
* wheel — exponential zoom, `wanted *= exp(-wheel*0.18)`.
* pan/zoom settle exponentially `1-exp(-dt*14)` (~0.1 s). Orbit not smoothed —
  smoothing would put subject behind cursor.
* framing — `distance = r / tan(fov/2) * 1.35`, `min 0.15r/0.02`,
  `max 60r/100`.

HUD strip (`kPanelWidth 460 + margin 16`) owns input: drag starting on it
does not orbit, same as glTF inspector's explorer.

## Controls

* model dropdown (HUD) — pick a row: fetch (lazy) → cache → import → turntable
* LMB drag — orbit
* MMB drag — orbit
* RMB drag — pan
* wheel — zoom
* F — re-frame (`r/tan(fov/2)*1.35`)
* G — floor toggle (flips `DrawFlags::Hidden` via `Patch<MeshComponent>`)
* R — re-download the current model, ignoring cache
* C — re-crawl the model list
* 0 — studio look (`Custom`)
* 1–4 — Low/Medium/High/Ultra (same write-back, signature fields differ)

The orbit gate keys off x only (the panel is a left strip), so the dropdown
field is drawn to finish before the panel's right edge — a drag that starts on
the field (or on the list floating under it, which shares its x) does not
orbit the subject.

## Environment

All optional, read once at startup except timeout:

* `ZHLN_REMOTE_GLB_URL` — URL to fetch first (default blob URL above); if it
  is not a row in the crawled list, the dropdown gains a synthetic row for it
* `ZHLN_REMOTE_GLB_REFRESH=1` — bypass cache on first fetch
* `ZHLN_REMOTE_GLB_RTR=1` — enable RTR (reflections + shadows)
* `ZHLN_REMOTE_GLB_TIMEOUT` — seconds, default 60; crawl and downloads share it
* `ZHLN_REMOTE_GLB_FRAMES` — auto-exit after N frames (for headless tests)
* `ZHLN_REMOTE_GLB_NO_CATALOG=1` — skip the model-list crawl (dropdown keeps
  the single row for the URL)
* `ZHLN_REMOTE_GLB_REPO` — repository to crawl, `owner/name` (default
  `KhronosGroup/glTF-Sample-Assets`); rows must be `.glb` under `Models/`
* `ZHLN_REMOTE_GLB_BRANCH` — branch to crawl (default `main`)
* `GITHUB_TOKEN` — sent as a bearer on the crawl call (5000 calls/hour
  instead of 60)
* `ZHLN_CACHE_DIR` — overrides `CacheDir()` (engine-wide)
* `ZHLN_NO_AUTO_QUALITY=1` — opts out of fidelity governor (engine-wide)

## Building

```bash
cmake -B build -DZHLN_BUILD_HTTP=ON
cmake --build build --target RemoteGLBSample
./build/samples/RemoteGLBSample
```

Needs `libcurl` with dev headers (`libcurl4-openssl-dev` / `libcurl-devel` /
`curl` / Homebrew `curl` / vcpkg `curl`). If not found, `zahlen_http` target
does not exist and the sample is skipped with a CMake status message
(`Skipping sample RemoteGLBSample: extras target(s) not built: ...`). The
rest of extras is unaffected. Pass `-DZHLN_BUILD_HTTP=OFF` to silence the
search. The crawl's JSON parsing comes from `zahlen_serialization`
(`extras/json`), which is part of extras and is always built with them — it
is named in `ZHLN_SAMPLE_EXTRAS_RemoteGLBSample` next to `zahlen_gltf` and
`zahlen_http` so the skip logic stays target-based.

Headless smoke:

```bash
./build/samples/RemoteGLBSample --headless
ZHLN_REMOTE_GLB_FRAMES=120 ./build/samples/RemoteGLBSample --headless
```

## Device lost

`GLTF::InstallDeviceLostHandler` subscribes `RebuildCachedPrefabs` (re-imports
every cached prefab, re-registers meshes/materials). Sample's own callback
rebuilds studio + settings around existing bounds, or settings alone if
import has not finished yet.

## Engine fixes discovered by this sample

* `src/render/RenderSetup.cpp` + `RenderInternal.hpp` — `clusterBoundsDirty`
  was only set on aspect/FOV change. Camera `nearZ`/`farZ` also affect
  `invProj` which `cluster_bounds.slang` uses to unproject NDC, so changing
  near/far (sample does `near=clamp(d*0.02)`, `far=clamp(d*12+r*24)`) left
  cluster bounds stale → point lights culled incorrectly → dark helmet,
  blown floor spots. Now dirty on near/far change and stores `lastNearZ`/`lastFarZ`.

* `resources/shaders/uniforms.slang` — added `nearZ`/`farZ` to `FrameUniforms`.
  `cluster_math.slang` previously used specialization constants `Near=0.1 Far=1000`
  for slice distribution. That mismatched actual camera planes (e.g. far 103),
  so `ViewDepthToClusterZ` mapped view depth 5m to slice 10 vs correct 13.
  Added `*With(near,far)` variants and made `cluster_bounds.slang`,
  `lighting.slang`, `volumetric_*`, `reflection.slang` use `frame.nearZ`/`farZ`.

* `src/engine/system/LightingSystem.cpp` — Sun/Directional lights overwrote
  explicit `light.direction` with `-worldMat.GetColumn3(2)`. `GetSunDirectionAndIntensity`
  already prioritized explicit direction for shadows, causing shadow direction
  ≠ lighting direction. Now respects explicit direction if length >1e-4.

## Files

* `samples/RemoteGLBSample.cpp` — the sample (~2000 lines)
* `samples/CMakeLists.txt` — `ZHLN_SAMPLE_EXTRAS_RemoteGLBSample zahlen_gltf
  zahlen_http zahlen_serialization`
* cache — `<CacheDir>/http/DamagedHelmet-<hash>.glb` and one file per picked
  model, same scheme
