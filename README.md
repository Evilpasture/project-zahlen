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
`ZHLN_DATA_DIR` override that, and `EngineConfig::crashDumpPath` overrides the crash-dump path in code. See
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

For a detailed breakdown of the engine's architecture, frame loop execution order and deferred render graph topology, see [include/ARCHITECTURE.md](include/ARCHITECTURE.md).

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
asset cooking and data/profile iteration targets can be added separately when needed.

### 4. Game Entry Point (`src/main.cpp`)
Link the reusable application layer (`target_link_libraries(my_game PRIVATE zahlen_application)`) instead of composing engine services, optional modules, crash handling and the main loop in each game. The target publishes the app-owned headers from `app/include/Zahlen/` (including `App.hpp` and `AppOutcome.hpp`); Core does not expose this include root. The `#include <Zahlen/App.hpp>` spelling is unchanged. For example:

```cpp
#include <Zahlen/App.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <cstdlib>

int main(int argc, char* argv[]) {
    const auto result = ZHLN::App()
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
    return result ? EXIT_SUCCESS : EXIT_FAILURE; // only main translates to an OS code
}
```

`App().Run([](ZHLN::Engine& engine) { /* per-frame code */ })` also works when you don't need command-line flags. The `(Engine&, float dt)` form gets the exact delta passed to `Tick`. `App().Run()` without a callback is for games composed entirely from ECS systems and installed modules. The CLI path takes `std::span<char* const>` (with or without a gameplay callback); the `Run(argc, argv, ...)` overloads shown above validate C-style arguments and adapt them to that span. Standard engine flags such as `--headless`, `--vsync` and `--fps-limit` work; `--help`, `--version` and `--print-graph` exit before engine creation.

Games can register their own flags before running, without pre-filtering argv:

```cpp
// Inside main(int argc, char* argv[]):
std::string level = "courtyard";
auto app = ZHLN::App();
app.AddCliOption(
    "--level", "<name>", "Initial level",
    [&](std::string_view name) -> std::expected<void, ZHLN::CommandLineError> {
        if (name.empty()) return std::unexpected(ZHLN::CommandLineError::MissingValue);
        level.assign(name);
        return {};
    },
    "-l"
);
app.Install([&](ZHLN::Engine& engine) {
    // The exact config used to create the engine is available on all Run paths.
    auto config = engine.Service<ZHLN::EngineConfig>();
    if (config) { /* use config->headless, config->vsync, and level here */ }
});
const auto result = app.Run(argc, argv); // --level=courtyard and -l courtyard both work
return result ? EXIT_SUCCESS : EXIT_FAILURE; // in main() only
```

`Run` returns `ZHLN::AppRunResult` (`std::expected<AppExitReason, ErrorCode>`), not an OS status. Successful reasons distinguish `UserClosed` (including an application-requested primary-window close), `CliHelpRequested`, `CliVersionRequested`, and `CliGraphRequested`. CLI errors keep the exact `CommandLineError` (e.g. `result.error().Is(CommandLineError::InvalidValue)`), and engine creation errors preserve the underlying `ErrorCode`. `AppError::InvalidArguments`, `SceneInitFailed`, and `FrameTickFailed` cover failures that have no lower-level error code. Only executable `main()` translates the result to `EXIT_SUCCESS` or `EXIT_FAILURE`. `KernelApp` uses the same result type and preserves Kernel/render/device-recovery error codes. Typed results allow embedding and precise assertions; process-wide platform/task/signal ownership still requires callers to coordinate simultaneous runs.

`.AddCliOption(ZHLN::CliFlag{...})` and `.WithCliOptions(std::span<const ZHLN::CliFlag>)` are also available. Actions run during parsing, return `CommandLineError` on failure, and may borrow application state during synchronous `Run`. The generic `zahlen_command_line` parser knows nothing about the engine. In `zahlen_application` (and `zahlen_host_runner` for Kernel-only tools), `ParseEngineConfig` reflects `EngineConfig`, adds host-only `--print-graph` / `--debug-attach`, and then adds game flags. Help and version are generic parser stops. There is no engine CLI preset or raw argv service. Installers, gameplay systems and `OnStart` can read `Engine::Service<EngineConfig>()` or `ECS::Res<EngineConfig>` on **both** CLI and no-CLI runs.

`.WithConfig(launch)` binds reflected fields to flags, converting `godMode` to `--god-mode` (and `fpsLimit` to `--fps-limit`). It supports non-bool integrals, `bool`, `float`, `double`, `std::string`, `ZHLN::FixedString<N>` (including `String32` / `String64` / `String128` / `String256`), and enums. For example:

```cpp
enum class Quality { Low, High };
struct Launch {
    bool godMode = false;
    float gamma ZHLN_ANNOTATION(ZHLN::CliRange<0.5f, 2.0f> {}) = 1.0f;
    std::string scene = "arena";
    ZHLN::String64 server {"localhost"};
    Quality quality = Quality::Low;
};
Launch launch;
auto app = ZHLN::App();
app.WithConfig(launch); // --god-mode, --gamma, --scene, --server, --quality
```

A bare bool flag sets `true`; `--god-mode=off` / `--god-mode=false` set `false` (also `on`, `true`, `1` / `0`, `yes` / `no`). Non-bool flags require a nonempty value; floats use `std::from_chars` in general format, reject non-finite values, and may use `CliRange<Min, Max>` just like integers. Enums accept exact `Reflect::StringToEnum<E>()` names and show the `Reflect::EnumNames<E>()` choices in `--help` (e.g. `--quality <Low|High>`). `ZHLN::Description<"...">` supplies help text and `ZHLN::CliShort<'c'>` supplies a short alias; the sandbox demonstrates both for `--cubes` in `app/main.cpp`. Generated flag names, aliases and enum-help text are owned by the application, including after a copy. Fixed-capacity strings reject overlong inputs rather than truncating them, and each field changes only after its input is valid. Keep the borrowed `launch` alive through synchronous `Run`. Because the tokenizer treats a leading `-` as another flag, spell negative numbers as `--threshold=-0.125`.

`EngineConfig` is the single configuration value for launch, physics, renderer and frame pacing. Every supported scalar field binds automatically (e.g. `--width=640`, `--headless=on`, `--fps-limit=120`, `--validation-mode=GPU`, `--log-level=Verbose`); nested aggregates use prefixes (`--physics-max-bodies=4096`). The non-owning `initialPipelineBinaryData` span is intentionally C++-only. Duplicate keys or aliases between engine, game and custom flags fail with `CommandLineError::DuplicateFlag` rather than shadowing or mirroring another value. Invalid values retain typed errors without committing a partial field. Former spellings such as `--validation=gpu` and `--renderdoc` are not aliases; update scripts to the reflected flags.

For applications that must validate settings or acquire resources before an engine exists, `.BeforeEngineCreate([&]() -> std::expected<void, ZHLN::ErrorCode> { /* prepare */ return {}; })` runs after successful CLI parsing (but not for help/version/graph early exits) and before process/engine creation. A failure is returned unchanged as a typed `AppRunResult` error. For example, the optional ProjectLight client uses it to validate its launch settings and connect only after CLI validation, without creating an engine on failure.

Games may also use `.AddLayer(layer)` for a move-only or copyable object implementing any of `Start(Engine&)`, `Tick(Engine&, float)`, `UI(Engine&)`, and `Stop(Engine&)`; UI-only and Stop-only layers are valid. Layers are owned by the application. `Start` runs after scene creation, `Tick` runs after events but before an optional per-frame `Run(callback)`, and `UI` runs in registration order in the Engine's UI frame phase. Once scene initialization succeeds, `Stop` runs in reverse registration order with the Engine still alive, on normal closure or frame failure (but not on CLI early exits or failed initialization). Dispatch uses function pointers, not virtual methods; state is allocated once per layer. The sandbox registers `SandboxGame` as a layer instead of storing its update in a capturing callback.

The default `zahlen` binary (`app/main.cpp`) is a compiled C++ sandbox: it spawns a physics ground and falling boxes, lights them with a sun and moving point light, fires impulse-driven cubes with **F**, and plays a short procedural sound. `--cubes=<0..64>` (`-c`) selects the initial box count and `--seed=<integer>` reproducibly jitters their positions; both appear in `zahlen --help`. **R** resets the view; **Esc** quits. When extras are built, FreeCam provides WASD movement, right-mouse look and Shift sprint. The sandbox needs no scene-document parser or optional-extension `#ifdef`s; the optional CharacterController module is installed by `App` but this fly-camera demo does not spawn a character. Move larger projects into their own `GameSession` and ECS systems rather than growing `main.cpp` indefinitely.

`App` owns the outer frame loop: the shared `ProcessScope` initializes Platform/TaskSystem, then App creates an Engine, installs the available baked default font and CharacterController, target camera, Interaction, Terrain, Animation/IK, FreeCam and RagdollAuthoring, builds the scene, polls events, calls your game logic, then `Engine::Tick(dt)` and tears down in order. CMake generates the ordered default-module calls from targets actually present in the build; `app/Application.cpp` contains no optional-module preprocessor guards. `Kernel::ProcessEvents` handles primary renderer resizing and `Engine::ProcessEvents` consumes the corresponding ECS input notification before gameplay; there is no resize/skip branch in App's loop. `FramePacer` owns App's optional FPS cap. It does **not** install the scene-host diagnostic fallback (which would replace a compiled game's scene). A `ZHLN_BUILD_EXTRAS=OFF` build uses just Core. Override the preset with `.WithDefaultModules(false)`, add project modules with `.Install([](ZHLN::Engine& engine) { /* install */ })` before the first scene, implement `UI(Engine&)` on each layer that draws an overlay, and pass `ZHLN::EngineConfig{...}` to `App(config)` for launch settings. `.OnStart(...)` and `.Install(...)` are application methods, not config delegates. `Engine::Run` remains available for bespoke hosts; its fourth `EventPoller` argument is an explicit low-level frame seam.

### Standalone tools (with `ZHLN_BUILD_EXTRAS=ON`)

| Executable | Entrypoint | Purpose |
| :--- | :--- | :--- |
| `zahlen` | `app/main.cpp` | Ordinary game using `ZHLN::App()`; built even with extras OFF |
| `zahlen-editor` | `tools/editor/main.cpp` | World editor with its own paused-simulation loop; `--scene <file.toml|file.lua>` imports a document, Ctrl+S saves `scene.toml` |
| `zahlen-benchmark` | `tools/benchmark/main.cpp` | Headless logic workload; tune with `ZHLN_BENCH_ENTITIES` and `ZHLN_BENCH_TICKS` |
| `zahlen-scene-host` | `tools/scene/main.cpp` | Scene/document and file-watcher integration; `--scene` and `--graphics-config` (Lua when available), optional Discord presence and a diagnostic scene when no document is selected |
| `zahlen_ui_editor` | `app/UIEditor.cpp` | Separate UI-tree editor using the Kernel-only `KernelApp` runner |

Kernel-only tools can link `zahlen_host_runner` and use `<Zahlen/KernelApp.hpp>` rather than creating a 3D/ECS Engine. `KernelApp().Named("Tool").Input(receiver).OnStart(...).OnInputFrame(...).OnUpdate(...).OnRender(...).OnDeviceRestored(...).OnShutdown(...).Run(argc, argv)` owns CLI early exits, `ProcessScope`, input pumping, frame acquisition/presentation, device-loss recovery, and pacing. The UI-tree editor supplies only its document, two-window UI, and font-atlas rebuild policy; Engine games still use `App` and the Engine's own frame graph.

The world editor and scene host share document import in `tools/scene/SceneDocument.cpp`. Only the scene host installs the Lua graphics file watcher; it installs a diagnostic fallback when no scene document was requested, so a loaded document is not overwritten by it. The benchmark does not initialize a window or engine. For interactive benchmark panels, use the existing `zahlen_benchmark_viewer` library in a custom host; the ordinary game has no F3/F4 benchmark overlay. `zahlen --editor` and `zahlen --benchmark` are not engine flags: the game rejects them as unknown arguments. Launch `zahlen-editor` or `zahlen-benchmark` directly. (The independent `ztest --benchmark` test-runner flag is unaffected.)

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

