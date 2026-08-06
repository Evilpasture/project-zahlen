# Project Zahlen

A **simple** project that integrates Vulkan, Jolt Physics and ImGUI for hardware raytracing, fast and straightforward creative work conversions and game development with Fennel/Lua.

## Build Requirements
* **CMake (>= 3.25)**: Build automation tool.
* **C++26 Compiler with -freflection**: Supporting C++26 standard features (GCC 16.1.1 or later).
* **C23 Compiler**: Supporting C23 standard features (such as `#embed` support).
* **Python**: Used during the asset building phase to scan level assets and configure the parallel build rules.

## Build Instructions

For creative works, the build system expects Blender files in `./blender/`

You can do it the hard way, or the easy way.

### Manual Build
1. Clone the repository and its submodules:
   ```sh
   git clone --recurse-submodules https://github.com/Evilpasture/project-zahlen.git
   ```
   
2. Create a build directory and configure the project:
   ```sh
   cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release 
   ```
   
3. Build the project:
   ```sh
   cmake --build build
   ```

4. Run the project:
   ```sh
   ./build/zahlen
   ```  

### Convenient Build

1. Clone the repository and its submodules:
   ```sh
   git clone --recurse-submodules https://github.com/Evilpasture/project-zahlen.git
   ```
   
2. Run this script:
   ```sh
   ./tools/build.sh --gcc
   ```

3. Run the project:
   ```sh
   ./build/zahlen
   ```

## Architecture

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
To utilize the automated Fennel compiler and Ninja asset cooking pipelines, structure your game repository as follows:

```text
MyGame/
├── CMakeLists.txt             # Game build script
├── extern/
│   └── zahlen/                # Engine submodule
├── gameplay/                  # Game C++ source files (.cpp, .cppm)
├── scripts/                   # Game Fennel / Lua scripts (.fnl, .lua, boot.lua)
├── resources/
│   └── assets/                # Raw game assets (.blend, .png, .wav, etc.)
└── src/
    └── main.cpp               # Game launcher entry point
```

### 3. Game CMakeLists.txt Boilerplate
Create a `CMakeLists.txt` in your game's root directory. Include the engine submodule and invoke its exported helper functions (`zahlen_compile_fennel` and `zahlen_configure_game_assets`):

```cmake
cmake_minimum_required(VERSION 3.28)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable C, CXX, and ASM (Required for Engine Fibers, Thread.S & mimalloc)
project(TestGame VERSION 0.1.0 LANGUAGES C CXX ASM)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add Engine Submodule
add_subdirectory(extern/zahlen)

# Game C++26 Modules Target (gameplay/*.cppm)
file(GLOB_RECURSE GAMEPLAY_MODULE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gameplay/*.cppm")

if(GAMEPLAY_MODULE_SOURCES)
    add_library(gameplay_modules STATIC)
    target_sources(gameplay_modules PUBLIC FILE_SET CXX_MODULES FILES ${GAMEPLAY_MODULE_SOURCES})
    set_target_properties(gameplay_modules PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_SCAN_FOR_MODULES ON)

    target_include_directories(gameplay_modules PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/gameplay
        ${zahlen_SOURCE_DIR}/include
        ${zahlen_SOURCE_DIR}/src
        ${zahlen_SOURCE_DIR}/extern/JoltPhysics
    )
    target_compile_definitions(gameplay_modules PRIVATE JPH_DOUBLE_PRECISION JPH_SHARED_LIBRARY)
    target_link_libraries(gameplay_modules PUBLIC zahlen_engine zahlen_ecs zahlen_audio Jolt)

    if(COMPILER_HAS_REFLECTION OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        target_compile_options(gameplay_modules PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")
    endif()
endif()

# Dynamic Gameplay Library (gameplay/*.cpp for Live Hot-Reloading)
file(GLOB GAMEPLAY_CPP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gameplay/*.cpp")

if(GAMEPLAY_CPP_SOURCES)
    add_library(gameplay SHARED ${GAMEPLAY_CPP_SOURCES})
    set_target_properties(gameplay PROPERTIES CXX_SCAN_FOR_MODULES ON BUILD_RPATH "$ORIGIN;$ORIGIN/extern/zahlen/extern/JoltPhysics/Build")
    target_include_directories(gameplay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gameplay ${zahlen_SOURCE_DIR}/include ${zahlen_SOURCE_DIR}/src ${zahlen_SOURCE_DIR}/extern/JoltPhysics)
    target_compile_definitions(gameplay PRIVATE JPH_DOUBLE_PRECISION JPH_SHARED_LIBRARY)
    target_link_libraries(gameplay PRIVATE zahlen_engine Jolt)
    if(TARGET gameplay_modules)
        target_link_libraries(gameplay PRIVATE gameplay_modules)
    endif()

    if(COMPILER_HAS_REFLECTION OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        target_compile_options(gameplay PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")
    endif()

    add_custom_command(TARGET gameplay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gameplay> $<TARGET_FILE_DIR:${PROJECT_NAME}>/
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/scripts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gameplay> "${CMAKE_CURRENT_SOURCE_DIR}/scripts/"
        COMMENT "Copying gameplay dynamic library for live hot-reloading..."
    )
endif()

# Dynamically discover and compile ANY .fnl files in scripts/ (handles 0, 1, or many files automatically)
file(GLOB_RECURSE FENNEL_GAME_SCRIPTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.fnl")

if(FENNEL_GAME_SCRIPTS)
    zahlen_compile_fennel(${PROJECT_NAME} COMPILED_GAME_LUA ${FENNEL_GAME_SCRIPTS})
    add_custom_target(compile_game_fennel ALL DEPENDS ${COMPILED_GAME_LUA})
else()
    add_custom_target(compile_game_fennel ALL)
endif()

file(GLOB_RECURSE STATIC_GAME_SCRIPTS LIST_DIRECTORIES false "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.lua" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.sh" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.txt")
list(FILTER STATIC_GAME_SCRIPTS EXCLUDE REGEX ".*/scripts/core/.*")

set(GAME_SCRIPT_SYMLINKS "")
foreach(STATIC_SRC IN LISTS STATIC_GAME_SCRIPTS)
    file(RELATIVE_PATH REL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/scripts" "${STATIC_SRC}")
    get_filename_component(REL_DIR "${REL_FILE}" DIRECTORY)
    set(OUT_FILE "${CMAKE_BINARY_DIR}/scripts/${REL_FILE}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/scripts/${REL_DIR}")
    if(WIN32)
        add_custom_command(OUTPUT "${OUT_FILE}" COMMAND ${CMAKE_COMMAND} -E copy "${STATIC_SRC}" "${OUT_FILE}" DEPENDS "${STATIC_SRC}" VERBATIM)
    else()
        add_custom_command(OUTPUT "${OUT_FILE}" COMMAND ${CMAKE_COMMAND} -E create_symlink "${STATIC_SRC}" "${OUT_FILE}" DEPENDS "${STATIC_SRC}" VERBATIM)
    endif()
    list(APPEND GAME_SCRIPT_SYMLINKS "${OUT_FILE}")
endforeach()

add_custom_target(sync_game_static_scripts DEPENDS ${GAME_SCRIPT_SYMLINKS})
add_dependencies(compile_game_fennel sync_game_static_scripts)

# Configure Game Ninja Asset Cooking Pipeline (zcook)
zahlen_configure_game_assets("${CMAKE_CURRENT_SOURCE_DIR}")

# Game Executable Target
add_executable(${PROJECT_NAME} src/main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/gameplay PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/extern>)
target_link_libraries(${PROJECT_NAME} PRIVATE zahlen_engine imgui Jolt)

if(TARGET gameplay)
    add_dependencies(${PROJECT_NAME} gameplay)
endif()

if(TARGET gameplay_modules)
    target_link_libraries(${PROJECT_NAME} PRIVATE gameplay_modules)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE JPH_DOUBLE_PRECISION)
add_dependencies(${PROJECT_NAME} compile_game_fennel)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /permissive-)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
endif()

if(COMPILER_HAS_REFLECTION OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU") OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    target_compile_options(${PROJECT_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")
endif()
```

### 4. Game Entry Point Boilerplate (`src/main.cpp`)
In your game's source folder, create `src/main.cpp` or anything you want. The engine provides a high-level, framework-like `ZHLN::Engine::Run` method that manages process initialization, engine setup, the synchronized 12-step frame loop, and command-line flags (such as `--editor` or `--fps-limit`):

```cpp
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <span>

int main(int argc, char* argv[]) {
    return ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)))
        .transform_error([](const ZHLN::Error& err) -> int {
            ZHLN::Log("CommandLine Error: {}", err.Message());
            return EXIT_FAILURE;
        })
        .and_then([](const ZHLN::CommandLineOptions& options) -> std::expected<int, int> {
            if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
                return 0; // Clean exit for CLI queries
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

### 5. Active Gameplay Entry Point
Your gameplay logic must implement the `NativeGameplayUpdate` hook, which is dynamically loaded from the compiled shared library and can be live-reloaded during runtime. Magic happens here!

Create `gameplay/gameplay.cpp` or whatever the hell you want:

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
    // Initialize your game-specific entities, physics, and scene state here
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

## Hardware expectations

The project is designed to run on a system with a discrete GPU and a dedicated CPU. Tested on NVIDIA RTX 3050 6GB in Arch Linux.
Should work on macOS too with MoltenVK if you manage to compile it, but expect MoltenVK overhead.

## System & Platform Dependencies

The project is primarily developed on Linux and macOS, but should also work on Windows as originally planned. 
That is true, if your compiler supports standard C++26 features. Otherwise, tough luck. Compile GCC 16.1.1+ yourself.

See [here](https://en.cppreference.com/cpp/compiler_support/26) for compiler support information.

These packages are expected to be installed on the host operating system:

* **Vulkan SDK (>= 1.3)**: Core graphics API, validation layers, and the DirectX Shader Compiler (`dxc`) used for SPIR-V shader compilation.
* **zstd (Zstandard)**: Compression algorithm used to build and decompress custom `.pak` assets.
* **Windows SDK** *(Windows only)*: Windows API headers and libraries. It's expected that you should have the SDK installed on your system.
* **Visual Studio Build Tools** *(Windows only)*: Required to build the project with Clang/LLVM when targeting Windows.
* **Fontconfig** *(Linux & macOS)*: System font customization and configuration library (used to locate standard system fonts for font atlas generation).
* **libevdev** *(Linux only)*: Kernel-level input device wrapper used by the native TTY/KMS fallback backend.
* **libseat** *(Linux only)*: Shared session management library used to acquire input and graphics permissions in TTY mode without root access.
* **X11 / Xlib** *(Linux only)*: Legacy windowing library.
* **Cocoa / QuartzCore** *(macOS only)*: Apple frameworks.

## Bundled / External Libraries
These are located in the `extern/` and `third_party/` directories:

### Physics & Math
* **Jolt Physics**: A multi-core 3D physics simulation and collision engine. Also used for fast SIMD math.
* **VulkanMemoryAllocator (VMA)**: Vulkan memory management utility.

### Scripting & Audio
* **LuaJIT**: JIT runtime for the Lua scripting language.
* **Fennel Compiler**: Used to transpile Fennel scripts to Lua.
* **miniaudio**: Single-file C audio playback and mixing library.

### Graphics & Tooling
* **Dear ImGui**: Immediate-mode graphical user interface for debug overlays and controllers.
* **GLFW**: Multi-platform window, GL/Vulkan context, and input handling.
* **SPIRV-Reflect**: Lightweight reflection library for SPIR-V shader bytecode.
* **RenderDoc**: Integrated in-app graphics debugger hook.

### Utilities & Formats
* **cgltf**: Lightweight glTF 2.0 parser and loader.
* **stb**: Single-file public domain libraries (including `stb_image` for texture loading and `stb_truetype` for font atlas rendering).

## LICENSE

This project is licensed under the GNU General Public License version 3.0 or later versions. See the [LICENSE](LICENSE.md) file for more details.
