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

## Environment wiring and remaining fidelity approximations

1. **Environment lighting comes from the scenario asset.** `EnvironmentMapComponent`
   names it; the engine decodes raw Radiance `.hdr`, cooked `ZRD1`, or an LDR
   JPEG equirect (linearizing its sRGB bytes before the bake). In particular,
   `khronos-MetalRoughSpheres-LDR` uses `spruit_sunrise_1k_LDR.jpg`, not the
   HDR version. The harness preflights the decoder before its first frame and
   reuses the cached map. `RenderSystem` passes the floats to
   `SetEnvironmentRadiance` for SH and specular prefiltering. `ambientExposure`
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
   for procedural sky; mip 0 samples the environment directly. The radiance
   panorama has a full FP32 mip chain; rough specular rays read a source LOD
   derived from their GGX reflection-direction PDF and the equirectangular
   texel's latitude-dependent solid angle. Smooth reflections and diffuse SH
   still read source mip 0; procedural sky is unchanged. The area-light LTC
   and split-sum BRDF LUT remain isotropic approximations.
   The baseline diffuse SH/GI is gated by `(1 - F) * (1 - metallic)`, so metal
   no longer receives diffuse IBL on top of its specular term. Sheen now
   imports its linear color factor, sRGB color texture and independent
   alpha-channel roughness, layers a Charlie direct lobe over the base and
   under clearcoat, and attenuates the base by an approximate energy term.
   Sheen IBL samples the existing GGX-filtered cube, not a Charlie-prefiltered
   environment/LUT. `KHR_texture_transform` applies offset, rotation, scale
   and texture-specific UV-set selection (TEXCOORD_0/1) to each textureInfo,
   including the separately sampled glTF occlusion texture. Higher UV sets
   are not stored. KHR_materials_specular remains unsupported.
5. **Texture addressing.** The importer carries each glTF texture reference's
   independent `wrapS`/`wrapT` (repeat, clamp-to-edge, mirrored-repeat) through
   its material to one of nine preallocated GPU samplers. Images remain shared
   even if their texture objects specify different samplers, but color and
   data references to the same source image use distinct sRGB/linear uploads.
   glTF's `minFilter`/`magFilter` are not imported yet: the material sampler
   bank uses trilinear minification and generated mipmaps (rather than mip 0)
   to stabilize tiled fabrics.

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

For `SheenCloth`, the blue/black weave should repeat across the cloth instead
of stretching into broad blue bands: its per-texture transform includes
`scale: [30, -30]`. The importer regression checks that factor, the UV-set
override, mirrored/rotated transforms, and the shared image's separate sRGB
and linear handles. It does **not** compare a rendered frame to the golden;
sheen's GGX-based environment approximation can still differ in brightness.

For HDR IBL speckles, rebuild `FidelityHarness` and run both
`SCENARIO=khronos-MetalRoughSpheres-HDR ./scripts/run_fidelity.sh -j1` and
`SCENARIO=khronos-IridescentDishWithOlives ./scripts/run_fidelity.sh -j1`.
Inspect the PAM captures against their goldens: the higher-roughness spheres
and the glass/olives should no longer show isolated bright dots, while the
smooth metal reflections retain their sharp environment detail. The independent
`tests/extras/test_ibl_importance_sampling_math.py` checks numerical properties
of the GGX PDF and equirectangular texel solid angle; it does **not** inspect
shader source, execute the shader, or replace these Vulkan captures.

For `TransmissionRoughnessTest`, run
`SCENARIO=khronos-TransmissionRoughnessTest ./scripts/run_fidelity.sh -j1`.
Compare against its golden: higher roughness columns should increasingly blur
the opaque scene; higher IOR rows should reflect more strongly, while the IOR 1
row remains clear regardless of roughness. `zshader` compiles and reflects the
forward shader, but it cannot verify the runtime mip views or the appearance:
a new GPU capture and golden comparison are still required.

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

For the Sponza chain/foliage fidelity regression, inspect that same capture
against **Filament's** golden at matched scene features, not screen coordinates
from differently zoomed viewers. The source asset's chain (material 20) and
foliage (materials 0 and 3) are double-sided alpha-masked surfaces with normal
maps; they do not use sheen or texture-transform extensions. Check both hanging
chains for the source texture's dark rusty color instead of chalk-white
reflections, and check leaves for isolated white pixels. The deferred pass must
orient the whole normal-map frame on backfaces, and the BRDF LUT's split-sum A
term must multiply F0 (the LUT already integrated angular Fresnel). `zshader`
compiles and reflects both paths; only the image comparison can test whether
the illumination and the masked geometry actually look right.

`run_fidelity.sh` keeps going on any of these (`ninja -k0`) and reports the dB
delta, so a scene rendering as "correct shape, wrong light" is visible
immediately rather than hiding behind a failure.
