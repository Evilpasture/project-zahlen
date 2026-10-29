# Project Zahlen

A **simple** project that integrates modern Vulkan, Jolt Physics, fast and straightforward creative work conversions and game development with pure C++ interface.

This project's renderer proudly has zero:

- `VkRenderPass`: Killed by dynamic rendering.
- `VkFramebuffer`: Killed by dynamic rendering.
- `VkDescriptorSetLayout`: Killed by descriptor heaps.
- `VkDescriptorPool`: Killed by descriptor heaps.
- `VkDescriptorSet`: Killed by descriptor heaps.
- `VkSampler` handles: Avoided by using descriptor heaps; the create info goes straight to `vkWriteSamplerDescriptorsEXT`.
- `VkPipelineLayout`: Killed by descriptor heaps.
- `VkShaderModule`: Deprecated in Vulkan 1.4 and killed by VK_KHR_maintenance5.
- `VkPipelineCache`: Killed by pipeline binaries.
- `VkBufferView`: Never existed due to buffer device addresses.
- `VkEvent`: Never existed due to synchronization2.
- `VkFence`: Replaced entirely by monotonic `VK_KHR_timeline_semaphore` completion values.

Besides that, it's kinda like Bevy but C++26 and Vulkan.

## Build Requirements
* **C++26 Compiler with -freflection**: Supporting C++26 standard features (GCC 16.1.1 or later). At this point, if you have a modern compiler I assume you know what you're doing.

## Build Instructions

For creative works, the build system expects Blender files in `./blender/` depending on your CMakeLists.txt.
For compiled assets, the build system expects usually in resources/assets/ as an unofficial convention.

Run tools/build.sh and fix every error you go along the way if you're missing dependencies.

### Runtime Directories

A dev tree keeps its caches and data under `build/`, while a distributed or
hand-launched copy writes to the per-user cache directory
(`~/Library/Caches/Zahlen`, `$XDG_CACHE_HOME/zahlen`, `%LOCALAPPDATA%\Zahlen\Cache`)
and looks for `data/base.pak` next to the executable. `ZHLN_CACHE_DIR` and
`ZHLN_DATA_DIR` override that, as do `RenderConfig::pipelineCachePath` and
`RenderConfig::crashDumpPath` in code. See
[include/ARCHITECTURE.md](include/ARCHITECTURE.md) section 9.

CDN and remote-download caches manage freshness, integrity, and retention:
ETag/Last-Modified revalidation runs on every request in `ZHLN_DEV_MODE` (every
5 minutes otherwise), and persisted checksums catch local byte corruption.
Bodies expire after 7 days; bodies plus metadata are bounded to 1 GiB per cache
directory. Cleanup runs on open and normal use, without caller invalidation.
See [CDN caching](extensions/net/CDN/README.md) for server requirements,
validation limits, configuration, and the lifetime of returned paths.

## Architecture

The optional source tree is split by role: `plugins/` contains asset formats and codecs, `extensions/` contains reusable engine subsystems plus network/platform I/O, and `gameplay/` contains domain-specific systems and project integrations. The existing top-level `modules/` remains reserved for Core C++ module interfaces.

For a detailed breakdown of the engine's architecture, frame loop execution order, deferred render graph topology, and scripting IPC protocol, see [include/ARCHITECTURE.md](include/ARCHITECTURE.md).

## Integrating as a Submodule

You can embed `project-zahlen` directly into an external game repository as a Git submodule. This allows you to develop your game logic independently while using the engine as a library.

### 1. Add Submodule
Add the engine to your game repository's dependencies folder (e.g., `extern/zahlen`):

```sh
git submodule add https://github.com/Evilpasture/project-zahlen.git extern/zahlen
git submodule update --init --recursive
```

### 2. Recommended Directory Structure
To utilize the Ninja asset cooking pipeline and declarative state files, structure your game repository as follows:

```text
MyGame/
├── CMakeLists.txt             # Game build script
├── extern/
│   └── zahlen/                # Engine submodule
├── gameplay/                  # Game C++ source files (.cpp, .cppm)
├── config/                    # Declarative Lua / TOML configuration (graphics.lua)
├── resources/
│   └── assets/                # Raw game assets (.blend, .png, .wav, etc.)
└── src/
    └── main.cpp               # Game launcher entry point
```

### 3. Game CMakeLists.txt
A game only needs to link the application preset; the engine, CLI, crash
handling and available default gameplay extensions are its transitive dependencies.

```cmake
cmake_minimum_required(VERSION 3.28)
project(MyGame LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(extern/zahlen)
add_executable(my_game src/main.cpp)
target_link_libraries(my_game PRIVATE zahlen_application)
target_compile_definitions(my_game PRIVATE JPH_DOUBLE_PRECISION)
zahlen_enable_reflection(my_game)
zahlen_add_runtime_dlls(my_game) # Windows: stage engine/Jolt DLLs beside the game
```

The engine requires C++26 static reflection (see Build Requirements). For an
opt-in core-only build, set `ZHLN_BUILD_EXTRAS=OFF` before `add_subdirectory`;
`zahlen_application` still exists, just without optional modules. Project-specific
asset cooking and hot-reload targets can be added separately when needed.

### 4. Game Entry Point (`src/main.cpp`)
Link the reusable application layer (`target_link_libraries(my_game PRIVATE zahlen_application)`) instead of composing engine services, optional modules, crash handling and the main loop in each game. For example:

```cpp
#include <Zahlen/App.hpp>
#include <Zahlen/PrefabFactory.hpp>

int main(int argc, char* argv[]) {
    return ZHLN::App()
        .OnStart([](ZHLN::Engine& engine) {
            // Persistent entities are created ONCE, after the default scene exists.
            (void) ZHLN::PrefabFactory::CreateBox(engine, JPH::Vec3(1.0f, 1.0f, 1.0f));
        })
        .Run(argc, argv, [](ZHLN::Engine& engine, float dt) {
            // Input has been pumped; this runs once each frame before engine.Tick(dt).
            // Put input/gameplay updates here, not a new CreateBox every frame.
            (void) engine;
            (void) dt;
        });
}
```

`App().Run([](ZHLN::Engine& engine) { /* per-frame code */ })` also works when you don't need command-line flags. The `(Engine&, float dt)` form gets the exact delta passed to `Tick`. Both `App().Run(callback)` **and** `App().Run()` without a callback leave the legacy `scripts/gameplay` loader off by default. With `argc, argv`, standard engine flags such as `--headless`, `--vsync` and `--fps-limit` work; `--help`, `--version` and `--print-graph` exit before engine creation.

The default `zahlen` binary (`app/main.cpp`) is a compiled C++ sandbox: it spawns a physics ground and falling boxes, lights them with a sun and moving point light, fires impulse-driven cubes with **F**, and plays a short procedural sound. **R** resets the view; **Esc** quits. When extras are built, FreeCam provides WASD movement, right-mouse look and Shift sprint. The sandbox uses no DLL, scene-document parser, or optional-extension `#ifdef`s; the optional CharacterController module is installed by `App` but this fly-camera demo does not spawn a character. Move larger projects into their own `GameSession` and ECS systems rather than growing `main.cpp` indefinitely.

`App` owns the outer frame loop: it initializes Platform/TaskSystem, creates an Engine, installs the available baked default font and CharacterController, target camera, Interaction, Terrain, Animation/IK, FreeCam and RagdollAuthoring, builds the scene, polls events, calls your game logic, then `Engine::Tick(dt)` and tears down in order. It does **not** install the scene-host diagnostic fallback (which would replace a compiled game's scene). A `ZHLN_BUILD_EXTRAS=OFF` build uses just Core. Override the preset with `.WithDefaultModules(false)`, add project modules with `.Install([](ZHLN::Engine& engine) { /* install */ })` before the first scene, use `.OnUI(...)` for UI, and pass `ZHLN::EngineRunConfig{...}` to `App(config)` for launch settings. `.OnStart(...)`, `.Install(...)`, and `.OnUI(...)` are application methods, not config delegates. `Engine::Run` remains available for bespoke hosts; its fourth `EventPoller` argument is an explicit low-level frame seam.

### Standalone tools (with `ZHLN_BUILD_EXTRAS=ON`)

| Executable | Entrypoint | Purpose |
| :--- | :--- | :--- |
| `zahlen` | `app/main.cpp` | Ordinary game using `ZHLN::App()`; built even with extras OFF |
| `zahlen-editor` | `tools/editor/main.cpp` | World editor with its own paused-simulation loop; `--scene <file.toml|file.lua>` imports a document, Ctrl+S saves `scene.toml` |
| `zahlen-benchmark` | `tools/benchmark/main.cpp` | Headless logic workload; tune with `ZHLN_BENCH_ENTITIES` and `ZHLN_BENCH_TICKS` |
| `zahlen-scene-host` | `tools/scene/main.cpp` | Scene/document and file-watcher integration; `--scene` and `--graphics-config` (Lua when available), optional Discord/fallback; `--legacy-gameplay-dll` explicitly tests deprecated hot reload |
| `zahlen_ui_editor` | `app/UIEditor.cpp` | Separate UI-tree editor (unchanged) |

The world editor and scene host share document import in `tools/scene/SceneDocument.cpp`. Only the scene host installs the Lua graphics file watcher; it installs a diagnostic fallback when no scene document was requested, so a loaded document is not overwritten by it. The fallback reports a missing DLL only after an explicit legacy opt-in. The benchmark does not initialize a window or engine. For interactive benchmark panels, use the existing `zahlen_benchmark_viewer` library in a custom host; the ordinary game has no F3/F4 benchmark overlay. Legacy `zahlen --editor` / `zahlen --benchmark` flags are rejected with a pointer to the tool binaries.

### 5. Deprecated DLL gameplay (explicit compatibility mode)

The old `GAMEPLAY_API` / `NativeGameplayUpdate` shared-library hook is **legacy**.
Ordinary games compile their source into the executable and call `App().Run(...)`;
they neither load nor watch `scripts/gameplay.dll` (`libgameplay.so` / `.dylib`).
`Engine::Create`, `Engine::Run`, and `App().Run()` without a callback also default
to **no** DLL loader. The retained `EngineConfig::enableNativeGameplay` and
`EngineRunConfig::enableNativeGameplay` booleans opt in explicitly, for
experiments or migration only:

```cpp
// Compatibility-only host: do not also pass a compiled gameplay callback.
return ZHLN::App(ZHLN::EngineRunConfig {.enableNativeGameplay = true})
    .Run(argc, argv);
```

The scene diagnostic tool has a separate `zahlen-scene-host --legacy-gameplay-dll`
flag (mutually exclusive with `--scene`). `App` rejects an opt-in DLL and a
compiled `Run(callback)` in the same process. `--driver=native` remains an alias
for the C++ frame driver; it does **not** enable the DLL loader.

Cross-library allocation, vtable and reflected-type identity are not a stable
hot-reload ABI. The compatibility adapter remains in `src/engine/NativeScriptModule.hpp`
but is deprecated; prefer recompiling the game or iterating on TOML/Lua scene
documents in `zahlen-scene-host` and its Lua graphics-profile watcher. Document
loading is distinct from hot-reloading C++ gameplay code. See the isolated
[legacy DLL compatibility guide](docs/legacy-gameplay-dll.md) only if you need
to migrate an existing dynamic module.

## Hardware expectations

The project is designed to run on a system with a discrete GPU and a dedicated CPU. Tested on NVIDIA RTX 3050 6GB in Arch Linux.
Should work on macOS too if you manage to compile it, but you need a CPU rasterizer due to missing VK_EXT_descriptor_heap support in MoltenVK and KosmicKrisp.

## System & Platform Dependencies

The project is primarily developed on Linux and macOS and Windows.
You can compile this project. That is true, if your compiler supports standard C++26 features. Otherwise, tough luck. Compile GCC 16.1.1+ yourself.

The project can also be compiled with Bloomberg Clang with its own libcxx and libunwind and work effectively. 
However, precompiled headers are buggy due to unknown internal compiler errors. tools/build.sh --p2996 should handle some of the work as long as you have your build ready.

See [here](https://en.cppreference.com/cpp/compiler_support/26) for detailed compiler support information.

These packages are expected to be installed on the host operating system: Read the Dockerfile.

## Bundled / External Libraries
These are located in the `extern/` and `third_party/` directories. To not update this everytime I add or remove a dependency, ls the directories. Take my words, it's all open source.


## LICENSE

This project is licensed under the GNU General Public License version 3.0 or later versions. 

All dependencies are licensed under their respective licenses too.

