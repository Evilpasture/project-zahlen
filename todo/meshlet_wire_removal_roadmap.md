# `PackedMeshletWire` Elimination Roadmap

- [x] **Audit & Inventory:** Locate all runtime GPU meshlet uploaders (7 call sites), offline `.zmesh` cooker/tests (`tools/zcook`, `TestCooker`, `TestMeshlets`, `TestMeshShaders`), and ABI/governance hooks (`GpuAbi.cpp`, `check_include_provenance.pl`).
- [x] **Step 1 — Host Meshlet Contract & Renderer `GpuPack` Boundary:**
  - Remove `kMeshletPackedBytes`, `PackMeshlets`, and `PackedMeshletWire` from `include/Zahlen/Render/Meshlet.hpp` and `src/engine/Meshlet.cpp`.
  - Add defaulted `operator==` on `MeshletDesc` / `MeshletBuildResult`, pin `sizeof(MeshletDesc) == 60`, and add unindexed `BuildMeshlets(std::span<const VertexPosition>)`.
  - Add `GpuPack::PackMeshlet(const MeshletDesc&) noexcept -> GPUMeshlet` in `src/render/GpuPack.{hpp,cpp}` writing generated `Slang::GPUMeshlet` by field name.
  - Add value-returning `MeshletStreams` (`include/Zahlen/Render/Types.hpp`), `RenderContext::CreateMeshletBuffer`, and `RenderContext::UploadMeshlets(const MeshletBuildResult&) -> MeshletStreams` (`RenderResources.cpp` + `RenderContextNull.cpp`), with atomic rollback on partial allocation failure and zero out-parameters.
  - Remove the 10 `GPUMeshlet` `static_assert`s from `src/render/GpuAbi.cpp` and update `modules/zahlen.cppm` + `configure/check_include_provenance.pl`.
- [x] **Step 2 — Convert Runtime Callers & `.zmesh` v8 Cooker:**
  - Convert all 7 runtime callers (`MeshBuilder.cpp`, `TerrainFactory.cpp`, `GLTFImporter.cpp`, `BSPImporter.cpp`, `StudioModelImporter.cpp`, `TestNegativeScale.cpp`, `TestUnlitMaterials.cpp`) to value-returning `ctx.UploadMeshlets(...)` + direct `Mesh` construction (deleting mutating `AttachMeshlets` / `AttachTerrainMeshlets` helpers).
  - Store `MeshletBuildResult` directly on `CompiledMesh` in `tools/zcook/Transform.{hpp,cpp}` and bump `.zmesh` `CookedMeshHeader` version `7 -> 8` (`Cook.cpp`, `AssetManager.hpp`, `CookerFixture.hpp`, `TestCooker.cpp`) serializing `MeshletDesc[meshletCount]`.
  - Update `TestMeshlets.cpp`, `TestMeshShaders.cpp`, `ShaderCompilation.cmake`, and `include/ARCHITECTURE.md`.
- [x] **Step 3 — Verification & Patch Delivery:**
  - Verified all 11 governance checks (`python3 configure/run_governance_checks.py` + `perl configure/check_include_provenance.pl`).
  - Syntax-checked `src/render/GpuAbi.cpp`, `src/render/GpuPack.cpp`, and `tests/render/Test{MeshShaders,NegativeScale,UnlitMaterials}.cpp` (`-fsyntax-only`) and built `modules/zahlen.cppm.o` with GCC 16.2 without building `zahlen_render`.
  - Built and ran `ZHLN_BUILD_RENDER=OFF` targets (`zcook`, `CPU_Assets` 17/17 passing, `GLTFImporter.cpp.o`, `BSPImporter.cpp.o`, `StudioModelImporter.cpp.o`, `TerrainFactory.cpp.o`) and emitted `/home/user/remove-packed-meshlet-wire-zahlen.patch`.
