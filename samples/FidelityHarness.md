# Fidelity Harness — glTF Render-Fidelity Integration

`samples/FidelityHarness.cpp` renders a Khronos
[glTF-Render-Fidelity-Generator](https://github.com/KhronosGroup/glTF-Render-Fidelity-Generator)
scenario headlessly through Zahlen and captures a still, so the engine can be
scored against the suite's reference goldens (Blender Cycles, Filament, Dassault
STELLAR, `<model-viewer>`, Babylon).

```
./build/samples/FidelityHarness --headless \
    --scenario build/fidelity_output/khronos-AlphaBlendModeTest.json \
    --output   build/fidelity_output/khronos-AlphaBlendModeTest.ppm
```

The whole suite is driven by `scripts/run_fidelity.py`, which clones the two
Khronos repositories, resolves every scenario (the generator's partial-override
+ default merge is reproduced exactly), runs the harness, converts the PPM
(dependency-free), and compares against the goldens with the generator's own
pixelmatch YIQ `rmsDistanceRatio` (dB) metric, failing above −22 dB. See that
file's docstring for usage; repos default to `build/fidelity/`.

## What the harness does — and does not — do

RemoteGLBSample "looked good" because of hardcoded studio cheats (0.08 exposure,
ACES grading, `ambientExposure = 4`, an extra sun with two punctual fills and a
0.03-roughness mirror floor). None of those exist in a conformance render, and
this harness builds none of them. Specifically:

* **No lights.** `InitializeDefaultScene` spawns no `LightComponent`, and the
  harness never calls `BuildStudio`. There is nothing to turn off.
* **No floor.** No `CreatePlane`, nothing to bounce light.
* **1:1 exposure and PBR-neutral tonemapping** (`post.tonemapper = 3` in
  `blit.slang`), `bloomStrength = 0`, `vignetteIntensity = 0`, `contrast = 1`,
  `saturation = 1`, identity colour filter.
* **No AA** (`AAMode::None`) — a still must not carry TAA history or jitter.
* **No SSR/RTR reflections and no shadows** — the only illumination is the IBL.
* **`giMode = 0`** removes the engine's screen-space AO/GI gather, leaving the
  baked SH diffuse irradiance plus the pre-filtered specular environment, which
  is what the split-sum model the contract exercises.
* **Camera from the scenario**: Khronos `{theta, phi, radius}` around
  `target` (phi measured from **+Y**; theta azimuth about **+Y**), `verticalFov`,
  near 0.01 / far 100. The harness renders at `DEVICE_PIXEL_RATIO = 2` of
  `SetResolution(width, height)` with `fov = verticalFov` (both axes scale,
  so the composition matches), matching the goldens' native 2x capture.

## CLIs

| Argument | Meaning |
| --- | --- |
| `--scenario <file.json>` | Scenario JSON (required) |
| `--output <file.ppm>` | P6 PPM capture path (required) |
| `--ambient-scale <f>` | IBL ambient scale; default `1.0` (conformance 1:1). An escape hatch while HDR→IBL rebake is outstanding. |
| `--headless` | Run without a window (core flag) |

Exit codes: `0` captured; `1` usage/scenario/capture error.

## Known divergences from the Khronos contract (i.e. the work left)

1. **HDR environment lighting is not read.** The engine bakes SH diffuse,
   pre-filtered specular cubemap and the BRDF LUT once at init,
   *from its built-in procedural sky* (`IblShCS`/`IblSpecularCS` in
   `ibl_bake.slang`, `IBLProcessor::Bake` in `src/render/IBLProcessor.hpp`).
   A conformance render instead illuminates from the scenario's `.hdr`
   equirectangular panorama. Until a radiance texture can drive the bake
   (a `Canvas::TextureCube` sampling the panorama in those compute shaders),
   `--ambient-scale` is the honest stand-in.
2. **Split-sum order-of-operation.** The IBL plumbing exists (BRDF LUT +
   pre-filtered cube + SH), but the processes are run with
   `ambientExposure` baked into their scale and the reflection pass samples
   `roughness * 5` of 6 mips; the standard split-sum `LD * DFG` product and
   `1/(9 · #mips)` mip mapping need an audit pass.
3. **Background.** The scene clear colour is a dark grey (`kClearColorScene`),
   not the scenario's neutral-grey-or-transparent background. A flat background
   pass sampling the (future) environment irradiance is needed.
4. **Extensions.** This harness has no control over importer support for
   `KHR_materials_*` (sheen, transmission, volume, iridescence, anisotropy,
   specular) that some scenarios exercise; those scenarios will diff by feature
   support, not by BRDF error.

`run_fidelity.py` keeps going on any of these and reports the dB delta, so a
scene rendering as "correct shape, wrong light" is visible immediately rather
than hiding behind a failure.
