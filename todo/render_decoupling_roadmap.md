# Zero-Cost `zahlen_render` Decoupling Roadmap

## Design Constraints & Goals
- **Strictly zero runtime cost when rendering is active (`ZHLN_BUILD_RENDER=ON`, default):**
  - Keep `ZHLN::RenderContext` non-virtual (`std::unique_ptr<Impl> _impl` PIMPL), statically linked into `zahlen_engine` with `cmake/zahlen_engine.map` internal binding — **0 virtual dispatches, 0 function-pointer indirections, 0 extra allocations, 0 hot-path branches, identical machine code**.
- **Complete render/Vulkan/Slang avoidance when the world simulation is enough (`-DZHLN_BUILD_RENDER=OFF`):**
  - Skip `Vulkan-Headers`, `VulkanMemoryAllocator`, `SPIRV-Reflect`, `src/vulkan` (`zahlen_vulkan` + `volk`), `cmake/ShaderCompilation.cmake` (`zshader`, `Slang`, `zahlen_shader_catalog`, `zahlen_gpu_types`, `.spv` cooking), `zahlen_shader_bytecode`, `HostBlitSwapchain`, and all 56 Vulkan renderer TUs in `src/render/`.
  - Satisfy the 4 `src/render`-defined public symbols (`ZHLN::RenderContext`, `ZHLN::FrameScope`, `ZHLN::PipelineStatsCapture`, `ZHLN::GetRenderGraphDump`) via a single-TU world-only null implementation (`src/render/null/RenderContextNull.cpp`) with zero `#ifdef` clutter in engine, plugin, extension, or gameplay layers.
- **No extra CPU rendering backends:** Mesa `lavapipe` already covers `VK_EXT_descriptor_heap` when software Vulkan execution is required.

---

## Phases

- [x] **Phase 1: Relocate CPU-Only World Geometry & Diagnostics Out of `src/render/` into `src/engine/`**
  - Move `src/render/Meshlet.cpp` $\to$ `src/engine/Meshlet.cpp` (`BuildMeshlets`, `PackMeshlets`) using an explicit 64-byte `PackedMeshletWire` record (`static_assert(sizeof(PackedMeshletWire) == kMeshletPackedBytes)`), while locking Slang's reflected `GPUMeshlet` layout against `kMeshletPackedBytes` and exact member offsets in `src/render/GpuAbi.cpp`.
  - Move `src/render/MeshBuilder.cpp` $\to$ `src/engine/MeshBuilder.cpp` (`PrefabFactory::Create*Mesh`, which uses only the public `RenderContext` API) and drop its unused `#include "Resources.hpp"`.
  - Move `src/render/RenderDocInit.cpp` $\to$ `src/engine/diagnostics/RenderDocInit.cpp` (`LoadRenderDocLibrary`, called solely by `src/engine/EngineGlobals.cpp`).
  - Move `meshoptimizer` `PRIVATE` linkage from `zahlen_render` to `zahlen_engine`.

- [x] **Phase 2: Sever False Render & Shader Compile-Order Edges in `ZHLN_BUILD_RENDER=ON` Builds**
  - Remove dead `${PROJECT_SOURCE_DIR}/extern/volk`, `${CMAKE_BINARY_DIR}/generated_shaders`, `add_dependencies(zahlen_gui zahlen_gpu_types)`, and `zahlen_vulkan` from `src/gui/CMakeLists.txt` (`src/gui/` has zero Vulkan or shader includes).
  - Remove `${CMAKE_BINARY_DIR}/generated_shaders` from `zahlen_engine`'s `PUBLIC` `target_include_directories` in `CMakeLists.txt` (zero headers under `include/` reference `generated_shaders`).

- [x] **Phase 3: Link-Time `ZHLN_BUILD_RENDER` Option & World-Only `RenderContextNull`**
  - Add `option(ZHLN_BUILD_RENDER "Build the Vulkan descriptor-heap renderer (OFF links the zero-overhead world-only null RenderContext)" ON)` in `CMakeLists.txt`.
  - Guard `Vulkan-Headers`, `VulkanMemoryAllocator`, and `SPIRV-Reflect` in `cmake/ThirdParty.cmake` behind `if(ZHLN_BUILD_RENDER)`.
  - Guard `src/vulkan`, `ShaderCompilation`, `zahlen_vulkan`, `spirv-reflect-static`, `zahlen_shader_bytecode`, `zshader`, and `HostBlitSwapchain` in `CMakeLists.txt` behind `if(ZHLN_BUILD_RENDER)`.
  - Implement `src/render/null/RenderContextNull.cpp` and branch `src/render/CMakeLists.txt` on `if(ZHLN_BUILD_RENDER)` so `ZHLN_BUILD_RENDER=OFF` compiles only `null/RenderContextNull.cpp` into `zahlen_render` with stateful handle generation and mesh/material lookup registries.
  - Guard `tests/render` in `tests/CMakeLists.txt` when `ZHLN_BUILD_RENDER=OFF`.

- [x] **Phase 4: Governance, End-to-End World-Only Build & Test Verification**
  - Run `configure/run_governance_checks.py --no-cache` and `configure/check_include_governance.py`.
  - Configure and build `zahlen`, `zcook`, `zahlen_module`, all `CPU_*` test suites, all `Extras_*` test suites, and `samples/*` with `-DZHLN_BUILD_RENDER=OFF` (with zero Vulkan/Slang submodules initialized!), and run `ctest` + headless engine smoke verification.

- [x] **Phase 5: Sever `zahlen_render` Binary & Linker Coupling to `zahlen_window` and `zahlen_filesystem`**
  - Extract `PresentationTarget` and `NativeSurfaceHandle` into a zero-allocation, header-only `zahlen_presentation` `INTERFACE` seam (`src/presentation/PresentationTarget.hpp`), removing `PresentationTarget.cpp`, `NativeSurfaceHandle.cpp`, `${PROJECT_SOURCE_DIR}/src/window`, and `PRIVATE zahlen_window` (plus transitive `glfw` link propagation) from `zahlen_render`.
  - Invert shader hot-reload file watching into `Kernel` (`RenderContext::NotifyShaderChanged(std::string_view)`), eliminating `<Zahlen/FileSystem/FileWatcher.hpp>` and all `ZHLN::FS::FileSystemWatcher` symbol references from `zahlen_render`.
  - Harden `configure/check_subsystem_boundaries.py` to resolve bare cross-subsystem headers, enforce the internal seam DAG, and forbid `zahlen_window` / `zahlen_filesystem` link or header edges in `src/render/` and `src/vulkan/`.
