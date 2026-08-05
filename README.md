# Zahlen Game Engine

A lightweight, low-overhead C++26/C23 game engine framework integrating Vulkan, Jolt Physics, an entity component system (ECS), and Fiber-based multithreading, featuring a fast asset compiler and a live-reloadable Fennel/Lua scripting interface.

Zahlen is designed to be embedded directly into game repositories as a Git submodule, allowing developers to write their game logic as a modular guest library (C++ or Fennel) while the engine manages the core systems in a deterministic, synchronized frame loop.

## Integration & Usage

### 1. Add as a Git Submodule
In your game repository, run the following command to add the engine into your dependencies folder (e.g., `extern/zahlen`):

```sh
git submodule add https://github.com/Evilpasture/project-zahlen.git extern/zahlen
git submodule update --init --recursive
```

### 2. Parent Project Directory Structure
To utilize Zahlen's automated pipelines (Fennel compilation and parallel asset cooking), structure your game repository like this:

```text
MyGame/
├── CMakeLists.txt             # Game build script
├── extern/
│   └── zahlen/                # Engine submodule
├── gameplay/                  # Game C++ source files and modules (.cpp, .cppm)
├── scripts/                   # Game Fennel / Lua scripts (.fnl, .lua, boot.lua)
└── resources/
    └── assets/                # Raw Game assets (.blend, .png, .wav, .jpg)
```

### 3. CMake Integration
In your game's root `CMakeLists.txt`, call `add_subdirectory(extern/zahlen)`. This compiles the engine and exposes its public targets, headers, and build-time pipeline functions:

```cmake
cmake_minimum_required(VERSION 3.28)
project(MyGame LANGUAGES C CXX ASM)

# 1. Include the engine submodule
add_subdirectory(extern/zahlen)

# 2. Build your gameplay dynamic library (libgameplay.so / gameplay.dll)
file(GLOB_RECURSE GAME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/gameplay/*.cppm" "${CMAKE_CURRENT_SOURCE_DIR}/gameplay/*.cpp")
add_library(gameplay SHARED ${GAME_SOURCES})
target_link_libraries(gameplay PRIVATE zahlen_engine)

# 3. Compile your Fennel gameplay scripts
zahlen_compile_fennel(MyGame COMPILED_LUA
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gameplay.fnl"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/main_menu.fnl"
)

# 4. Configure the parallel asset cooking pipeline (zcook + Ninja)
zahlen_configure_game_assets("${CMAKE_CURRENT_SOURCE_DIR}")

# 5. Build your launcher and link against the engine
add_executable(MyGame src/main.cpp)
target_link_libraries(MyGame PRIVATE zahlen_engine)
add_dependencies(MyGame gameplay)
```

### 4. Game Entry Point (src/main.cpp)
Create your game's main entry point in `src/main.cpp`. The engine provides a high-level `ZHLN::Engine::Run` method that encapsulates process initialization, window creation, the synchronized frame loop, and CLI options automatically:

```cpp
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Engine.hpp>
#include <span>

int main(int argc, char* argv[]) {
    return ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)))
        .transform_error([](const ZHLN::Error& err) -> int {
            ZHLN::Log("CommandLine Error: {}", err.Message());
            return EXIT_FAILURE;
        })
        .and_then([](const ZHLN::CommandLineOptions& options) -> std::expected<int, int> {
            if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
                return 0; // Clean exit for CLI queries (--help, --version, etc.)
            }

            ZHLN::SetLogLevel(options.logLevel);

            // ZHLN::Engine::Run automatically manages:
            // 1. Engine creation & window setup
            // 2. Branching to WorldEditor if --editor is passed
            // 3. Loading & hot-reloading libgameplay.so / gameplay.dll via NativeScriptModule
            // 4. Executing the 12-step synchronized frame pipeline
            return ZHLN::Engine::Run(options);
        })
        .or_else([](int errorCode) -> std::expected<int, int> { return errorCode; })
        .value();
}
```

### 5. Writing Game Logic
Your game's entry point must implement the `NativeGameplayUpdate` hook, which the engine loads dynamically from `libgameplay.so` (or `gameplay.dll`) for live hot-reloading. 

Create your active gameplay file (e.g., `gameplay/gameplay.cpp`):

```cpp
#include <Zahlen/Engine.hpp>
#include <Zahlen/Types.hpp>

#if defined(_WIN32)
#define GAMEPLAY_API extern "C" __declspec(dllexport)
#else
#define GAMEPLAY_API extern "C" [[gnu::visibility("default")]]
#endif

namespace Game {
void StartGame(ZHLN::Engine* engine) {
    // Initialize your game-specific entities, physics bodies, and scene here
}
} // namespace Game

GAMEPLAY_API ZHLN::GameplayStatus NativeGameplayUpdate(ZHLN::Engine* engine, float dt) {
    if (!engine) {
        return ZHLN::GameplayStatus::Error;
    }

    // This hook is invoked by the engine every frame during the update phase.
    // If you edit and compile this file, the engine hot-reloads it live.

    return ZHLN::GameplayStatus::OK;
}
```

---

## Standalone Engine Development

If you are testing the engine itself, you can build and run the test launcher standalone.

### Build Requirements
* **CMake (>= 3.28)**: Build automation.
* **C++26 Compiler**: Supporting C++26 standard features and static reflection (GCC 16.1.1+ or Clang 19+).
* **C23 Compiler**: Supporting C23 standard features (specifically `#embed` support).
* **Python**: Used during the asset building phase to generate parallel build rules.
* **Fennel**: Compiler binary installed in your system PATH (used to transpile core scripts).

### Standalone Build Steps
1. Clone the repository and its submodules recursively:
   ```sh
   git clone --recurse-submodules https://github.com/Evilpasture/project-zahlen.git
   cd project-zahlen
   ```
   
2. Create a build directory and configure the project:
   ```sh
   cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release 
   ```
   
3. Build the engine and the default test environment:
   ```sh
   cmake --build build
   ```

4. Run the standalone launcher:
   ```sh
   ./build/zahlen
   ```  

---

## Architecture

For a detailed breakdown of the engine's architecture, 12-step frame loop execution order, deferred render graph topology, and scripting IPC protocol, see [include/ARCHITECTURE.md](include/ARCHITECTURE.md).

## System & Platform Dependencies

The project is primarily developed on Linux and macOS, but also supports Windows (provided you compile with a toolchain that fully supports C++26 reflection and C23 `#embed`).

Ensure these packages are installed on your host OS:

* **Vulkan SDK (>= 1.3)**: Core graphics API, validation layers, and `dxc` (DirectX Shader Compiler) for SPIR-V compilation.
* **zstd (Zstandard)**: Compression algorithm used to build and decompress custom `.pak` assets.
* **Fontconfig** *(Linux & macOS)*: Used by the font manager to locate system fonts for font atlas generation.
* **libevdev** *(Linux only)*: Kernel-level input device wrapper used by the native TTY/KMS backend.
* **libseat** *(Linux only)*: Shared session management library used to acquire input and graphics permissions in TTY mode without root.

## Bundled Libraries (Third-Party)
These are located in the `extern/` and `third_party/` directories:
* **Jolt Physics**: Multi-core 3D physics simulation and collision engine. Also used for fast SIMD math.
* **VulkanMemoryAllocator (VMA)**: Vulkan memory management.
* **LuaJIT**: Just-In-Time compiler for the Lua scripting engine.
* **Dear ImGui**: Immediate-mode graphical user interface for debug overlays and inspector panels.
* **GLFW**: Multi-platform window, Vulkan context, and input handling.
* **SPIRV-Reflect**: Lightweight reflection library for SPIR-V shader bytecode.
* **miniaudio**: C audio playback and mixing library.
* **cgltf**: Lightweight glTF 2.0 parser and loader.
* **stb**: Single-file utilities (`stb_image` and `stb_truetype`).

## LICENSE

This project is licensed under the GNU General Public License version 3.0 or later versions. See the [LICENSE](LICENSE.md) file for more details.
