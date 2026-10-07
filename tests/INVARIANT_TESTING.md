# Invariant render testing: no absolute pixels, no committed GLBs, quiet logs

Status: adopted 2026-10-07. This is the follow-through on `GOLDEN_IMAGES.md`
Tier 0 ("make the differential tests the default shape") plus the two
companion fixes: uncommitted glTF coverage and quiet test output.

## 1. The rule

A render test must not fail when the renderer gets *better*. Concretely:

- **No absolute pixel counts or 8-bit floors as correctness gates.**
  `goldPixels > 100`, `meanDelta < -0.10`, `saturated < 6144` encode today's
  look, not today's correctness. Every exposure, tonemapping, IBL, or
  material change moves them, so fidelity work fails the suite it is
  supposed to be carried by.
- **Presence floors are fine.** `share > 0.01`, `lit > 0.5%`, `parts > 0`
  pin *existence*, not look. They survive any grade change that leaves the
  subject on screen.
- **Sanity bounds are fine, and must read as sanity bounds.** `meanLuma > 1`
  ("not blacked out"), `darkPct < 80` ("not blacked out the other way").
  If a bound needs tuning after a look change, it was a look encoding --
  convert it instead of tuning it.

`GOLDEN_IMAGES.md` bucket B is the list of tests that still break this rule.
Bucket A (differential/A-B) and the two ray-traced noise suites are already
the model: hardware-portable, no stored state, both sides move together.

Tier 2 of that document (a golden store) is deliberately **not** adopted
here. Goldens trade threshold friction for regeneration friction, and need
the before/after/diff review habit to be worth it. Revisit only if a test
cannot be expressed as an invariant at all.

## 2. The three replacement shapes

### A/B in the same process

Capture twice, change one thing, compare pooled. Both sides share the
device, exposure, and tonemap, so the comparison survives all three:

```cpp
const auto off = Capture(*engine, "thing_off.ppm");
SetThing(*engine, true);
TickFrames(*engine, 4);
const auto on = Capture(*engine, "thing_on.ppm");
const auto diff = Image::CompareFrames(off, on);
// pooled fraction, not a count: a one-pixel TAA edge cannot fail this
ZHLN::Test::ExpectGt(diff.frac32, 0.001);
```

For comparisons that must ignore sub-pixel jitter, downsample first with
`Image::DownsampleBox` (e.g. to 320x240) and compare the pooled outputs.

### Noise-relative

Measure the run's own floor, gate the effect against it. `TestAmbientOcclusion`
already did this for `darkPct` (`3x` the repeat-window noise); the helpers
in `helpers/ImageTesting.hpp` generalize it:

- `Image::MeasureNoiseFloor(frames)` -- pairwise `meanAbs` mean/stddev and
  worst `frac32` over consecutive captures of an unchanged scene.
- `Image::DescribeSeries(values)` -- mean/stddev/min/max of any metric series.
- `Image::ExceedsNoise(effect, noiseStd, k, floor)` -- `effect > k*noiseStd
  && effect > floor`. The k-sigma term scales with the run's own jitter;
  the floor keeps a bit-exact-zero noise run from passing on dust.

### Share/ratio hue

Hue questions must be asked relative to the window's own brightness:

- `Image::DominantHueShare(img, rect, channel)` -- share of pixels where one
  channel dominates, floored at a fraction of the window's own max luma.
- `Image::YellowShare(img, rect)` (new) -- the sibling for hues no single
  channel dominates (gold, sodium vapour): R and G both above `ratio` x B.
- `Image::MeasureSubRegion` channel means for cross-region ordering
  ("left is warmer than right"), never against a constant.

## 3. Worked exemplars in this patch

### `TestPBR`: dielectric vs metallic (was: `goldPixels > 100`)

Old: whole-frame counts above fixed floors (`r > 60 && g > 40 && b < 40`).
Any tonemap change zeroes both counters -- which is exactly the observed
failure (`0` vs `> 100` on both).

New (`pbr_dielectric_vs_metallic_surface_response`): two windows framing the
gold (left) and red (right) boxes; each half must (a) show its hue above a
presence floor (`YellowShare > 0.01`, red share `> 0.01`) and (b) beat the
*other* half at its own hue (`goldWarm > 1.3x redWarm`, `redRed > 1.5x
goldRed`). Shared sky cancels in the cross-half comparison; exposure moves
both sides together. Patch 2 re-leveled the scene underneath it (sun
220 -> 40, exposure 12 -> 2): the artifact PNG showed the gate was truthful
but the old levels overexposed red to white -- a stale scene, not a stale
gate (verdict log, row 8).

### `TestAmbientOcclusion`: mode signatures (was: `meanDelta < -0.10`)

Old: every AO mode had to darken the frame-wide mean past an absolute line.
GTAO mode 3 measured `meanDelta = +0.05` while still darkening contacts
(`minDelta = -17.9`, `darkPct = 0.41`) -- a small global gain from elsewhere
in the pipeline that the mean-sign gate misread as "no occlusion".

New: net darkening share, `darkPct - brightPct > max(0.02, 3x noise)`, plus
the existing depth (`minDelta < -3`), blackout (`darkPct < 80`), and
structure (`stdDelta > max(0.1, 2x noise)`) gates. A dead mode scores ~0, a
brightening bug scores negative, a working mode scores positive however the
global gain drifts. The previously failing modes (sample AO at `-0.06`,
both GTAOs at `+0.05`) pass on structure; the checks that already were
noise-relative are untouched.

## 4. Quiet logs

The runner now captures engine logs per test instead of streaming them:

- Every test runs at `Verbose` with output suppressed into a bounded buffer
  (`ZHLN::BeginLogCapture`, `src/engine/Log.cpp`). A passing test prints
  only its `[ RUN ]`/`[ PASS ]` lines; a failing test prints its usual
  failure details plus the captured engine log (head 15 + tail 50 lines,
  omission counted).
- `ZHLN_TEST_LOG=verbose` disables the capture and streams engine logs,
  for debugging.
- Panics bypass suppression, so a fatal always prints.
- The per-frame `[Test Capture]` statistics in `RenderResources.cpp` moved
  from Info to Debug: they are kept in the failure dump (tests capture at
  Verbose) but no longer drown even a verbose passing run. The
  no-completed-frame path was promoted to Warning, since it is an error.

Test `Println` lines (`[INFO]`, `[PASS]`, `[SKIP]`) are unaffected -- they
are test output, not engine chatter, and still print.

## 5. glTF coverage without committed GLBs

Two mechanisms, both already proven in the tree; this patch wires the
second one into ctest.

### Procedural in-memory GLBs (fast, precise)

`TestGLTFImport.cpp` never commits a GLB: `MakeGlb()` + `SerializeJSON()`
synthesize conformant documents (emissive-strength, anisotropy + sampler
wraps, sheen/UV-transform, transmission, unlit, negative-scale) and check
the importer against an independent cgltf parse. `TestTextureTransforms`
does the same for UV at render time (procedural atlas + panels, no asset).
Prefer this for importer and feature questions: the fixture is the spec
case, reviewed as code.

### Fetched Khronos models (broad, uncommitted)

`TestGLTFSampleAssets.cpp` renders real `glTF-Sample-Assets` models that are
never committed:

```sh
cmake -DZHLN_FETCH_SAMPLE_ASSETS=ON ...   # shallow clone into build/sample-assets/
# or: git clone --depth 1 https://github.com/KhronosGroup/glTF-Sample-Assets.git <dir>
#     ZHLN_SAMPLE_ASSETS=<dir> ctest -R GPU_Pipeline
```

Resolution order is `$ZHLN_SAMPLE_ASSETS`, then the configured default;
when neither exists the tests SKIP (same convention as the LFS-pointer
skip in `TestGLTFImport`). The seed test imports under the fidelity
options (`emissiveFactorScale = 1`, 2048px -- the `FidelityHarness` units),
auto-frames from prefab bounds, and asserts only invariants: non-empty
prefab, entities spawned, non-degenerate frame, and model visibility as an
empty-vs-model A/B (`frac32 > 0.001`).

Adding a scenario test: extend the candidate list, keep the fidelity
import + auto-frame + A/B visibility preamble, then assert the feature as
an invariant (monotonic roughness response, UV-column agreement à la
`TestTextureTransforms`, normal-map orientation). The full Khronos
still-vs-golden diff stays where it belongs: `scripts/run_fidelity.sh`,
out of ctest.

## 6. Verdict log: the 16 GPU_Lighting failures (triaged 2026-10-08)

Each row names the mechanism the evidence supports and what Patch 2 did.
Score: 13 stale gates or bounds (converted), 2 stale scenes (PBR re-leveled,
halo given its bloom master), 1 real bug (quarantined). Both patch-2
self-resolving rows decided as designed: the sweep table confirmed
occlusion geometry, the halo run exposed the composite gate.
The pattern underneath: the light transport is healthy -- SSR,
RTR, shadows, AO, clustered tinting all prove themselves in differentials
hiding inside these "failures" -- while the suite failed on absolutes,
overexposed scenes, and one real switch coupling.

| test | verdict | mechanism | patch-2 action |
|---|---|---|---|
| `lit_scene_static_frame_stability` | stale | 6/7 stability gates pass on bit-identical frames; only the 2% blowout cap fails on a 42%-saturated bright scene | cap becomes a 90% white-out sanity bound |
| `point_light_cluster_culling_sweep` | stale bound, geometry confirmed | per-step table shows one contiguous zero-run (steps 6-14, ends 500+px): occlusion, not a pop; the patch-2 bound miscounted (12 present vs >= 14) | presence >= 10/21 + exactly one zero-run + hot ends (patch 3); a second run or dead end still means quarantine |
| `multi_light_cluster_accumulation` | stale | TL misses 1.3x by 4.5% while 24.6% red by count; center is tonemap-clipped white | cross-quadrant chroma ordering + center-brightest-luma + R/G balance |
| `raytraced_shadow_occlusion` | stale | 95-luma signal with `dark = 0` both sides (floor too low); the "flicker" is designed 1 SPP dither | mean-delta occlusion + dither-an-order-below-signal + settled-means |
| `raytraced_reflection_coverage` | stale | scene defeats itself: sun 140 washes the 1x emitter below red threshold even in DIRECT view (and it runs SSR-only despite the name) | SSR on/off A/B in a tight mirror crop, any hue; artifact shape stays with the noise suite |
| `multi_emissive_reflection` | stale | strips ~100x the blob area make mean ratios unachievable; 112 red px prove SSR works; blue strip passes vacuously on sky | per-strip SSR A/B + cross-strip dominant-count ordering; blue by A/B only |
| `dense_multi_light_emissive` | stale | palette authors ZERO green-dominant lights (computed: 1 red, 11 blue, 0 green); 19% blowout is by construction | palette-derived primary gates (green ungated) + 50% sanity cap |
| `pbr_dielectric_vs_metallic` | stale scene, then stale metric | re-level worked (gold warm 0.76, red leads +43/+40) but strict dominant-red share still reads 0.000 at that brightness | re-leveled (sun 40, exposure 2), then red side converted to cross-half R-G excess ordering (patch 3): gold F0 crushes blue, so R-G is the only honest axis |
| `rtr_metallic_f0` | stale | probe is 97% sky-mirror; relatives pass (gold << chrome blueness) while the 2x absolute was never producible | blob-peak-relative presence + kept F0 orderings |
| `rtr_roughness_monotone` | stale | red 24x emitter; spread-then-dilute (1814 -> 10522 -> 0 red px) is correct lobe physics; peak falls 228 -> 227 -> 103 | peak monotonicity + collapse; mean gates dropped |
| `rtr_chrome_preserves_hue` | stale | +10/+10 cross-side differentials prove hue transfer on a shared B~136 pedestal | margined splitHue + shared-floor; absolute dominance dropped |
| `rtr_live_pbr_patch` | stale | retint proven (dL = 70); the patch-2 yellow ratio had the direction backwards -- 53x is dielectric OVER metal (yellow emitter, dielectric 89% yellow-mix) | direction-free chroma distance |dB/R| + |dG/R| > 0.30, measured 0.77 (patch 3) |
| `rtr_metal_palette` | stale | ordering au < cu < sil holds; absolute silver neutrality contradicts mirror-of-blue-sky | kept gold links + copper-silver direction link; neutrality dropped |
| `rtr_is_live_stay_still` | REAL -> quarantine | jitter bbox starts at y=102 yet on/off shifts 3.4% above it: a systematic (non-dithering) leak, likely bloom/exposure coupling | instrumented (on/on-top + top meanAbs) + quarantined; CONFIRMED systematic (on/on-top exactly 0.000, meanAbs 0.12 vs 0.00 -- sub-luma post coupling, zero jitter); bloom-off run localizes it |
| `emission_survives_no_lights` | stale | 100x through default Khronos Neutral desaturates BY DESIGN; the control reads 109 (scene not perfectly unlit) | hue at sibling-proven 6x + brightness at 100x + emissive-vs-control differential |
| `neon_glow_halo` | stale scene (missing master switch) | patch-2 explicit on/off STILL bit-identical -- but the feed is fine: blit.slang multiplies the bloom chain by `bloomStrength = 0` (default), so the composite ate the feed | in-scene `bloomStrength = 0.01` on both frames (master on, feed flipped; patch 3). Test was right, scene missed a switch -- no quarantine |
| `all_ao_modes...` | stale | mean-sign gate vs global gain (patch 1, Section 3) | fixed in patch 1 |

The ray-traced noise suites (5/5, 4/5) remain the shape to copy: analytic,
self-referential, no constants from another machine.

## 7. Migration checklist for the next conversion

1. State the question in one sentence ("the shadow appears and is stable").
2. Pick the cheapest Section 2 shape that asks it (A/B first).
3. Add a presence floor, never a look count.
4. `[INFO]`-log the measured numbers; gate on the invariant.
5. Verify fail-to-pass: break the feature, watch the test fail, revert.

## 8. Quarantine workflow

Quarantine is for diagnosed renderer bugs, never for stale gates and never
for red tests of unknown cause. A stale gate converts (Sections 2-3); an
ambiguous failure gets instrumented until its own output decides
(self-resolving: sweep presence pattern, halo explicit on/off). Only what
is left -- a failure the evidence pins on engine behavior -- takes an entry.

How: add `{suite, test, reason + evidence pointer}` to `QuarantineList()`
in `tests/TestsFramework.hpp`, using the exact names `[ RUN ]` prints. The
test keeps running; a failure prints `[ QUARANTINE ]` with the reason,
counts aside, stays out of the global failed list, and does not touch the
exit code -- but it keeps its full diagnosis (fatal, recorded expectations,
engine log), because a quarantined test is still a debugging instrument. A
quarantined test that passes prints a remove-the-entry nudge: entries must
die when the bug does.

`ZHLN_TEST_QUARANTINE=off` disables every entry (strict mode for tracking a
bug to zero). To retire an entry: fix the engine, watch the test pass, run
once strict to confirm it holds unquarantined, delete the entry.

Current entries: `rtr_is_live_and_rough_surfaces_stay_still` (verdict 14:
CONFIRMED systematic -- the on/on-top control reads exactly 0 with top
meanAbs 0.12 vs 0.00, i.e. a sub-luma post coupling with zero jitter; a
bloom-off run localizes it to bloom vs exposure). The halo (verdict 16)
was resolved WITHOUT an entry: the patch-2 comment's quarantine trigger
missed the composite gate, and the shader read proved the feed fine --
quarantine stays for diagnosed engine bugs only.

## 9. Systemic lesson: Neutral plus hot scenes

The default tonemapper is Khronos PBR Neutral (`tonemapper = 3`), which
fades bright highlights toward white -- that is its specified look, not a
bug. Most GPU_Lighting scenes were authored hotter than Neutral's shoulder
(suns 100-240, ambient exposures 6-12, emissions 20-100x), so reds wash to
white, greens wash to white, and gates written for a linear response fail
on correct rendering. Six of the sixteen failures were this one story.

The rule this implies: scene levels are load-bearing. When a hue gate
fails, check the PNG and the direct view FIRST -- if the source does not
read the hue directly, no reflection, probe, or ratio gate downstream can
pass, and the fix is re-leveling the scene (then proving direct reads in
the log), never tuning the gate to the clip. Proven-sane level combos:
culling sweep (sun 40, exposure 2.0), shadow floor (sun 240, exposure 1.5
on a distant floor), unlit-scene hue (6x emission reads chromatic),
multi-emissive direct (6x under sun 100).

And four shipped defaults no test may assume away: `glowIntensity = 0.0`
(glow is opt-in -- an "on" case must pass its own value), `tonemapper = 3`
(Neutral desaturates past the shoulder -- assert hue below it, brightness
at scale), "unlit" scenes still get default sun/IBL (the emissive control
reads ~109 -- compare against the control, don't floor it), and
`bloomStrength = 0.0` (the composite multiplies the whole bloom chain by
it -- a glow test must run the master switch in-scene, or it compares off
against off).
