# Golden images: replacing static heuristics in the render suites

Status: proposal, written 2026-09-27. Nothing here is implemented yet.

The render suites have been carrying the fidelity work on heuristics — counts of
"gold-ish pixels", mean luma windows, dark-pixel percentages. They catch a
broken renderer, but they also fail a *better* one, because what they encode is
today's look rather than today's correctness. This document is the audit behind
that claim and a concrete design for replacing them with reference captures.

---

## 1. What the render suites assert today

27 files, ~75 tests (`tests/render/Test*.cpp`), each a member function returning
`std::expected<void, ErrorCode>` discovered by `ZHLN::Test::RunSuite`. They fall
into four buckets.

### A. Differential / A-B — keep as-is

Two captures taken **in the same process on the same device**, compared to each
other: AO on vs off, radius small vs large, mesh path vs vertex path, camera
moved vs still, frame N vs frame N+1.

- `Image::CompareFrames` users: `TestClusteredLighting.cpp`,
  `TestRayTracedShadows.cpp`, `TestRenderDistanceStability.cpp`
- temporal/noise suites: `TestRayTracedNoiseStability.cpp`,
  `TestRayTracedReflectionNoise.cpp`, `TestPresentPacing.cpp`,
  `TestRenderPerformance.cpp`

These are the healthiest tests in the tree. They are hardware-portable, they
need no stored state, and because both sides move together they are largely
immune to exposure or tonemapping drift. They are also already self-calibrating
in places — `TestAmbientOcclusion.cpp:416` gates `darkPct` against
`3.0 * noise.darkPct` measured in the same run rather than against a constant.

### B. Absolute pixel heuristics — the targets for replacement

A threshold tuned by looking at one frame on one machine.

| location | assertion | why it blocks improvement |
|---|---|---|
| `TestPBR.cpp:176-177` | `goldColoredPixels > 100`, `redColoredPixels > 100` | any material or exposure change moves the count |
| `TestPBR.cpp:373-378` | `smooth.maxL > 20 && rough.maxL > 20 && rough.warm > 50`, then `tighter \|\| hotter \|\| peaked` | an OR of three vague signals; the test cannot say what "correct" is |
| `TestPBR.cpp:464` | `pureGreenPixels > 500` | pure count, no reference |
| `TestAmbientOcclusion.cpp:413-418` | `meanDelta` in `(-80, -0.10)`, `darkPct < 80`, `stdDelta > 0.1` | makes GTAO stronger or subtler is a failure |
| `TestDescriptorHeaps.cpp:96,107,113` | `matched[probe] >= 60`, `distinctColors >= 40` | correctness question (does box *i* sample texture *i*) wearing a count costume |
| `MeasureSubRegion` / `MeasureImage` users | `TestEmissiveShading`, `TestReflections`, `TestTransparentMaterials`, `TestUI`, `TestClusteredLighting`, `TestRayTracedShadows` | the `45` level floor, `1.35` channel ratio and `1.6` hue ratio in `helpers/ImageTesting.hpp` are fixed constants applied to a moving picture |

`TestDescriptorHeaps` is the illustrative one. Its question is perfectly crisp
— 64 boxes, each with its own procedural texture, must each resolve through the
bindless array — and the encoding of that question is "at least 60 sampled
pixels per probe box matched a nearest-neighbour hue". When the current branch
rendered all 64 boxes with one texture, the test reported `matched=0`. It did
its job. A golden would have reported the same thing and *shown* 64 identical
boxes, which is the part a count cannot do.

### C. Re-derived engine math — deleted

`TestMeshShaders.cpp` carried
`cone_culling_sphere_formulation_no_false_positive`: both the old apex
formulation and the new sphere formulation re-implemented as lambdas *inside
the test*, with no engine code called. It was a frozen transcript of a bug fix,
so it could not fail when the engine regressed and could not pass if the
culling math were ever changed again on purpose. Its fifth case still carried
the author's uncertainty in the source: `// actually still not cull because
axis outward, camera behind still sees back? Let's make axis opposite.`

It is deleted, and so is the shader function it mirrored: `ConeBackfaceCulled`
in `resources/shaders/common.slang` had no caller — `basic_task.slang` uses
`ConeBackfaceCulledSphere` — and the CPU test was the last thing keeping the
apex formulation load-bearing.

The regression it was guarding now lives in `meshlet_closeup_no_black_hole`,
which was already the right shape and had stopped being the right *subject*:

- the box was spawned with half-extents of `1.0` while the camera sat `0.6`
  out, which put the camera **inside** the box. The frame was the sky seen
  through backfaces, the centre of the image was never black, and the test
  passed without a meshlet in view. Half-extents are now `0.5`, which is what
  the comment always claimed — front face at z = +0.5, camera 0.1 in front.
- a box can never be cone-culled anyway: every face is flat, so all normals in
  a meshlet are equal, `coneCutoff` is `1.0`, and `ConeBackfaceCulledSphere`
  returns false on its first line. The test now also renders a **sphere**, the
  subject whose meshlets have real normal cones.

Both subjects are rendered on the mesh path and the vertex path and compared,
so the assertion stays a silhouette mismatch rather than an absolute colour.

### D. Scene setup / API smoke — keep

Entity and component liveness, `GameplayStatus::OK` per tick, validation-layer
count deltas. No pixels involved.

---

## 2. Staleness findings while auditing

1. ~~Two divergent frame stacks.~~ **Fixed.** `helpers/ImageTesting.hpp` defined
   `ZHLN::Test::Image::RgbImage` + `LoadPPM`, and
   `tests/render/NoiseFrameCapture.hpp` defined a second
   `ZHLN::Test::Frame::RgbImage` + `LoadPPM` — the drift `ImageTesting.hpp` was
   written to end, left half-finished. `NoiseFrameCapture.hpp` is gone and its
   temporal-noise utilities (`LumaPlane`, `TemporalMoments`,
   `FitBernoulliNoise`, `BBox`, `Crop`, `RmsInRegion`,
   `RunningMeanResidualSeries`, `LumaDifference`, `BBoxOfChangedPixels`) now
   live in `Image::`. `tests/RayTracedNoiseMetrics.hpp` stays where it is: its
   `ZHLN::Test::Noise` metrics are a separate, CPU-tested library
   (`tests/TestRayTracedNoiseMetrics.cpp`), not a second copy of anything.
   There is one `RgbImage` in the tree now, so a golden harness has one loader
   to grow on.
2. **GPU suites are off in CI.** `tests/CMakeLists.txt:203` —
   `option(ZHLN_BUILD_GPU_TESTS … OFF)`. `.github/workflows/ci.yml` builds the
   `builder` stage of the Dockerfile and runs CTest; the render suites are not
   in that run. Everything in bucket B is therefore a **local-only** gate today:
   it fails on a developer's machine, never in a PR.
3. **The Docker image already pins a software device.** `Dockerfile:21-22`
   installs `vulkan-icd-loader` and `vulkan-swrast`, `:35` pins
   `VULKAN_SDK_VER=1.4.357.0`, and `:52-53` force
   `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` — Lavapipe.
   This is the single most important fact for this proposal: **the reference
   device a golden store needs already exists and is already pinned.** It is
   currently used only to keep the validation layers quiet; nothing renders
   against it.

---

## 3. Proposal

Three tiers, in the order they should be built. The point of the ordering is
that tier 0 costs almost nothing and tier 2 is where the real argument is.

### Tier 0 — make the differential tests the default shape (~free)

Nothing to build. When a new render test is written, prefer "capture A, change
one setting, capture B, compare" over "capture, assert a count". Extend this to
the tests in bucket B that can be expressed as an A/B pair — e.g. the
`TestPBR` roughness comparison at `:373` already *is* an A/B pair (smooth box vs
rough box); it only fails to be portable because the gate is `rough.warm > 50`
rather than "rough differs from smooth in the expected direction".

### Tier 1 — fold the second frame stack (done)

`NoiseFrameCapture.hpp` is deleted and its utilities now live in
`ZHLN::Test::Image` inside `helpers/ImageTesting.hpp`; the two ray-traced noise
suites had their `using` declarations repointed. No test logic changed, and the
`namespace_allowlist.json` entry that excused the deleted file's `detail`
namespace was removed — `check_namespace_governance.py` flagged it as an entry
matching nothing, which is the check doing its job.

### Tier 2 — the golden store

**What is stored.** For each converted test, a reference capture plus a sidecar:

```
tests/goldens/
  lvp-1.4.357.0/                    <- device fingerprint
    PBRTestSuite/
      metallic_response_distinguishes_gold_from_red.png
      metallic_response_distinguishes_gold_from_red.json
```

**Per-developer fingerprints, not one CI device.** The directory name is
derived from the device, never chosen by hand: `{vendorId}-{deviceId}-{driverVersion}`
on hardware, `lvp-{SDK version}` under the pinned Lavapipe ICD. Every machine
that runs the suites generates and compares against *its own* goldens, so a
regression is caught on the machine you are working on rather than three days
later in a CI job nobody watches. The cost is that goldens stop being shared
truth — a change that is correct on an RTX and wrong on Radeon is only caught
by whoever owns a Radeon — and the store grows by one directory per device.
That is the trade we picked.

A test that finds no golden for the current fingerprint **writes one and
reports "golden created for this device"**, it does not fail: first run on a
new machine seeds that machine's baseline, and every run after compares. A
*changed* golden is a failure, which is the whole point.

The `.json` sidecar records everything that has to match for a comparison to be
meaningful: image size, disable-TAA flag, warmup frame count, fixed `dt`, the
graphics-settings hash, and the source path of the test. Mismatched sidecar →
skip with a reason, never a red X.

**Storage: Git LFS.** Full-resolution PNGs under
`tests/goldens/<fingerprint>/`, tracked through LFS. This follows from the
per-developer decision: one device per developer is several directories of
images, and in-repo blobs would make every clone pay for every machine. The
cost is that a checkout needs the LFS tooling and CI needs `lfs: true` on its
checkout step, and a golden that cannot be fetched must report "golden
unavailable" and skip rather than fail.

**What "matching" means.** Not per-pixel equality. `Image::CompareFrames`
already computes what is needed — mean absolute channel error and the fraction
of pixels over 12 and over 32 — and the gate should be on those *pooled*
fractions (e.g. `frac32 < 0.02`), not on counts, so a one-pixel-wide TAA edge
or a denoiser jitter cannot fail a run. Downsampling the comparison to a fixed
size (say 320x240 for a 640x480 capture) buys a lot of tolerance for free and
makes the stored golden ~10x smaller. If pooled fractions turn out to need
per-test hand-holding, the upgrade is FLIP, not more thresholds.

**Determinism requirements per scene.** A golden is only reproducible if the
scene is. The harness must provide, and every converted scene must use:
- `ZHLN::Test::Headless::DisableTAA(engine)` — exists (`HeadlessEngineFixture.hpp:355`)
- a fixed warmup frame count through `TickFrames(engine, n, dt)` — exists (`:377`)
- no wall-clock-driven animation, and seeded RNG for anything stochastic
  (particles, RT sample offsets) — needs a per-scene audit, this is the real work

**Updating.** `--update-goldens` rewrites the store for the current fingerprint
and writes a report directory next to it: golden, current, and
`WriteAmplifiedDiff` output (already implemented, `ImageTesting.hpp`) per
changed test, so a reviewer looks at three images per change instead of
inferring from a number.

### The one rule that decides whether this is worth doing

Goldens become a *worse* form of lock-in than thresholds unless regenerating
one is a reviewed human act. If `--update-goldens` is a rubber stamp, the store
is just today's heuristics with extra steps and a bigger diff. So:

> A PR that changes a golden must carry the before/after/diff images in its
> description, and the golden change is reviewed as an image, not as a number.

If that review habit does not exist, tiers 0 and 1 are still worth doing and
tier 2 is not.

---

## 4. Open questions

1. ~~Where do the images live?~~ **Decided: Git LFS**, full resolution. (See
   tier 2.) A `.gitattributes` rule over `tests/goldens/**` still has to be
   written, and the CI checkout step needs `lfs: true`.
2. **Does Lavapipe actually run these scenes?** The engine wants mesh shading
   and ray tracing, and the render suites are named after them. Lavapipe
   supports both in recent Mesa, but slowly, and `ubuntu-latest` has a time
   budget. Someone has to try it before the reference job can be designed —
   possibly the reference job runs only the subset of suites that complete
   under Lavapipe, with the ray-traced ones staying local-only.
3. **Which tests convert first?** Suggested first cut, highest value per unit
   of work: `TestPBR` (3), `TestAmbientOcclusion` (2), `TestEmissiveShading`
   (2), `TestDecal` (3), `TestViewmodel` (3). Deferred: `TestUI` (text
   rasterization is the least portable thing in the engine — subpixel coverage
   differs per font rasterizer version), and the three ray-traced noise suites
   (already temporal, already self-referential, little to gain).
4. **Does this land on the current branch?** Tiers 0 and 1 and the
   `TestMeshShaders` cleanup have: they are small, and they are on the branch
   under review. Tier 2 wants its own PR — mixing a golden harness into the
   render-graph refactor makes both harder to read.
5. **Is `TemporalMoments` / the Bernoulli fit the right basis for a noise
   golden?** The two ray-traced noise suites already measure convergence
   analytically (ratio of measured to `p(1-p)d^2` variance, running-mean
   residual). Those are *better* than an image comparison where they apply,
   because they say why the frame is wrong and not just that it is. They are
   the model for what a golden should grow into where a golden is the only
   option.
