# Fidelity Harness — glTF Render-Fidelity Integration

`samples/FidelityHarness.cpp` renders a Khronos
[glTF-Render-Fidelity-Generator](https://github.com/KhronosGroup/glTF-Render-Fidelity-Generator)
scenario headlessly through Zahlen and captures a still, so the engine can be
scored against the suite's reference goldens (Blender Cycles, Filament, Dassault
STELLAR, `<model-viewer>`, Babylon).

```
./build/samples/FidelityHarness --headless \
    --scenario build/fidelity_output/khronos-AlphaBlendModeTest.json \
    --output   build/fidelity_output/khronos-AlphaBlendModeTest.pam
```

The whole suite is driven by `scripts/run_fidelity.sh`, which clones the two
Khronos repositories, resolves every scenario through `tools/fidelity list`
(the generator's partial-override + default merge is reproduced exactly),
renders via this harness, and compares against the goldens through
`tools/fidelity compare` with the generator's own pixelmatch YIQ
`rmsDistanceRatio` (dB) metric. The driver emits a Ninja graph, so scenario
re-renders are parallel (`-j`) and mtime-cached (a scenario whose model, JSON
and harness binary are unchanged is skipped). See that file's header comment for
usage; repos default to `build/fidelity/`.

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
| `--output <file.pam>` | Capture path (required). `.pam` keeps alpha so omit-background pixels are skipped; `.ppm` stays P6 and forces alpha opaque. |
| `--ambient-scale <f>` | IBL ambient scale; default `1.0` (conformance 1:1). Applied at shade time, not baked. |
| `--headless` | Run without a window (core flag) |

Exit codes: `0` captured; `1` usage/scenario/capture error.

## Known divergences from the Khronos contract (i.e. the work left)

1. **HDR environment lighting is the scenario's `.hdr`.** `EnvironmentMapComponent`
   names the asset; the engine decodes it (raw Radiance or cooked `ZRD1`) and
   `RenderSystem` passes the floats to `SetEnvironmentRadiance`. The bake
   samples the equirect for SH and the specular prefilter. `ambientExposure`
   is applied at shade time, not in the bake. An unauthored fallback sun is
   suppressed while that component is set, so the panorama is the only light.
2. **Specular LOD.** The reflection pass samples `roughness * 5.0` of the 6
   mips (`mipCount - 1`). That is the live shader, not `roughness * 5/6`.
3. **Background.** `renderSkybox` false (the generator default, and the
   harness default) writes alpha 0 and the suite captures PAM so the metric
   skips those pixels. `renderSkybox` true samples cube mip 0, without the
   procedural `lightDir` rotation. The procedural gradient remains the
   background when no environment component is set.
4. **Extensions.** This harness has no control over importer support for
   `KHR_materials_*` (sheen, transmission, volume, iridescence, anisotropy,
   specular) that some scenarios exercise; those scenarios will diff by feature
   support, not by BRDF error.

`run_fidelity.sh` keeps going on any of these (`ninja -k0`) and reports the dB
delta, so a scene rendering as "correct shape, wrong light" is visible
immediately rather than hiding behind a failure.
