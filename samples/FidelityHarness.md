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

* **No lights.** `InitializeDefaultScene` spawns no `LightComponent`. The
  harness still destroys any that exist, including the point light a prefab
  spawn attaches to an emissive part, and the unauthored 180-intensity fill
  is dropped while the environment map is set.
* **No floor.** No `CreatePlane`, nothing to bounce light.
* **1:1 exposure and PBR-neutral tonemapping** (`post.tonemapper = 3` in
  `blit.slang`), `bloomStrength = 0`, `vignetteIntensity = 0`, `contrast = 1`,
  `saturation = 1`, identity colour filter. The blit writes linear color; the
  headless target is `R8G8B8A8_SRGB`, so the store encodes sRGB the way a
  swapchain does. A `_UNORM` target was writing the linear bytes into the PAM.
* **No AA** (`AAMode::None`) — a still must not carry TAA history or jitter.
* **No SSR/RTR reflections and no shadows** — the only illumination is the IBL.
* **`giMode = 0`** removes the engine's screen-space AO/GI gather, leaving the
  baked SH diffuse irradiance plus the pre-filtered specular environment, which
  is what the split-sum model the contract exercises.
* **Camera from the scenario**: Khronos `{theta, phi, radius}` around
  `target` (phi measured from **+Y**; theta azimuth about **+Y**), `verticalFoV`,
  near 0.01 / far 100. Radius **0 is valid**: upstream uses it for
  `khronos-Sponza` and `khronos-MetalRoughSpheresNoTextures` to put the eye at
  the target. The view still faces *opposite the orbit direction*, rather than
  normalizing the zero eye-to-target vector. Nonfinite cameras fail before
  capture. The harness renders at `DEVICE_PIXEL_RATIO = 2` of
  `SetResolution(width, height)` with the scenario FOV (both axes scale,
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
4. **Extensions.** `KHR_materials_transmission` samples a mipmapped copy of the
   lit opaque scene: roughness and IOR select its blur even for thin-walled
   (zero-thickness) surfaces, while `KHR_materials_volume` thickness also
   offsets the refraction UV. Reflection strength comes from IOR-dependent
   dielectric F0; without iridescence, IOR 1 has neither refraction blur nor
   Fresnel reflection. The composite writes depth, so only the nearest
   surface shows (the sample viewer's single layer). It does not apply volume
   attenuation, a transmission texture, or a second glass layer.
   `KHR_materials_iridescence` samples the factor and thickness textures.
   `KHR_materials_clearcoat` is a second dielectric GGX lobe (F0 0.04) with
   its own normal, in direct light and image-based lighting. The base is
   attenuated by one `(1 - Fc)`.
   `KHR_materials_anisotropy` now imports strength, rotation and the linear RG/B
   direction/strength texture. The deferred direct BRDF uses the extension's
   anisotropic GGX; the split-sum cubemap uses one lookup along a bent
   reflection, with LOD biased partway toward the wide-axis roughness (an
   approximation, not an anisotropically prefiltered integrator). The HDR
   prefilter uses 512 samples for mips 1–2 and 1024 for mips 3–5, versus 32
   for procedural sky; mip 0 samples the environment directly. The HDR source
   still has only mip 0, so very small bright lights can retain some aliasing.
   The area-light LTC and split-sum BRDF LUT remain isotropic approximations.
   The baseline diffuse SH/GI is gated by `(1 - F) * (1 - metallic)`, so metal
   no longer receives diffuse IBL on top of its specular term. Sheen and
   specular remain unsupported. Other differences (for example texture
   coordinate sets and texture transforms) still contribute to the metric.
5. **Texture addressing.** The importer carries each glTF texture reference's
   independent `wrapS`/`wrapT` (repeat, clamp-to-edge, mirrored-repeat) through
   its material to one of nine preallocated GPU samplers. Images remain shared
   even if their texture objects specify different samplers. glTF's
   `minFilter`/`magFilter` are not imported yet: the bank retains the renderer's
   existing linear, mip-0-only material filtering.

To check the reported mismatch visually, render the Khronos
`AnisotropyStrengthTest` scenario with this harness and the matching HDR map.
Compare its strength rows and roughness columns against the generator's golden:
the rows must no longer be identical, and increasing roughness should widen
rather than whiten the highlight. The importer regression in
`tests/render/TestGLTFImport.cpp` checks strength, rotation and the optional
texture path; it is not a substitute for this GPU image comparison. The IBL
uses an isotropic BRDF LUT and one bent-reflection cubemap lookup with an
anisotropy-aware LOD, so exact pixel agreement with a reference path tracer
is not expected.

For `TransmissionRoughnessTest`, run
`SCENARIO=khronos-TransmissionRoughnessTest ./scripts/run_fidelity.sh -j1`.
Compare against its golden: higher roughness columns should increasingly blur
the opaque scene; higher IOR rows should reflect more strongly, while the IOR 1
row remains clear regardless of roughness. The GPU-free wiring regression in
`tests/extras/test_fidelity_transmission_wiring.py` checks the resource, graph,
and shader contract but **does not** establish visual fidelity; a new GPU
capture and golden comparison are still required.

For `TextureSettingsTest`, compare the clamp S/T rows (solid green) and
mirror S/T rows (checkmarks) against the golden; repeat S/T should remain
checkmarks. This requires a new GPU capture—successful shader compilation and
an importer fixture alone cannot verify the image.

For the zero-radius camera regression, run
`SCENARIO=khronos-Sponza ./scripts/run_fidelity.sh -j1` and check that the
candidate PNG contains opaque scene pixels rather than an all-transparent
capture. The same check applies to `khronos-MetalRoughSpheresNoTextures`.
The upstream Blender reference parents an oriented camera to the target; it
does **not** calculate its direction by looking at a distinct target point.

`run_fidelity.sh` keeps going on any of these (`ninja -k0`) and reports the dB
delta, so a scene rendering as "correct shape, wrong light" is visible
immediately rather than hiding behind a failure.
