# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 1. Force the compiler selection BEFORE project()
if(WIN32 AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

project(zahlen LANGUAGES C CXX ASM)

# --- BUILD TYPE & GLOBAL FLAGS ---
# Everything in this section must be set before ANY add_subdirectory: directory
# scope snapshots these variables when each child directory is processed, so a
# flag added here reaches every vendor and subsystem target, while one added
# after a subdirectory was processed reaches none of it.

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Qunused-arguments)
    add_link_options(-Qunused-arguments)
endif()

# --- DWARF IN RELEASE, ALWAYS ---
# An optimized build without debug info produces stacks that cannot be
# symbolized, which makes release-mode crashes and profiler output worthless.
# Plain Release therefore carries the same -g that RelWithDebInfo already has.
# Plain (non-cache) set, so a user who deliberately passes -DCMAKE_CXX_FLAGS_RELEASE
# keeps their value and merely gains -g on top of it.
if(NOT CMAKE_CXX_FLAGS_RELEASE MATCHES "(^| )-g")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g")
endif()
if(NOT CMAKE_C_FLAGS_RELEASE MATCHES "(^| )-g")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -g")
endif()

if(UNIX OR APPLE)
    set(CMAKE_ASM_COMPILE_OBJECT "<CMAKE_ASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> -c <SOURCE>" CACHE STRING "ASM compile command" FORCE)
endif()

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fno-rtti -fno-exceptions")
    execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE MACOS_SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CMAKE_SYSROOT ${MACOS_SDK_PATH})
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# clangd walks from the source file toward the repo root and never into a
# nested binary dir (build/p2996, build/debug, …). A source-root symlink is
# what lets `clangd --check=src/engine/Engine.cpp` see Jolt and Zahlen
# headers. The link may dangle until configure finishes writing the file.
if(NOT WIN32)
    file(CREATE_LINK
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/compile_commands.json"
        SYMBOLIC RESULT _zhln_compdb_link
    )
    if(_zhln_compdb_link)
        message(STATUS "clangd: could not symlink compile_commands.json (${_zhln_compdb_link})")
    endif()
endif()

# --- CONFIG MODULES (cmake/) ---
# CompilerWarnings  - the warning sets and the zahlen_warnings interface target
# HostToolRuntime   - build-time rpath for tools the build itself runs (zshader, zcook)
# GitRevision       - ZHLN_GIT_HASH from git describe/rev-parse
# LinkerConfig      - mold/lld/gold detection, -rdynamic, ZHLN_TARGET_TRIPLE
# Sanitizers        - USE_SANITIZERS flags, CTest environment, ASan runtime copy
# VulkanSandbox     - opt-in hermetic lavapipe environment for the CPU test groups
# GovernanceChecks  - the configure/ Python boundary checks, one call each
# ReflectionConfig  - -freflection detection, transpiler fallback, target helpers
#
# ThirdParty (the extern/ add_subdirectory calls) is included AFTER the options
# below on purpose: the vendor projects consume the global flags, so the
# "before any add_subdirectory" rule above applies to it as well.
include(CompilerWarnings)
include(HostToolRuntime)
include(GitRevision)
include(LinkerConfig)
include(Sanitizers)
include(VulkanSandbox)
include(GovernanceChecks)
# VkResultMirror runs configure/generate_vk_result.py, which emits Vk::Result into
# the build tree. It needs Python3 from GovernanceChecks above, and must run
# before add_subdirectory(src/vulkan) consumes ZHLN_VKRESULT_GENERATED_DIR below.
include(VkResultMirror)
include(ReflectionConfig)

# --- ENVIRONMENT & OPTIONS ---
option(ZHLN_DEV_MODE "Enable raw loose-file asset loading for development" ON)
if(ZHLN_DEV_MODE)
    add_compile_definitions(ZHLN_DEV_MODE)
endif()

# Absolute project root (forward slashes also on Windows) for code that reads
# or writes repo-root files at runtime, e.g. the performance baseline cache
# (tests/extras/profile/PerfBaseline.hpp). Defined once here so every target shares it
# instead of each consumer inventing its own path plumbing.
string(REPLACE "\\" "/" ZHLN_PROJECT_ROOT_FWD "${PROJECT_SOURCE_DIR}")
add_compile_definitions(ZHLN_PROJECT_ROOT="${ZHLN_PROJECT_ROOT_FWD}")

option(ZHLN_IN_DOCKER "Build running inside a Docker container" OFF)
if(ZHLN_IN_DOCKER)
    add_compile_definitions(ZHLN_IN_DOCKER=1)
    message(STATUS "Docker container build mode enabled.")
endif()

# Workaround for Clang P2996 revisions where copying a reflected
# annotation value fails constant evaluation ('read of object outside
# its lifetime' in libc++'s optional, observed on macOS/arm64): error
# messages fall back to plain enumerator names.
option(ZHLN_NO_ANNOTATION_EXTRACT "Disable compile-time reflection annotation extraction" OFF)
if(ZHLN_NO_ANNOTATION_EXTRACT)
    add_compile_definitions(ZHLN_NO_ANNOTATION_EXTRACT)
endif()

option(ZHLN_BUILD_TESTS "Build engine test suite" ON)
option(ZHLN_BUILD_SAMPLES "Build engine sample applications" ON)
option(ZHLN_BUILD_EXTRAS "Build first-party engine feature modules and VFX toolkits" ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(ZHLN_DEBUG=1)
endif()

# --- THIRD-PARTY LIBRARIES (extern/) ---
include(ThirdParty)

# --- CORE SUBSYSTEMS ---
# The windowing subsystem is above the RHI on purpose: src/vulkan reads the
# presentation bridge's private header (src/window/NativeSurfaceInternal.hpp) to
# turn a published native handle into a VkSurfaceKHR, so the bridge has to exist
# before the backend that consumes it is configured.
add_subdirectory(src/window)
add_subdirectory(src/vulkan)
add_subdirectory(src/threading)
add_subdirectory(src/filesystem)
add_subdirectory(src/physics)
add_subdirectory(src/ecs)
add_subdirectory(src/audio)
add_subdirectory(src/gui)

# --- ASSEMBLE COMPILER FLAGS METADATA ---
set(COMPILER_FLAGS_LIST "${CMAKE_CXX_FLAGS}")
if(CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" UPPER_BUILD_TYPE)
    set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} ${CMAKE_CXX_FLAGS_${UPPER_BUILD_TYPE}}")
else()
    set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} ${CMAKE_CXX_FLAGS_DEBUG}")
endif()

set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} ${ZHLN_COMPILE_WARNINGS_STR} -fno-exceptions -fno-rtti")

if(COMPILER_HAS_REFLECTION)
    set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} -freflection")
endif()

string(STRIP "${COMPILER_FLAGS_LIST}" COMPILER_FLAGS_LIST)
string(REGEX REPLACE " +" " " COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST}")

# --- ENGINE LIBRARY ---
add_library(zahlen_engine SHARED)

# Engine implementation and renderer each own their source lists and attach to
# zahlen_engine. Keep the Vulkan backend above so these layers can consume its
# public headers.
add_subdirectory(src/engine)

# Shader compilation runs BEFORE src/render: Resources.cpp embeds the cooked
# SPIR-V through '#embed SHADER_*_PATH' macros that are applied as
# source-file properties, and those only reach the zahlen_render target when
# they are set in its home directory (see src/render/CMakeLists.txt).
include(ShaderCompilation)

add_subdirectory(src/render)

# The RHI (and Volk inside it) must bind to THIS image's symbols:
# executables linking zahlen_vulkan's static archive alongside the engine
# (e.g. the extras GPU tests via the extras targets) embed their own copies of
# volkInitialize & friends, and those preempt the engine's PLT calls --
# leaving the engine's per-image dispatch table NULL after a "successful"
# ZHLN_EnsureVulkanLoader(). The version script localizes the RHI's
# symbols in this .so so they bind at link time. See cmake/zahlen_engine.map.
if(UNIX AND NOT APPLE)
    target_link_options(zahlen_engine PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/zahlen_engine.map"
    )
endif()

set_target_properties(zahlen_engine PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
)

target_compile_definitions(zahlen_engine PRIVATE
    ZHLN_ENGINE_BUILD
    JPH_DOUBLE_PRECISION
    JPH_SHARED_LIBRARY
    ZHLN_GIT_COMMIT_HASH="${ZHLN_GIT_HASH}"
    ZHLN_COMPILER_FLAGS="${COMPILER_FLAGS_LIST}"
    ZHLN_META_BUILDER="CMake ${CMAKE_VERSION}"
    ZHLN_BUILD_TOOL="${CMAKE_GENERATOR}"
    ZHLN_TARGET_TRIPLE="${ZHLN_TARGET_TRIPLE}"
    ZHLN_LINKER_NAME="${ZHLN_LINKER}"
    ZHLN_COMPILED_SCRIPTS_DIR="${CMAKE_BINARY_DIR}"
)

target_sources(zahlen_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/stbi_impl.c
)

# List all module interface files
set(ZHLN_MODULE_FILES
    modules/zahlen-core.cppm
    modules/zahlen-threading.cppm
    modules/zahlen-math.cppm
    modules/zahlen-ecs.cppm
    modules/zahlen-physics.cppm
    modules/zahlen-render.cppm
    modules/zahlen-audio.cppm
    modules/zahlen-scripting.cppm
    modules/zahlen-engine.cppm
    modules/zahlen.cppm
)

# Prevent CMake from injecting PCH before 'module;' on .cppm files
set_source_files_properties(${ZHLN_MODULE_FILES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Register C++20/26 module file set
target_sources(zahlen_engine
    PUBLIC
        FILE_SET CXX_MODULES
        TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules
        FILES ${ZHLN_MODULE_FILES}
)

target_link_libraries(zahlen_engine PRIVATE
    zahlen_render
    zahlen_vulkan
    zahlen_window
    zahlen_physics
    zahlen_threading
    zahlen_filesystem
    zahlen_ecs
    zahlen_audio
    zahlen_gui
    glfw
    spirv_reflect
    mimalloc-static
    ${ZSTD_LIBRARY}
    zahlen_warnings
)

if(APPLE)
    # macOS has no native Vulkan WSI: windowed sessions render offscreen and
    # the HostBlit plugin blits the finished image through its own OpenGL
    # window (PresentationMode::HostBlit). All engine call sites are gated
    # with `if constexpr (isMac)`, so only Apple builds reference the plugin.
    target_link_libraries(zahlen_engine PRIVATE HostBlitSwapchain)
endif()

target_link_libraries(zahlen_engine PUBLIC Jolt)

target_include_directories(zahlen_engine SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/JoltPhysics
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/renderdoc
)
target_include_directories(zahlen_engine SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated_shaders
)
target_include_directories(zahlen_engine SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/clay
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/cgltf
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/stb
)

# The engine carries no src/vulkan include path: the one translation unit that
# needed it (src/engine/WindowSurface.cpp, which implemented
# Window::CreateVulkanSurface) is gone. Window now publishes an opaque
# NativeSurfaceHandle and src/vulkan owns every VkSurfaceKHR in the tree, so
# everything here goes through <Zahlen/Render/Render.hpp>. Do not widen this.

# Module producers and every transitive consumer must agree on these ABI
# modes. Publish them from the engine target so core/extras PCMs, tools,
# samples, and tests cannot drift into incompatible configurations.
if(NOT MSVC)
    target_compile_options(zahlen_engine
        PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
        PRIVATE
            $<$<AND:$<CXX_COMPILER_ID:GNU>,$<COMPILE_LANGUAGE:CXX>>:${GCC_ONLY_FLAGS}>
            $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
    )
    if(WIN32)
        target_link_options(zahlen_engine PRIVATE -Xlinker /subsystem:console)
        target_compile_options(zahlen_engine PRIVATE -Wno-microsoft-anon-tag -Wno-unknown-pragmas -Wno-unused-parameter)
        target_compile_options(zahlen_engine PRIVATE -Wno-microsoft-anon-tag -Wno-unknown-pragmas -Wno-unused-parameter -Wno-microsoft-enum-forward-reference)
        target_compile_options(zahlen_engine PRIVATE -D_CRT_SECURE_NO_WARNINGS)
    endif()
endif()

# GCC carries -freflection on the two most reflection-heavy targets by hand;
# Clang receives the same flag through zahlen_enable_reflection (below).
target_compile_options(zahlen_vulkan PRIVATE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-freflection>
)
target_compile_options(zahlen_render PRIVATE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-freflection>
)

# --- ENGINE LAUNCHER & EDITOR EXECUTABLE ---
# The composition root. It lives outside src/ on purpose: wiring an engine
# together means naming the optional layers it runs with, and src/ is the core
# library, which may not know extras exist. See configure/check_core_extras_boundary.py.
add_executable(zahlen
    app/main.cpp
)
target_link_libraries(zahlen PRIVATE zahlen_engine Jolt)
target_compile_definitions(zahlen PRIVATE JPH_DOUBLE_PRECISION)

# The composition root wires engine-private platform and editor services. It is
# intentionally the only non-subsystem target that can see src/; core targets
# themselves are restricted to their local implementation directories.
target_include_directories(zahlen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(zahlen PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
)

# v0.1 UI-tree editor: Hierarchy + Design canvas + Inspector over a GUI::UINode.
# A second composition root rather than a mode of `zahlen`, so the world editor
# and the UI builder do not share a process or a command-line flag.
#
# Gated on the extras layer: the editor edits a GUI::UINode, and that document
# schema is extras/UI (zahlen_ui_schema) -- without extras there is no tree to
# edit, the same way a sample is skipped when its domains are not built.
if(ZHLN_BUILD_EXTRAS)
    add_executable(zahlen_ui_editor
        app/UIEditor.cpp
    )
    target_link_libraries(zahlen_ui_editor PRIVATE zahlen_engine Jolt)
    target_compile_definitions(zahlen_ui_editor PRIVATE JPH_DOUBLE_PRECISION)
    set_target_properties(zahlen_ui_editor PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
    )
else()
    message(STATUS "Skipping zahlen_ui_editor: the UI document schema lives in extras (ZHLN_BUILD_EXTRAS=OFF)")
endif()

# --- ASSET COMPILERS & HELPERS ---
include(ECSCompiler)
include(FennelCompiler)
include(AssetPipeline)

# If building engine standalone (not as a submodule), cook local engine assets
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    zahlen_configure_game_assets("${CMAKE_CURRENT_SOURCE_DIR}")
endif()

add_custom_target(link_resources ALL
    COMMAND ${CMAKE_COMMAND}
        -D SRC="${CMAKE_CURRENT_SOURCE_DIR}/resources"
        -D DST="${CMAKE_BINARY_DIR}/resources"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CreateSymlinks.cmake"
    COMMENT "Creating zero-copy symlinks for resources..."
)

add_dependencies(zahlen link_resources)
if(TARGET zahlen_ui_editor)
    add_dependencies(zahlen_ui_editor link_resources)
endif()

if(USE_SANITIZERS)
    zahlen_install_sanitizer_runtime(zahlen)
endif()

# --- APPLY TARGET-SPECIFIC REFLECTION FLAGS ---
# Reflection is not a per-target feature: it changes struct layout.
# InputStateComponent's first member is std::bitset<Reflect::EnumCount<KeyCode>()>,
# and EnumCount is 72 with P2996 and 0 in the fallback spelling -- so a target
# compiled without these flags lays mouseX out at offset 4 instead of 16 and
# silently reads another field. Every target that includes a header whose layout
# depends on Reflect has to be in this list. zahlen_gui was missing, which is
# what made Context::Button read the key bitset as the mouse position.
zahlen_enable_reflection(zahlen_window)
zahlen_enable_reflection(zahlen_vulkan)
zahlen_enable_reflection(zahlen_render)
zahlen_enable_reflection(zahlen_engine)
zahlen_enable_reflection(zahlen_audio)
zahlen_enable_reflection(zahlen_physics)
zahlen_enable_reflection(zahlen_ecs)
zahlen_enable_reflection(zahlen_gui)
zahlen_enable_reflection(zahlen_threading)
zahlen_enable_reflection(zahlen_filesystem)
zahlen_enable_reflection(zahlen)
zahlen_enable_reflection(zahlen_ui_editor)
zahlen_enable_reflection(zcook)

# zshader is on this list for the same reason as everything else on it: it
# includes a reflection header. Reflect.cpp names descriptor types and stages by
# asking Reflect::EnumToString about them, so without -freflection it does not
# compile at all (Core.hpp's static_assert) -- and it cannot get the flags from
# zahlen_engine, because the tool cannot link the engine that runs this far into
# the build: zahlen_engine -> zahlen_render -> the catalog zshader writes.
zahlen_enable_reflection(zshader)

zahlen_enable_reflection(HostBlitSwapchain)

# --- EXTRAS ---
if(ZHLN_BUILD_EXTRAS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extras")
    add_subdirectory(extras)

    # The engine itself has no scripting; the composition root installs a runtime
    # if one was built. Without it ScriptRunner forwards to nothing and the
    # engine runs C++-only.
    if(TARGET zahlen_scripting_lua)
        target_link_libraries(zahlen PRIVATE zahlen_scripting_lua)
        target_compile_definitions(zahlen PRIVATE ZHLN_HAS_SCRIPTING=1)
    endif()

    # The native world editor is an extra. Without it --editor fails at
    # runtime (app/main.cpp) rather than a missing-header compile error.
    if(TARGET zahlen_editor)
        target_link_libraries(zahlen PRIVATE zahlen_editor)
        target_compile_definitions(zahlen PRIVATE ZHLN_HAS_EDITOR=1)
    endif()

    # Ctrl+S in the editor writes the world back out as a scene document
    # (Scene::Extract + extras/toml). Core may not reach for extras, so the
    # composition root is what links the document layer, and app/main.cpp guards
    # its include on the definition below: with ZHLN_BUILD_EXTRAS=OFF the editor
    # still extracts a scene, it just has nothing to serialise it through.
    #
    # zahlen_serialization, not the zahlen_extras aggregate: TOML is the only
    # extras domain either composition root touches (app/main.cpp includes
    # <toml/SceneTOML.hpp> and <toml/TOML.hpp>, app/UIEditor.cpp includes
    # <toml/UITOML.hpp>, and that is the whole of it). Linking the aggregate here
    # would not have pulled the other domains' objects into the executables --
    # a static archive only contributes members a consumer references -- but it
    # would have made their module interfaces a build-order dependency of both,
    # so the app could not build without the wire protocol, the VFX toolkits and
    # the glTF importer.
    if(TARGET zahlen_serialization)
        target_link_libraries(zahlen PRIVATE zahlen_serialization)
        target_compile_definitions(zahlen PRIVATE ZHLN_HAS_SCENE_TOML=1)
        target_link_libraries(zahlen_ui_editor PRIVATE zahlen_serialization)
        target_compile_definitions(zahlen_ui_editor PRIVATE ZHLN_HAS_UI_TOML=1)
        # UIEditor.cpp builds, renders and mutates a GUI::UINode; the schema
        # also reaches it through UITOML.hpp, but the direct use is what the
        # link is for.
        if(TARGET zahlen_ui_schema)
            target_link_libraries(zahlen_ui_editor PRIVATE zahlen_ui_schema)
        endif()
    endif()

    # Gameplay layers moved out of core: the composition root names the ones
    # the native host runs with and installs them after Engine::Create (see
    # InstallGameplayExtras in app/main.cpp). Core itself never links these --
    # configure/check_core_extras_boundary.py keeps it that way.
    if(TARGET zahlen_character_controller)
        target_link_libraries(zahlen PRIVATE zahlen_character_controller)
        target_compile_definitions(zahlen PRIVATE ZHLN_HAS_CHARACTER_CONTROLLER=1)
    endif()
    # The third-person target camera rig: installing it re-seeds the boot
    # camera's component and re-inserts the rig's frame step before the core
    # CameraSystems step (core only provides the seam and the plain camera).
    if(TARGET zahlen_camera)
        target_link_libraries(zahlen PRIVATE zahlen_camera)
        target_compile_definitions(zahlen PRIVATE ZHLN_HAS_CAMERA=1)
    endif()
    if(TARGET zahlen_interaction)
        target_link_libraries(zahlen PRIVATE zahlen_interaction)
        target_compile_definitions(zahlen PRIVATE ZHLN_HAS_INTERACTION=1)
    endif()
    if(TARGET zahlen_terrain)
        target_link_libraries(zahlen PRIVATE zahlen_terrain)
        target_compile_definitions(zahlen PRIVATE ZHLN_HAS_TERRAIN=1)
    endif()
    # The animation-modifier toolkit: installing its two-bone IK
    # post-processor is what makes TwoBoneIKComponent chains take effect in
    # the skinning pass (core only calls the hook).
    if(TARGET zahlen_animation)
        target_link_libraries(zahlen PRIVATE zahlen_animation)
        target_compile_definitions(zahlen PRIVATE ZHLN_HAS_ANIMATION=1)
    endif()
    # The compiled-in fail-safe scene: installing it re-inserts the fallback
    # frame step and subscribes its teardown hook (core only provides the
    # seams and the enableFallbackScene flag).
    if(TARGET zahlen_fallback_scene)
        target_link_libraries(zahlen PRIVATE zahlen_fallback_scene)
        target_compile_definitions(zahlen PRIVATE ZHLN_HAS_FALLBACK_SCENE=1)
    endif()

    # The production baked-font path: installing its loader hook makes fontbm
    # `.fnt`+`.png` bakes (or a pak's cooked `.zfont`) the atlas core
    # materialises, instead of the embedded zero-asset default. With
    # ZHLN_BUILD_EXTRAS=OFF the hook is never installed and the engine
    # runs on the embedded default only -- core never parses a TTF at runtime.
    if(TARGET zahlen_fonts)
        target_link_libraries(zahlen PRIVATE zahlen_fonts)
        target_compile_definitions(zahlen PRIVATE ZHLN_HAS_FONTS=1)
        target_link_libraries(zahlen_ui_editor PRIVATE zahlen_fonts)
        target_compile_definitions(zahlen_ui_editor PRIVATE ZHLN_HAS_FONTS=1)
    endif()
endif()

# --- TESTS ---
if(ZHLN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- SAMPLES ---
if(ZHLN_BUILD_SAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples")
    add_subdirectory(samples)
endif()

# --- HOST TOOLS THAT NEED EXTRAS ---
# The fidelity runner's compare/list/report tool links zahlen_serialization
# (extras/json), so it follows the samples rule: built only when that domain
# was built. It has no GPU or window, so it is cheap to build and always-on in
# an extras build.
if(TARGET zahlen_serialization AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/fidelity")
    # Binary dir is the source-relative path so the executable lands at
    # build/tools/fidelity/fidelity, where scripts/run_fidelity.sh looks first
    # (and where it does not collide with the build/fidelity/ scratch clone
    # directory the driver uses for the Khronos repos).
    add_subdirectory(tools/fidelity tools/fidelity)
endif()
