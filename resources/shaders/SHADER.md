# SHADER GUIDE — Slang-modernized

> **Migration note (2026-08-10):** All shaders have been modernized to idiomatic Slang syntax.
> The canonical sources are now `*.slang` (modules, `import`, explicit `[shader(...)]` stages).
> The legacy `*.hlsl` files are retained for DXC fallback and are patched for the silent bugs
> that DXC masks but `slangc` correctly diagnoses.

## Conventions (unchanged)

* **DO NOT REMOVE `#pragma pack_matrix(column_major)` in `.hlsl` legacy files.** DXC needs it.
  For `.slang` the equivalent is the compiler flag `-matrix-layout column_major` (see `cmake/ShaderCompilation.cmake`).
  Slang defaults to `row_major` memory layout, DXC to `column_major`; without the flag all `float4x4`
  uniforms/PushConstants are silently transposed – a bug the strict Slang frontend surfaces.

* **RIGHT HANDED COORDINATES, COLUMN MAJOR / VECTOR-COLUMN, CCW ONLY.**

## Build

* **Slang-first.** `cmake/ShaderCompilation.cmake` probes for `slangc` (via `VULKAN_SDK` or `PATH`).
  If `slangc` and `ZHLN_USE_SLANG=ON` (default) **and** a `*.slang` counterpart exists for a given
  `*.hlsl`, that `.slang` is compiled with:

  ```
  slangc <shader>.slang -target spirv -profile <vs_6_5|ps_6_5|cs_6_0>
         -entry <VSMain|PSMain|CSMain|...>
         -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
         -I resources/shaders -I include -o <gen>.spv
  ```

  Otherwise `dxc` is used:

  ```
  dxc -T <stage> -E <entry> -spirv -fspv-target-env=vulkan1.3 -I ... -Fo <gen>.spv
  ```

* To force DXC even when Slang is present: `cmake -DZHLN_USE_SLANG=OFF`.

* The `tools/slang_migrate.py` script is the source of truth for the `.hlsl` → `.slang` translation.
  It fixes the silent bugs DXC hides and generates `module`/`import`/`[shader(...)]` idioms.
  Re-run after editing HLSL: `python3 tools/slang_migrate.py`.

## Slang idioms used

* **Modules:** `pbr_helpers.hlsl` → `pbr_helpers.slang: module pbr_helpers;`
  `uniforms.hlsl` → `uniforms.slang: module uniforms;`
  `common.hlsl` → `common.slang: module common; import pbr_helpers; import uniforms;`
  Consumers do `import pbr_helpers; import uniforms;` or `import common;` instead of `#include`.
* **Split for `SKIP_BINDINGS`:** `culling.hlsl`/`punctual_shadows.hlsl` historically did
  `#define SKIP_BINDINGS` + `#include "common.hlsl"` to get types without bindings.
  Modern Slang splits `common` into `common` (with `[[vk::binding]]`) and
  `common_types.slang` (types/helpers only). Pure Slang code should `import common_types;`.
  For compatibility the old macro path is preserved (Slang tolerates it in HLSL-compat mode).
* **`[shader("vertex"|"fragment"|"compute")]`** annotates every entry point (`VSMain`, `PSMain`,
  `PSShadow`, `PSForward`, `CSMain`, `Smaa*`) – the Slang-idiomatic way to declare stage
  (instead of relying solely on `-T <profile>`).
* **`[[vk::binding]]` / `[[vk::push_constant]]` / `[[vk::constant_id]]`** are preserved
  verbatim for SPIR-V Vulkan 1.3 parity. No change needed – Slang understands the `vk` namespace.
* **`SMAA.hlsl`:** The library is wrapped as `SMAA.slang: module SMAA; __include "SMAA.hlsl"`.
  `smaa_wrap.slang` does `import SMAA;` instead of `#include "SMAA.hlsl"`. The DXC hack
  `#define SamplerState static SamplerState` (redefining a type keyword) is removed for Slang;
  Slang correctly rejects it – the modern fix is to make those samplers `static` inside the
  library or to use the wrap's explicit samplers (`linearSampler`, `pointSampler`) via
  `SMAA_SAMPLER_INTERPOLATION` etc.

## Silent bugs fixed (DXC-masked, Slang-strict)

These were invisible with `dxc` alone and surfaced when the shaders were first compiled with `slangc`:

| File | Bug | Fix |
|---|---|---|
| `basic.hlsl` / `basic.slang` | Duplicate `[[vk::binding(11,0)]]` for `texTransLighting` *and* `texTransLightingSampler` – separate `Texture` and `Sampler` cannot alias the same binding unless combined. DXC aliases, Slang errors. Vulkan validation may also complain. | Sampler moved to `[[vk::binding(12,0)]]`. |
| `smaa_wrap.hlsl` / `.slang` | `#define SamplerState static SamplerState` redefines a type keyword – DXC silently allows macro redefinition of a keyword, Slang (correctly) rejects it. | Macro removed; Slang port uses `import SMAA;` + explicit static handling. See `SMAA.slang` shim. |
| `hang_gpu.hlsl` / `.slang` | Unconditional `vk::RawBufferStore` to `0x100` – an intentional GPU MMU page fault / TDR. DXC compiles it as-is; any dispatch hangs the GPU with no opt-out. | Guarded by `#ifndef ENABLE_HANG_TEST` → safe no-op unless compiled with `-D ENABLE_HANG_TEST`. |
| `volumetric_fog_inject.hlsl`, `volumetric_injection.hlsl` | Copy-paste: `float n011 = Hash3D(ip + float3(0,0,1))` duplicates `n001`'s offset, should be `(0,1,1)` – broken trilinear noise (subtle fog banding). DXC does not warn about dead value. | Fixed to `(0,1,1)`. |
| `ambient.hlsl`, `lighting.hlsl`, `reflection.hlsl`, `decal.hlsl`, `mesh_particle_update.hlsl`, `particle_update.hlsl` | Push-constant blocks ~144–180 bytes exceed Vulkan's `maxPushConstantsSize` guaranteed minimum (128). DXC accepts any size; Slang validates. Desktop drivers with 256 still work, but portable/mobile fails. | Added `// SLANG WARNING` + rationale. Idiomatic Slang would use `ParameterBlock<FrameUniforms>` / `ConstantBuffer` for large per-frame data and keep push constants <128. Flagged for future split. |
| `common.hlsl` et al. | `#pragma pack_matrix(column_major)` vs Slang `row_major` default – DXC default is `column_major`, Slang is `row_major`. Without explicit `-matrix-layout column_major`, every `float4x4` is silently transposed. | `.slang` files remove the pragma and rely on `-matrix-layout column_major` (see `ShaderCompilation.cmake`). `.hlsl` retains pragma with a Slang-note comment. |
| All | HLSL `#include` vs Slang `import`/`module` – not a functional bug but software-engineering: `#include` shares macros and needs guards, `import` is hygienic and enables separate compilation. | Library headers (`pbr_helpers`, `uniforms`, `common`) are now `module`/`import`; consumers use `import`. `common_types.slang` split handles the old `SKIP_BINDINGS` hack idiomatically. |

## Migration script

`tools/slang_migrate.py` is idempotent:

```
# Patch .hlsl in-place for the bugs above (duplicate binding, noise, hang guard, push-constant warnings, pragma notes)
# Generate .slang counterparts with module/import/[shader(...)] and -matrix-layout guidance
python3 tools/slang_migrate.py
```

It preserves `[[vk::binding]]` / `push_constant` / `constant_id` for Vulkan parity and adds a footer
with `-matrix-layout column_major` guidance.

## Testing the migration

```bash
# Slang strict (recommended)
slangc resources/shaders/ambient.slang -target spirv -matrix-layout column_major \
       -fvk-use-dx-layout -fspv-target-env=vulkan1.3 -I resources/shaders -I include \
       -profile ps_6_5 -entry PSMain -o /tmp/ambient.spv && spirv-val /tmp/ambient.spv

# DXC fallback
dxc -T ps_6_5 -E PSMain -spirv -fspv-target-env=vulkan1.3 -I resources/shaders -I include \
    resources/shaders/ambient.hlsl -Fo /tmp/ambient.dxc.spv
```

Both should produce functionally identical SPIR-V (modulo the bug fixes above, which are intentional
divergences from the old DXC-output that was silently wrong).

## References

* Slang language guide: modules, `import`, `[shader(...)]` – https://shader-slang.org
* SPIR-V specifics (push constants, `[[vk::...]]`, matrix layout) – https://docs.shader-slang.org
* Vulkan spec: `maxPushConstantsSize` == 128 minimum, `column_major` vs `row_major`.
