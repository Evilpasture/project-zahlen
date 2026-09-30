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

* **No lights.** `InitializeDefaultScene` spawns no `LightComponent`. Prefab
  virtual point lights are disabled (they are opt-in anyway); the harness
  defensively destroys any that exist. The unauthored 180-intensity fill is
  dropped while the environment map is set.
* **Authored emissive units.** The import uses `emissiveFactorScale = 1`: an
  emissive factor is its glTF linear value, multiplied by
  `KHR_materials_emissive_strength` when present and by the sRGB-decoded
  emissive texture when present. Other engine clients retain the 100x import
  boost needed for their low-exposure neon look. In `TextureEncodingTest` the
  factor-only green emitter is authored as `0.2462`, not `24.62`; boosting it
  was pushing neutral tonemapping into its pale-highlight desaturation. This
  is an import-unit fix, not an image-grade adjustment.
* **No floor.** No `CreatePlane`, nothing to bounce light.
* **Static glTF pose.** An animator is retained where needed for skinning and
  bind-pose evaluation, but no clip is selected: the generator's Filament
  renderer does not play animations for its stills. Authored node/mesh morph
  weights are preserved while the harness settles eight frames. Auto-playing
  the first clip made the captured pose depend on render/load time.
* **Authored texture detail through 2048 px.** The usual glTF runtime cap is
  1024 px, but the harness opts into 2048 px for stills. The importer retains
  that choice on the prefab for device-lost rebuilds and refuses to reuse the
  same cache path with a different resolution limit. Material samplers already
  use trilinear mipmapping and hardware anisotropic filtering; texture detail
  cannot be recovered from an image downsampled before GPU upload.
* **1:1 exposure and PBR-neutral tonemapping** (`post.tonemapper = 3` in
  `blit.slang`), `bloomStrength = 0`, `vignetteIntensity = 0`, `contrast = 1`,
  `saturation = 1`, identity colour filter. The blit writes linear color; the
  headless target is `R8G8B8A8_SRGB`, so the store encodes sRGB the way a
  swapchain does. A `_UNORM` target was writing the linear bytes into the PAM.
* **Spatial SMAA** (`AAMode::SMAA`) smooths rasterized edges in the still,
  including coverage at scene/background boundaries. It has no TAA camera jitter
  or history. Khronos goldens are antialiased; disabling every AA pass left
  obvious one-pixel stair steps even at the goldens' native 2x resolution.
  Use `--no-aa` to capture the unfiltered baseline for A/B comparison. Spatial
  AA cannot recover geometry smaller than a rendered pixel like coverage AA can.
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
| `--no-aa` | Disable the default spatial SMAA for an unfiltered comparison. |
| `--headless` | Run without a window (core flag) |

Exit codes: `0` captured; `1` usage/scenario/capture error.

## Environment wiring and remaining fidelity approximations

1. **Environment lighting comes from the scenario asset.** The harness uses
   the optional `AssetCooking` decoder to read raw Radiance `.hdr`, cooked
   `ZRD1`, or an LDR JPEG equirect (linearizing its sRGB bytes before the bake).
   In particular, `khronos-MetalRoughSpheres-LDR` uses
   `spruit_sunrise_1k_LDR.jpg`, not the HDR version. The harness preflights the
   source and registers owned linear pixels under the scenario's lighting key.
   `EnvironmentMapComponent` names that key; core never decodes the source.
   `RenderSystem` looks up the prepared pixels and passes them to
   `SetEnvironmentRadiance` for SH and specular prefiltering. A missing key is
   a render error, not a request to read a file. `ambientExposure` is applied
   at shade time, not in the bake. An unauthored fallback sun is suppressed
   while that component is set, so the panorama is the only light.
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
   dielectric F0; without iridescence, dielectric regions at IOR 1 have
   neither refraction blur nor Fresnel reflection. The linear R channel of
   `transmissionTexture` multiplies the factor; glTF `alphaMode` remains an
   independent coverage control (MASK
   discards holes, BLEND fades the composite). Metallic regions absorb
   transmitted light. The composite writes depth, so only the nearest surface
   shows (the sample viewer's single layer). It does not apply volume
   attenuation or render a second glass layer.
   `KHR_materials_iridescence` samples the factor and thickness textures.
   `KHR_materials_clearcoat` is a second dielectric GGX lobe (F0 0.04) with
   its own normal, in direct light and image-based lighting. The base is
   attenuated by one `(1 - Fc)`.
   `KHR_materials_unlit` uses base-color factor × sRGB-decoded base-color texture
   × vertex color, without emission, direct/ambient light, AO, PBR or reflections.
   Its MASK fragments still use alpha cutoff and depth, OPAQUE fragments stay
   in the G-buffer for the opaque scene copy, and BLEND fragments composite in
   forward. The UNORM8 G-buffer anisotropy-strength channel reserves code 255
   for unlit; lit strengths use codes 0–254 and decode to the full 0–1 range.
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
   are not stored. `KHR_materials_specular` and
   `KHR_materials_pbrSpecularGlossiness` remain unsupported. In the Khronos
   `SpecGlossVsMetalRough` scenario, the right bottle has no core PBR fallback:
   it stays gray/default, as in `<model-viewer>`. The importer prints a yellow
   warning for the *required* spec/gloss extension but still attempts to draw
   the rest of the asset. Files cgltf cannot parse or whose buffers fail to
   load still fail import.
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

For **`NormalTangentTest`**, run
`SCENARIO=khronos-NormalTangentTest ./scripts/run_fidelity.sh -j1` and compare
with the glTF Sample Viewer golden. The left cell of each pair is geometry;
the right is a normal-mapped surface with differently oriented UVs. The source
mesh supplies `NORMAL` and `TEXCOORD_0`, **not `TANGENT`**. The importer generates
tangents from triangle positions and UVs, with a bitangent along **-dP/dV**:
glTF's UV V increases down the image, but its OpenGL-style normal-map +Y points
up. A +dP/dV bitangent reproduces Khronos's "Incorrect flipped Y" example,
with concave or rotated right-hand spheres. The companion
`NormalTangentMirrorTest` supplies tangents whose handedness confirms the
-dP/dV convention. All five right-hand reflections should match the apparent
orientation of the real geometry on their left. Authored glTF tangents remain
authoritative; do not flip the green channel of every normal map globally.
The CPU regression covers the reconstructed normal-map direction as well as
UV rotations and mirroring; a fresh GPU capture is still needed to compare
with the golden.

For **`NegativeScaleTest`**, run
`SCENARIO=khronos-NegativeScaleTest ./scripts/run_fidelity.sh -j1`. All six
front/back/sample indicators should be green at scales +1 and -1, and the
corresponding white/dark-blue spheres should have consistent illumination.
The check/X material is authored single-sided: the importer must not turn it
double-sided just because a node is mirrored. A negative determinant reverses
the winding of each *draw*, including when inherited from a parent; the vertex
and meshlet raster paths restore CCW winding before culling and front-face
shading. The mirrored tangent-frame handedness also follows the world transform
without modifying authored or generated local tangents. The pinned glTF import
regression checks the real asset's material flags and mesh sharing; the
headless GPU regression checks culling and double-sided normals on both raster
paths. Neither is a pixel-perfect match to the Sample Viewer golden, so compare
a new capture as well.

**MultiUVTest's `<model-viewer>` golden is not a neutral brightness target.**
The official material samples base color from `TEXCOORD_0` and the emissive
logo from `TEXCOORD_1` (`emissiveFactor = [1, 1, 1]`). It specifies no
metallic/roughness factors or texture, so glTF defaults both factors to **1**:
the panel is a fully rough **metal**, lit predominantly by specular IBL rather
than diffuse light. The fidelity generator gives both renderers the same
`lightroom_14b.hdr`, but its `<model-viewer>` page explicitly selects Three.js
`tone-mapping="aces"`; the Zahlen harness uses PBR Neutral at exposure 1.
Three.js's ACES shader includes a subjective `exposure / 0.6` lift before
its filmic color transform. For illustration, a uniform linear gray value
of 0.25 maps to approximately sRGB 152 in that ACES shader versus 126 with
Zahlen's Neutral shader, even before IBL differences. Three.js also adds
indirect *specular multiscattering* for rough metals; Zahlen's current IBL
uses the single-scattering `F0 * BRDF.x + BRDF.y` term only. That missing
energy-compensation term can darken this panel but is distinct from a UV,
emissive-factor, or color-texture import error. A correctly positioned logo
supports the UV routing, not photometric agreement. Do not change the asset's
metallic factor or globally raise exposure to chase one viewer's golden; for
closer parity, compare a common tone map and inspect the HDR prefilter and
rough-metal specular energy across other scenarios before changing the PBR
model. Zahlen's simpler `ACESFilm` option is **not** the Three.js ACES shader.

For **`MaterialsVariantsShoe`**, run
`SCENARIO=khronos-MaterialsVariantsShoe ./scripts/run_fidelity.sh -j1` and
compare the blue fabric, laces and eyelets with `blender-cycles-golden.png`.
The shoe's base color, normal and packed occlusion/roughness/metallic images
are authored at 2048×2048; the old 1024px importer cap discarded three
quarters of their texels. The fidelity harness now retains all their source
pixels. This improves *available texture detail*, not Cycles' 128 adaptive
samples, denoising, self-shadowing or indirect light from nearby geometry.
Zahlen's IBL-only still has spatial SMAA and prefiltered environment lighting;
it cannot acquire path-traced contact shadows simply by brightening the render
or sharpening the final screenshot.

For **`MorphStressTest`**, run
`SCENARIO=khronos-MorphStressTest ./scripts/run_fidelity.sh -j1` and compare
with `filament-golden.png`: the yellow block's top should be level with the
untouched green block, not raised. This is the *base pose*, not a failure to
import the first morph target. The official mesh has eight targets, and its
first `Individuals` clip begins lifting target 0 (the yellow block) almost
immediately. The prefab factory normally starts clip 0; letting the harness
run it through the eight settle ticks produced a time-dependent raised yellow
block that Filament's still does not have. Fidelity captures now leave the
animator idle, preserving authored default weights. Zahlen currently imports
only the **first four** targets; that separate limitation affects animated
poses involving targets 4–7, not the **first** block's incorrect height here.

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

**ToyCar's smooth windshield is a viewer difference, not an asset correction.**
The `Glass` material in the generator's pinned GLB authors roughness 0,
`transmissionFactor: 1`, no volume thickness and no roughness map. Zahlen's
transmission shader keeps zero-thickness screen UVs, floors roughness at 0.001,
and bilinearly samples the full-resolution scene near mip 0. The generator's
`<model-viewer>` 4.0.0 golden uses Three.js r169, which floors effective
roughness at 0.0525 *plus geometric-normal derivatives* and bicubically
reconstructs the transmission target. The resulting blur around the car's
interior frames is renderer-specific filtering, not frosted glass in the glTF.
We deliberately leave Zahlen's sharp thin-glass filtering alone rather than
forcing an invented roughness or thickness just to match that golden.

For **`UnlitTest`**, run
`SCENARIO=khronos-UnlitTest ./scripts/run_fidelity.sh -j1` and compare with
`gltf-sample-viewer-golden.png`. The bevelled orange and blue solids must have
uniform color across their faces despite different normals or scene lighting;
only their antialiased silhouettes should blend into the background. The
attributed official GLB in `tests/render/assets/` drives both an importer test
and a GPU regression that measures front/top/side colors with the sun off/on.
The latter also tests masked and blended unlit base-color texture coverage.
Those property checks do not replace comparison of a full harness capture
with the sample viewer golden.

For **`TransmissionTest`**, run
`SCENARIO=khronos-TransmissionTest ./scripts/run_fidelity.sh -j1` and compare
with `gltf-sample-viewer-golden.png`. The red/blue columns use a striped
transmission texture (linear R); the green/blue columns also use alpha MASK
textures, leaving holes in otherwise transmissive spheres. These are separate
controls: a zero transmission texel leaves an *opaque surface*, while a failed
alpha mask leaves *no surface*. The bottom row's metallic map attenuates
transmission. The importer test in `tests/render/TestGLTFImport.cpp` checks the
factor/texture/coverage/UV/sampler pipeline, and `TestTransparentMaterials.cpp`
checks masked/textured transmission in a headless GPU frame. Those small tests
do not replace a capture of the complete scenario against the golden.

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

For `TextureEncodingTest`, run
`SCENARIO=khronos-TextureEncodingTest ./scripts/run_fidelity.sh -j1` and compare
against Filament's golden. The factor-only and texture-driven emissive spheres
should be green, not pale mint, in all texture-encoding columns. The importer
fixture checks emissive strength, sRGB texture registration, and both import
scales; a new GPU capture is still needed to compare the final image.

For the Sponza chain/foliage fidelity regression, inspect that same capture
against **Filament's** golden at matched scene features, not screen coordinates
from differently zoomed viewers. At 1000% zoom, the wall/arch silhouette should
no longer have the hard one-pixel staircase of the `--no-aa` capture. The
reference's antialiasing may still be smoother than spatial SMAA; compare the
same image resolution and crop before judging edge quality. The source asset's
chain (material 20) and foliage (materials 0 and 3) are double-sided alpha-masked
surfaces with normal maps; they do not use sheen or texture-transform
extensions. Check both hanging chains for the source texture's dark rusty color
instead of chalk-white reflections, and check leaves for isolated white pixels.
The deferred pass must
orient the whole normal-map frame on backfaces, and the BRDF LUT's split-sum A
term must multiply F0 (the LUT already integrated angular Fresnel). `zshader`
compiles and reflects both paths; only the image comparison can test whether
the illumination and the masked geometry actually look right.

`run_fidelity.sh` keeps going on any of these (`ninja -k0`) and reports the dB
delta, so a scene rendering as "correct shape, wrong light" is visible
immediately rather than hiding behind a failure.
