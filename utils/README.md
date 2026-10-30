# utils/ — the host programs

This is not a junk drawer of dev scripts. Three of the eight programs here are
**steps of the build**: no `zcook`, no `zshader`, no `ecsc` means no engine
binary, because the build cannot produce cooked assets, generated GPU bindings
or the ECS code without them. Run them by hand when you want to, but the
`cmake --build` graph already does.

Scripts, shell glue and Blender-side authoring live in [`../scripts/`](../scripts/).
Nothing here is a hardware driver; the Vulkan layer is under `src/vulkan/`.

## The pipeline they form

```
resources/ + plugins/           assets the game ships
        │
        ├── ecsc ─────────────►  ECS registration code (configure time, Cargo/Rust)
        │
        ├── zcook ────────────►  cooked asset bundles (build time, C++)
        │      └── font bake, GLB import, ninja manifest
        │
        └── zshader ──────────►  SPIR-V + ShaderBindings.hpp + generated GPU types
                                 (build time, C++, drives slangc)
                                          │
                                          ▼
                                   the engine targets
```

## What each program is

| Program | Language | Target(s) | The build runs it? | What it does |
| :--- | :--- | :--- | :--- | :--- |
| `ecsc/` | Rust (Cargo) | built by `cmake/ECSCompiler.cmake` | **yes**, at configure time | Compiles the `.ecs` descriptions into the ECS registration/codegen units; also ships an LSP mode. |
| `zcook/` | C++ | `zcook` (`cmake/AssetPipeline.cmake`) | **yes**, through `add_custom_command` | The offline asset pipeline: GLB import, material cooking, font baking, and the ninja manifest that fans assets out to workers. |
| `zshader/` | C++ | `zshader` (`cmake/ShaderCompilation.cmake`) | **yes**, before the engine compiles | Shader compiler driver: invokes `slangc`, reflects the result, and emits `ShaderBindings.hpp` and the generated GPU struct headers the engine includes. |
| `ztest/` | C++ | `ztest` (`cmake/ZTest.cmake`) | **yes** in the test stage | Test runner and manifest owner; `--replay` drives a crashed worker's recorded command stream. |
| `fidelity/` | C++ | `fidelity` | no | Standalone render-fidelity harness (list/compare/report), driven by `scripts/run_fidelity.sh` and `scripts/capture_fidelity.sh`. |
| `editor/` | C++ | `zahlen_world_editor` | no (`ZHLN_BUILD_EXTRAS`) | World editor: scene authoring against the runtime. |
| `scene/` | C++ | `zahlen_scene_documents`, `zahlen_scene_host` | no (`ZHLN_BUILD_EXTRAS`) | Scene document model plus a host that loads/watches scene files. |
| `benchmark/` | C++ | `zahlen_benchmark_app` | no (`ZHLN_BUILD_EXTRAS`) | Windowless benchmark executable. |

None of these are `install()`ed today: they exist inside the build tree only
(`zahlen_set_bin_output` puts them next to the engine binary). That is the one
piece of "first-class" plumbing still missing — see the note in
`include/ARCHITECTURE.md` §1.4.

## Conventions

* Each program owns its `CMakeLists.txt`, except `zcook`, `zshader` and `ztest`,
  whose targets are created by the cmake module that also wires the custom
  commands around them (`AssetPipeline.cmake`, `ShaderCompilation.cmake`,
  `ZTest.cmake`). That coupling is deliberate: the module and the program define
  one command-line contract, and splitting them is how the two spellings drift.
* A program that generates code must be deterministic: the same inputs produce
  byte-identical outputs, or every build relinks the engine.
* Anything a program writes goes to the build tree, never back into the source
  tree.
