# SHADER GUIDE — Slang-only (DXC wiped)

> **2026-08-10 — Slang migration complete. DXC has been removed.**
> All shaders are now **Slang-only** `*.slang` (modules, `import`, explicit `[shader(...)]` stages).
> The legacy `*.hlsl` files and the DXC toolchain have been deleted. The build is `slangc`-only.

## Conventions

* **Column-major, right-handed, vector-column, CCW only.** `slangc` is invoked with
  `-matrix-layout column_major` (see `cmake/ShaderCompilation.cmake`). Slang defaults to
  `row_major` — without the flag every `float4x4` uniform / PushConstant is silently
  transposed.

## Build

`cmake/ShaderCompilation.cmake` is **Slang-only**. It `find_program(slangc)` and
`FATAL_ERROR` if not found (install `shader-slang` via Vulkan SDK ≥1.3.296 or
`pacman -S shader-slang`):

```
slangc <shader>.slang -target spirv -profile <vs_6_5|ps_6_5|cs_6_0>
       -entry <VSMain|PSMain|CSMain|...>
       -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
       -I resources/shaders -I include -o <gen>.spv
```

SPIR-V is embedded via `#embed` using the `SHADER_*_SLANG_*_PATH` macros
(e.g. `SHADER_BASIC_SLANG_VS_PATH`).

```bash
# Single-file check
slangc resources/shaders/ambient.slang -target spirv -matrix-layout column_major \
       -fvk-use-dx-layout -fspv-target-env=vulkan1.3 -I resources/shaders -I include \
       -profile ps_6_5 -entry PSMain -o /tmp/ambient.spv && spirv-val /tmp/ambient.spv
```

## Slang idioms

* **Modules:** `pbr_helpers.slang: module pbr_helpers;`, `uniforms.slang: module uniforms;`,
  `common.slang: module common; import pbr_helpers; import uniforms;`
  Consumers `import pbr_helpers; import uniforms;` or `import common;`.
* **Split for `SKIP_BINDINGS`:** The old HLSL hack `#define SKIP_BINDINGS` + `#include "common.hlsl"`
  is replaced by `common` (with `[[vk::binding]]`) and `common_types.slang`
  (types/helpers only). Use `import common_types;` when bindings must be excluded.
* **`[shader("vertex"|"fragment"|"compute")]`** annotates every entry point (`VSMain`,
  `PSMain`, `PSShadow`, `PSForward`, `CSMain`, `Smaa*`).
* **`[[vk::binding]]` / `[[vk::push_constant]]` / `[[vk::constant_id]]`** are preserved
  verbatim for Vulkan 1.3.
* **`SMAA`:** External HLSL library wrapped as `SMAA.slang: module SMAA; __include "SMAA.hlsl"`.
  `smaa_wrap.slang` does `import SMAA;`. The old DXC hack
  `#define SamplerState static SamplerState` is gone — Slang correctly rejects
  redefining a type keyword.

## Silent bugs fixed (previously masked by DXC)

These were invisible with `dxc` and are now caught by `slangc`:

| File (now `.slang` only) | Bug | Fix |
|---|---|---|
| `basic.slang` | Duplicate `[[vk::binding(11,0)]]` for `texTransLighting` and `texTransLightingSampler` | Sampler → `[[vk::binding(12,0)]]` |
| `smaa_wrap.slang` | `#define SamplerState static SamplerState` redefines keyword | Macro removed; `import SMAA;` |
| `hang_gpu.slang` | Unconditional `RawBufferStore` to `0x100` (page fault/TDR) | Guarded by `#ifndef ENABLE_HANG_TEST` → safe no-op |
| `volumetric_fog_inject.slang`, `volumetric_injection.slang` | `n011 = Hash3D(0,0,1)` duplicates `n001`, should be `(0,1,1)` | Fixed |
| `ambient/lighting/reflection/decal/mesh_particle_update/particle_update` | Push constants ~144–180 B > 128 guaranteed | Flagged `// SLANG WARNING`; future `ParameterBlock` |
| `common` et al. | `row_major` vs `column_major` transpose | `-matrix-layout column_major` |
| All | `import`/`module` vs `#include` | Hygienic modules, `common_types.slang` split |

## References

* Slang: modules, `import`, `[shader(...)]` — https://shader-slang.org
* SPIR-V `[[vk::...]]`, matrix layout — https://docs.shader-slang.org
* Vulkan: `maxPushConstantsSize` == 128 minimum
