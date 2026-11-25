# COM-Only Boundary Refactor Tracker

Branch: `com-only-refactor` (based on `com-api` @ `915ccec7`)

Goal: Collapse the triple API (`Engine.hpp` + `Capabilities/` + `Native*` + `COM/`) into a single COM boundary (`libzahlen_engine.so` + `include/Zahlen/COM/`) with cross-distro packaging support.

## Decisions
- **Animation (Phase 3.3):** Option A — host-driven `Animation::Layer` over `COM::IEngine` (`IEntities`, `IAssetLibrary`, `IGraphicsDevice`, `ISimulation`); `extensions/Animation` stays optional outside `libzahlen_engine.so`.
- **RagdollAuthoring (Phase 3.3):** Cut `gameplay/RagdollAuthoring` until needed.
- **Optional Extensions Packaging (Phase 5.2):** Drop prebuilt static `.a` extension archives; ship optional COM extensions as source/header packages compiled by the consumer toolchain against `Zahlen::Engine`.

---

## Phase 0: Fast Compiler Feedback Loop
- [x] Configure `build-refactor` with `-DZHLN_SYNTAX_ONLY=ON -DZHLN_BUILD_TESTS=OFF -DZHLN_BUILD_SAMPLES=OFF -DZHLN_BUILD_EXTRAS=OFF`
- [x] Verify baseline `zahlen_engine` + `zahlen_cube` syntax-check green

## Phase 1: Delete Duplicate `Native*` Layer and `Capabilities/`
- [x] Delete `extensions/FreeCam/NativeFreeCam.{hpp,cpp}` and `zahlen_native_freecam`
- [x] Delete `extensions/Camera/NativeTargetCamera.{hpp,cpp}` and `zahlen_native_camera`
- [x] Delete `extensions/CharacterController/{NativeCharacterController.hpp,CharacterController.cpp,CharacterMovement.hpp,CharacterMovement.cpp,PlayerInput.hpp,PlayerInput.cpp}` and `zahlen_native_character_controller`
- [x] Delete `extensions/Terrain/{TerrainFactory.hpp,TerrainFactory.cpp,TerrainSystem.hpp,TerrainSystem.cpp}` and `zahlen_native_terrain`
- [x] Delete `extensions/VFX/{Lightning.cppm,Lightning.cpp,LightningUpdate.hpp,LightningUpdate.cpp,Explosion.cppm,Explosion.cpp,ExplosionLifecycle.cpp,ExplosionUpdate.hpp}` and `zahlen_native_vfx`
- [x] Delete `extensions/editor/NativeGUIEditor.{hpp,cpp}` and `zahlen_native_editor`
- [x] Delete `extensions/Benchmark/Viewer/NativeBenchmarkViewer.{hpp,cpp}` and `zahlen_native_benchmark_viewer`
- [x] Delete `extensions/NativeHostUI/` and CMake references
- [x] Delete `plugins/Scene/NativeScene.{hpp,cpp}` and `zahlen_native_scene`
- [x] Delete `plugins/glTF/{GLTFImporter.hpp,GLTFImporter.cpp,glTF.cppm,glTF.cpp}` and `zahlen_gltf` (keep `zahlen_gltf_assets`)
- [x] Delete `plugins/BSP/{BSPImporter.hpp,BSPImporter.cpp,StudioModelImporter.hpp,StudioModelImporter.cpp}` and `zahlen_bsp` (keep `zahlen_bsp_assets`)
- [x] Delete `include/Zahlen/Engine/NativeEngineFactory.hpp` and remove `CreateConfiguredEngine` from `src/engine/EngineCOM.cpp`
- [x] Delete `modules/` and `zahlen_module` target from `CMakeLists.txt`
- [x] Delete `include/Zahlen/Engine/Capabilities/*` (11 headers) and `src/engine/Capabilities.cpp`
- [x] Rewire `src/engine/EngineCOM.cpp`, `src/engine/system/*.cpp`, `src/engine/PrefabFactory.cpp`, `src/engine/MeshBuilder.cpp` (and any other core consumers) to direct internal subsystems
- [x] Verify `ZHLN_BUILD_EXTRAS=OFF` syntax-check clean

## Phase 2: Move Private Engine Headers from `include/Zahlen/` into `src/`
- [x] Split cooked asset headers (`CookedTextureHeader`, `CookedMeshHeader`, `CookedAnimHeader`, `CookedAnimTrack`, plus any `CookedFontHeader` if needed) into `include/Zahlen/Render/CookedFormats.hpp`
- [x] Move `include/Zahlen/Engine/{Engine,World,AssetManager,FramePlan,Frame,GameModule,SystemContext,EngineServices,PrefabFactory,PrefabObservers,PublishedComponents,SceneResources,VisibilityList,PoseUploads,ModelPrefab}.hpp` -> `src/engine/` (keep `Clock.hpp`, `SpawnParams.hpp`)
- [x] Move `include/Zahlen/Render/{RenderContext,FrameScope,Render,SceneData,RenderGraphDump}.hpp` -> `src/render/`
- [x] Move `include/Zahlen/Physics/{Physics,Jolt,MathConversions}.hpp` -> `src/physics/` (keep `PhysicsIDs.hpp`, `PhysicsValues.hpp`, `PhysicsShapes.hpp`)
- [x] Move `include/Zahlen/Audio/Audio.hpp` -> `src/audio/` (keep `AudioTypes.hpp`, `AudioValues.hpp`)
- [x] Move `include/Zahlen/Window/{Window,PlatformHost}.hpp` -> `src/window/` (keep `WindowInput.hpp`)
- [x] Move `include/Zahlen/ECS/{ECS,SystemGraph,SystemParameters,SystemSignature,SystemAccess,Carrier,EntityCommandBuffer,ExtensionServices,Inspection,ComponentPolicy}.hpp` -> `src/ecs/` (keep `Entity.hpp`, `Components.hpp`, `SceneValues.hpp`; resolve `extensions/UI/UITree.hpp` `EventBus` dependency)
- [x] Audit remaining headers in `include/Zahlen/` against the public surface contract
- [x] Update `configure/check_include_provenance.pl` and `configure/check_capability_boundary.py` (and any related governance scripts)
- [x] Verify `ZHLN_BUILD_EXTRAS=OFF` (`zahlen_engine` + `zahlen_cube`) builds and links cleanly

## Phase 3: Enable `-DZHLN_BUILD_EXTRAS=ON` and Convert Holdouts
- [x] 3.1 `extensions/net/Network/` (`NetworkClient.cpp`, `NetworkReplicator.cpp`) -> `COM::IEngine` / `COM::IEntities` + `Network::Step(COM::IEngine&, float dt)`
- [x] 3.2 `gameplay/Interaction/` (`InteractionSystem`, `BSPInteraction`) + `plugins/BSP/BSPInteraction.hpp` -> `Interaction::Layer` over `COM::IEngine` + `Character` host components
- [x] 3.3 `extensions/Animation/` -> delete `RuntimeFactory.cpp`/`Runtime.hpp`, migrate `AnimationContext`, `TwoBoneIK`, and `ProceduralAnimation` to host-driven `Animation::Layer` over `COM::IEngine`; delete `gameplay/RagdollAuthoring/`
- [x] 3.4 `gameplay/ProjectLight/ProjectLightClient/` -> migrate `ClientSession` and `SceneMirror` to `COM::IEngine` + `Character::Layer` + `CameraRig::Layer`
- [x] 3.5 `app/` and `utils/` -> standardize `app/main.cpp`, `utils/editor`, `utils/scene`, `utils/benchmark`, `app/ProjectLightClientMain.cpp` (and `UIEditor`, `zcook`, `zshader`, `fidelity`, `ztest`) onto COM host model (`Zahlen/COM/App.hpp` + `HostUI.hpp`), retiring `Application.cpp`, `DefaultModules.cpp.in`, `NativeSceneDocument`, etc.
- [x] Resolve any remaining extras compile/link issues with `-DZHLN_BUILD_EXTRAS=ON`

## Phase 4: Clean API Signatures (Eliminate `bool` Out-Params)
- [x] Convert internal/host C++ functions (`Animator::Read`, `FontLoader::LoadBakedFont`, `Console::GetEntry`, `GUIEditor` ray helpers, `BSPRead` helpers, and audited peers) from `bool Foo(..., Out&)` to `std::optional<Out>` / `std::expected<Out, ErrorCode>`
- [x] Audit COM vtables (`IEngine`, `IGraphicsDevice`, `IEntities`, `ISimulation`, `IAssetLibrary`, `IActionContext`, `IAudioDevice`, `IDebugDraw`, `IFramePipeline`, `IState`, `ILogicWorld`) so fixed-layout queries return `COM::Option<T>` / `COM::Result<T>` and only variable-length queries use buffer-fill virtuals behind inline `std::vector`/`std::string` helpers

## Phase 5: Fix Cross-Distro Packaging (`cmake/Toolchains.cmake` & `PackageConfig.cmake.in`)
- [x] Strip `ZahlenEngine` install to `libzahlen_engine.so` + stripped `include/` only (no `Jolt` target or `Jolt/` headers, no private headers)
- [x] Convert optional COM extensions to source/header-only packages compiled by consumer toolchain over `Zahlen::Engine` (stop installing prebuilt static `.a` archives)
- [x] Remove `${CMAKE_CXX_COMPILER_VERSION}` and `${ZHLN_GIT_HASH}` (and Jolt/GLIBCXX date locks where applicable) from `ZHLN_TOOLCHAIN_ABI` / `PackageConfig.cmake.in`, preserving glibc floor + runtime `ChainInNode` `LayoutHash` validation
- [x] Update packaging scripts (`scripts/package-toolchain.sh`, `scripts/test-toolchain-install.sh`) and the (since removed) `tests/installed/` consumer smoke test; verify install + consumer build

## Phase 6: Re-enable Tests & Samples (`-DZHLN_BUILD_TESTS=ON -DZHLN_BUILD_SAMPLES=ON`)
- [x] Delete obsolete tests covering removed `Capabilities/`, `Native*`, `Engine.hpp`, or `GameModule` plumbing
- [x] Port remaining unit/integration tests and samples to `COM::CreateEngine` / `COM::CreateLogicWorld` (and dedicated internal core test target where directly testing `src/`)
- [x] Full non-syntax-only build of engine + extras + samples + tests
- [x] Run governance checks and CTest suite
- [x] Verify installed toolchain package consumer build
- [x] Generate clean `.patch` against `com-api` (`915ccec7`)

## Completion & Verification Summary
- **Single-`.so` boundary**: `libzahlen_engine.so` is the sole shipped shared library (161 `ZHLN::` public symbols + intentional `mimalloc` allocator overrides; zero `JPH::`, Vulkan/volk/VMA, Clay, miniaudio, meshoptimizer, SPIRV-Reflect, cgltf, or stb symbol/header leaks; zero `libJolt*` artifacts; Jolt sealed as hidden PIC static archive).
- **Toolchain packaging**: 30 relocatable CMake packages (`ZahlenEngine` shared library, `ZahlenCooker` binary, and 28 source/header packages under `share/zahlen/src/<pkg>`, zero `.a` archives). Verified with `-DZHLN_DEV_MODE=OFF -DZHLN_INSTALL_TOOLCHAIN_PACKAGES=ON`, `zpkg` selective `--only` install + checksum verification + prefix/store relocation, the (since removed) `tests/installed/` consumer smoke, and full out-of-tree compilation & execution of all 29 CMake library packages (including C++26 modules) in both `Release` and `Debug` (`-O0 -g`) against the single Release `libzahlen_engine.so`.
- **Governance**: `ZHLN_GOVERNANCE_CACHE=0 python3 configure/run_governance_checks.py` — 15/15 checks passed (0 violations).
- **Test suites (Release, Fedora 44 aarch64, Mesa lavapipe `LP_NATIVE_VECTOR_WIDTH=256`)**:
  - `tests/TestImage`: PASSED (0 failed)
  - `tests/ztest_cpu`: 221/221 PASSED (0 failed)
  - `tests/extras/*` (29 binaries): 375/375 PASSED (0 failed)
  - `tests/render/GPU_Interface`: 10/10 PASSED (0 failed)
  - `tests/render/GPU_Lighting`: 34/34 PASSED (0 failed)
  - `tests/render/GPU_Pipeline`: 43/43 PASSED (0 failed)
  - `tests/render/extras/TestCombatFX`: 4/4 PASSED (0 failed)
  - `tests/render/GPU_Performance` (`RenderPerformanceValidationSuite`): 9/9 PASSED (0 failed)
  - `utils/zpkg` (`ctest`): 1/1 PASSED (0 failed)
