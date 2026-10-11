# Project Zahlen

A **simple** project that integrates modern Vulkan, Jolt Physics, fast and straightforward creative work conversions and game development with pure C++ interface.

It's kinda like Bevy but C++.

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
        .transform_error([](ZHLN::ErrorCode code) -> int {
            // ErrorCode carries the category and the value; promote to Error
            // where somebody actually reads the text.
            ZHLN::Log("CommandLine Error: {}", ZHLN::Error(code).Message());
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
#include <Zahlen/CommandLine.hpp> // GameplayStatus
#include <Zahlen/Engine.hpp>

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

