# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

project(zahlen LANGUAGES C CXX ASM)

if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  message(
    FATAL_ERROR
      "zahlen requires a GNU-style toolchain (gcc/g++, or clang/clang++ with "
      "the GNU driver). MSVC and clang-cl cannot build C++26 static "
      "reflection; reconfigure with -DCMAKE_C_COMPILER=clang "
      "-DCMAKE_CXX_COMPILER=clang++.")
endif()

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-Qunused-arguments" ZHLN_HAS_QUNUSED_ARGUMENTS)
check_cxx_compiler_flag("-fblocks" ZHLN_HAS_FBLOCKS)
check_cxx_compiler_flag("-fannotation-attributes"
                        ZHLN_HAS_ANNOTATION_ATTRIBUTES)
check_cxx_compiler_flag("-stdlib=libc++" ZHLN_HAS_STDLIB_LIBCXX)

check_cxx_compiler_flag("-Wc23-extensions" ZHLN_HAS_WC23_EXTENSIONS)

include(CheckSymbolExists)
set(CMAKE_REQUIRED_LIBRARIES ws2_32)
check_symbol_exists(WSAStartup "winsock2.h" ZHLN_HAVE_WINSOCK)
unset(CMAKE_REQUIRED_LIBRARIES)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      "Release"
      CACHE STRING "Choose the type of build." FORCE)
endif()

if(ZHLN_HAS_QUNUSED_ARGUMENTS)
  add_compile_options(-Qunused-arguments)
  add_link_options(-Qunused-arguments)
endif()

if(NOT CMAKE_CXX_FLAGS_RELEASE MATCHES "(^| )-g")
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g")
endif()
if(NOT CMAKE_C_FLAGS_RELEASE MATCHES "(^| )-g")
  set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -g")
endif()

if(UNIX OR APPLE)
  set(CMAKE_ASM_COMPILE_OBJECT
      "<CMAKE_ASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> -c <SOURCE>"
      CACHE STRING "ASM compile command" FORCE)
endif()

if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fno-rtti -fno-exceptions")
  execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE MACOS_SDK_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(CMAKE_SYSROOT ${MACOS_SDK_PATH})
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT WIN32)
  file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
       "${CMAKE_SOURCE_DIR}/compile_commands.json" SYMBOLIC
       RESULT _zhln_compdb_link)
  if(_zhln_compdb_link)
    message(
      STATUS
        "clangd: could not symlink compile_commands.json (${_zhln_compdb_link})"
    )
  endif()
endif()

include(CompilerWarnings)
include(HostToolRuntime)
include(GitRevision)
include(LinkerConfig)
include(Sanitizers)
include(VulkanSandbox)
include(GovernanceChecks)
include(ReflectionConfig)

option(ZHLN_DEV_MODE "Enable raw loose-file asset loading for development" ON)
if(ZHLN_DEV_MODE)
  add_compile_definitions(ZHLN_DEV_MODE)
endif()

string(REPLACE "\\" "/" ZHLN_PROJECT_ROOT_FWD "${PROJECT_SOURCE_DIR}")
add_compile_definitions(ZHLN_PROJECT_ROOT="${ZHLN_PROJECT_ROOT_FWD}")

option(ZHLN_IN_DOCKER "Build running inside a Docker container" OFF)
if(ZHLN_IN_DOCKER)
  add_compile_definitions(ZHLN_IN_DOCKER=1)
  message(STATUS "Docker container build mode enabled.")
endif()

option(ZHLN_NO_ANNOTATION_EXTRACT
       "Disable compile-time reflection annotation extraction" OFF)
if(ZHLN_NO_ANNOTATION_EXTRACT)
  add_compile_definitions(ZHLN_NO_ANNOTATION_EXTRACT)
endif()

option(ZHLN_GRAPH_VISUALIZE
       "Compile the render-graph dump behind --print-graph into the engine" OFF)

option(ZHLN_BUILD_TESTS "Build engine test suite" ON)
option(ZHLN_BUILD_SAMPLES "Build engine sample applications" ON)
option(ZHLN_BUILD_EXTRAS
       "Build first-party engine feature modules and VFX toolkits" ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_definitions(ZHLN_DEBUG=1)
endif()

include(ThirdParty)

add_subdirectory(src/window)
add_subdirectory(src/vulkan)
add_subdirectory(src/threading)
add_subdirectory(src/filesystem)
add_subdirectory(src/physics)
add_subdirectory(src/ecs)
add_subdirectory(src/audio)
add_subdirectory(src/gui)

set(COMPILER_FLAGS_LIST "${CMAKE_CXX_FLAGS}")
if(CMAKE_BUILD_TYPE)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" UPPER_BUILD_TYPE)
  set(COMPILER_FLAGS_LIST
      "${COMPILER_FLAGS_LIST} ${CMAKE_CXX_FLAGS_${UPPER_BUILD_TYPE}}")
else()
  set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} ${CMAKE_CXX_FLAGS_DEBUG}")
endif()

set(COMPILER_FLAGS_LIST
    "${COMPILER_FLAGS_LIST} ${ZHLN_COMPILE_WARNINGS_STR} -fno-exceptions -fno-rtti"
)

if(COMPILER_HAS_REFLECTION)
  set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} -freflection")
endif()
if(COMPILER_HAS_PARAMETER_REFLECTION_FLAG)
  set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} -fparameter-reflection")
elseif(COMPILER_HAS_REFLECTION_LATEST_FLAG)
  set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} -freflection-latest")
endif()

string(STRIP "${COMPILER_FLAGS_LIST}" COMPILER_FLAGS_LIST)
string(REGEX REPLACE " +" " " COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST}")

add_library(zahlen_engine SHARED)

add_subdirectory(src/engine)

include(ShaderCompilation)

add_subdirectory(src/render)

if(UNIX AND NOT APPLE)
  target_link_options(
    zahlen_engine PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/zahlen_engine.map")
endif()

if(MINGW)
  # PE side of the question the version script above answers for ELF: what does
  # the engine's shared library export?
  #
  # On ELF the map says `global: *` and localizes the renderer's internals only,
  # an exception that exists for symbol preemption -- something PE does not have,
  # since a DLL binds intra-image by default. What PE does have is a much
  # narrower export set: GNU ld stops auto-exporting as soon as one object carries
  # __declspec(dllexport), which every ZHLN_API declaration does. Only the
  # annotated public API would be exported, and the engine's own consumers call
  # more than that on purpose -- the header-inline Panic<> reaches InternalPanic,
  # the ECS Registry holds a Mutex whose slow paths are out of line,
  # EntityCommandBuffer::Playback, Fiber and TaskSystem are non-public by design
  # and all reachable from a test. They link on Linux because the .so exports
  # everything; without this flag the Windows test executables fail with
  # `undefined reference to ZHLN::Mutex::LockSlow()`.
  #
  # Exporting the whole image restores the ELF behaviour. The DLL is a private
  # build artifact and its import library is what consumers link, so the wider
  # export table costs nothing.
  target_link_options(zahlen_engine PRIVATE -Wl,--export-all-symbols)
endif()

set_target_properties(zahlen_engine PROPERTIES CXX_STANDARD 26
                                               CXX_STANDARD_REQUIRED ON)

target_compile_definitions(
  zahlen_engine
  PRIVATE ZHLN_ENGINE_BUILD
          JPH_DOUBLE_PRECISION
          JPH_SHARED_LIBRARY
          ZHLN_GIT_COMMIT_HASH="${ZHLN_GIT_HASH}"
          ZHLN_COMPILER_FLAGS="${COMPILER_FLAGS_LIST}"
          ZHLN_META_BUILDER="CMake ${CMAKE_VERSION}"
          ZHLN_BUILD_TOOL="${CMAKE_GENERATOR}"
          ZHLN_TARGET_TRIPLE="${ZHLN_TARGET_TRIPLE}"
          ZHLN_LINKER_NAME="${ZHLN_LINKER}"
          ZHLN_COMPILED_SCRIPTS_DIR="${CMAKE_BINARY_DIR}")

target_sources(zahlen_engine
               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/stbi_impl.c)

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
    modules/zahlen.cppm)

set_source_files_properties(${ZHLN_MODULE_FILES}
                            PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_sources(
  zahlen_engine
  PUBLIC FILE_SET
         CXX_MODULES
         TYPE
         CXX_MODULES
         BASE_DIRS
         ${CMAKE_CURRENT_SOURCE_DIR}/modules
         FILES
         ${ZHLN_MODULE_FILES})

target_link_libraries(
  zahlen_engine
  PRIVATE zahlen_render
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
          zahlen_warnings)

# Every target whose objects are linked into libzahlen_engine compiles the
# engine's own public headers, so all of them must see ZHLN_API as dllexport.
# On Windows Common.h maps ZHLN_API to dllimport without ZHLN_ENGINE_BUILD, and
# then a call between two of these objects -- say EntityCommandBuffer.cpp in
# zahlen_ecs calling ZHLN::ECS::Registry::Create defined in ECS.cpp in the same
# target -- is emitted as an import of __imp__ZN4ZHLN3ECS8Registry6CreateEv:
# a reference the DLL was always meant to satisfy from its own text, which
# MinGW ld reports as an undefined reference while linking it. The visibility
# macro is ignored outside Windows (Common.h uses gnu::visibility), so this
# changes nothing on the other platforms.
foreach(ZHLN_ENGINE_LIB_TARGET
        zahlen_render  zahlen_vulkan  zahlen_window   zahlen_physics
        zahlen_threading  zahlen_filesystem  zahlen_ecs  zahlen_audio
        zahlen_gui)
  target_compile_definitions(${ZHLN_ENGINE_LIB_TARGET} PRIVATE ZHLN_ENGINE_BUILD)
endforeach()

# Windows: the MinGW link surface the engine's own sources need.
#
#   stdc++exp  <bits/print.h>'s vprint_unicode calls std::__open_terminal and
#              std::__write_to_terminal when _WIN32 is defined (the POSIX branch
#              delegates to vprint_nonunicode instead), and those two definitions
#              live in libstdc++exp, not in libstdc++. PUBLIC because the same
#              header instantiation lands in the consumers' own objects: the
#              engine's Log.cpp, CommandLine.cpp, RenderCore.cpp, Mutex.cpp and
#              RenderDocInit.cpp all print to a FILE*, and so do the tools,
#              tests and samples that link this library. tools/zshader carries
#              the same expression for libslang's in-process passes.
#   dbghelp    the symbol resolver in src/engine/diagnostics/StackTrace.cpp; its
#              #pragma comment(lib, "dbghelp.lib") is MSVC-only, so MinGW needs
#              the import library named.
#   atomic     std::atomic<ZHLN::Vk::DiagnosticsSink> is a 16-byte object, and
#              GCC calls libatomic's __atomic_load_16/__atomic_store_16 for it
#              unless -mcx16 is on -- it is not, this tree asks for -mavx2 and
#              friends rather than an ISA level.
target_link_libraries(
  zahlen_engine
  PUBLIC $<$<AND:$<BOOL:${MINGW}>,$<CXX_COMPILER_ID:GNU>>:stdc++exp>)
if(MINGW)
  target_link_libraries(zahlen_engine PRIVATE dbghelp atomic)
endif()

if(APPLE)
  target_link_libraries(zahlen_engine PRIVATE HostBlitSwapchain)
endif()

target_link_libraries(zahlen_engine PUBLIC Jolt)

target_include_directories(
  zahlen_engine SYSTEM
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/extern/JoltPhysics
          ${CMAKE_CURRENT_SOURCE_DIR}/third_party/renderdoc)
target_include_directories(
  zahlen_engine SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                              ${CMAKE_BINARY_DIR}/generated_shaders)
target_include_directories(
  zahlen_engine SYSTEM
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/extern/clay
          ${CMAKE_CURRENT_SOURCE_DIR}/extern/cgltf
          ${CMAKE_CURRENT_SOURCE_DIR}/extern/stb)

target_compile_options(
  zahlen_engine
  PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
  PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>)
if(WIN32)
  # -Wno-microsoft-anon-tag and -Wno-microsoft-enum-forward-reference are clang
  # diagnostics and now come from the probe in cmake/CompilerWarnings.cmake, so a
  # GNU driver is not handed flags it does not know. -Wno-unused-parameter is
  # already probed there too, for every target that links zahlen_warnings.
  target_compile_options(zahlen_engine PRIVATE -Wno-unknown-pragmas -D_CRT_SECURE_NO_WARNINGS)
endif()

add_executable(zahlen app/main.cpp)
target_link_libraries(zahlen PRIVATE zahlen_engine Jolt)
target_compile_definitions(zahlen PRIVATE JPH_DOUBLE_PRECISION)

# The engine's DLL is written next to it; Jolt's is not, and PE has no rpath to
# find it with. This is the binary a person runs, so the DLLs go beside it.
zahlen_add_runtime_dlls(zahlen)

target_include_directories(zahlen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(zahlen PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED
                                                        ON)

if(ZHLN_BUILD_EXTRAS)
  add_executable(zahlen_ui_editor app/UIEditor.cpp)
  target_link_libraries(zahlen_ui_editor PRIVATE zahlen_engine Jolt)
  target_compile_definitions(zahlen_ui_editor PRIVATE JPH_DOUBLE_PRECISION)

  zahlen_add_runtime_dlls(zahlen_ui_editor)
  set_target_properties(zahlen_ui_editor PROPERTIES CXX_STANDARD 26
                                                    CXX_STANDARD_REQUIRED ON)
else()
  message(
    STATUS
      "Skipping zahlen_ui_editor: the UI document schema lives in extras (ZHLN_BUILD_EXTRAS=OFF)"
  )
endif()

include(ECSCompiler)
include(FennelCompiler)
include(AssetPipeline)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  zahlen_configure_game_assets("${CMAKE_CURRENT_SOURCE_DIR}")
endif()

add_custom_target(
  link_resources ALL
  COMMAND
    ${CMAKE_COMMAND} -D SRC="${CMAKE_CURRENT_SOURCE_DIR}/resources" -D
    DST="${CMAKE_BINARY_DIR}/resources" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CreateSymlinks.cmake"
  COMMENT "Creating zero-copy symlinks for resources...")

add_dependencies(zahlen link_resources)
if(TARGET zahlen_ui_editor)
  add_dependencies(zahlen_ui_editor link_resources)
endif()

if(USE_SANITIZERS)
  zahlen_install_sanitizer_runtime(zahlen)
endif()

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

zahlen_enable_reflection(zshader)

zahlen_enable_reflection(HostBlitSwapchain)

if(ZHLN_BUILD_EXTRAS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extras")
  add_subdirectory(extras)

  if(TARGET zahlen_scripting_lua)
    target_link_libraries(zahlen PRIVATE zahlen_scripting_lua)
    target_compile_definitions(zahlen PRIVATE ZHLN_HAS_SCRIPTING=1)
  endif()

  if(TARGET zahlen_editor)
    target_link_libraries(zahlen PRIVATE zahlen_editor)
    target_compile_definitions(zahlen PRIVATE ZHLN_HAS_EDITOR=1)
  endif()

  if(TARGET zahlen_serialization)
    target_link_libraries(zahlen PRIVATE zahlen_serialization)
    target_compile_definitions(zahlen PRIVATE ZHLN_HAS_SCENE_TOML=1)
    target_link_libraries(zahlen_ui_editor PRIVATE zahlen_serialization)
    target_compile_definitions(zahlen_ui_editor PRIVATE ZHLN_HAS_UI_TOML=1)
    if(TARGET zahlen_ui_schema)
      target_link_libraries(zahlen_ui_editor PRIVATE zahlen_ui_schema)
    endif()
  endif()

  if(TARGET zahlen_character_controller)
    target_link_libraries(zahlen PRIVATE zahlen_character_controller)
    target_compile_definitions(zahlen PRIVATE ZHLN_HAS_CHARACTER_CONTROLLER=1)
  endif()
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
  if(TARGET zahlen_animation)
    target_link_libraries(zahlen PRIVATE zahlen_animation)
    target_compile_definitions(zahlen PRIVATE ZHLN_HAS_ANIMATION=1)
  endif()
  if(TARGET zahlen_fallback_scene)
    target_link_libraries(zahlen PRIVATE zahlen_fallback_scene)
    target_compile_definitions(zahlen PRIVATE ZHLN_HAS_FALLBACK_SCENE=1)
  endif()

  if(TARGET zahlen_fonts)
    target_link_libraries(zahlen PRIVATE zahlen_fonts)
    target_compile_definitions(zahlen PRIVATE ZHLN_HAS_FONTS=1)
    target_link_libraries(zahlen_ui_editor PRIVATE zahlen_fonts)
    target_compile_definitions(zahlen_ui_editor PRIVATE ZHLN_HAS_FONTS=1)
  endif()
  # Keep the default Zahlen composition root untouched. The project-light
  # bridge gets its own opt-in application entrypoint and CLI/config lifecycle.
  if(TARGET zahlen_project_light_client)
    add_executable(zahlen_project_light_app EXCLUDE_FROM_ALL app/ProjectLightClientMain.cpp)
    set_target_properties(zahlen_project_light_app PROPERTIES
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF
      OUTPUT_NAME "project-light-client"
    )
    target_compile_definitions(zahlen_project_light_app PRIVATE JPH_DOUBLE_PRECISION)
    target_link_libraries(zahlen_project_light_app PRIVATE
      zahlen_engine
      Jolt
      zahlen_project_light_client
      zahlen_character_controller
      zahlen_camera
      zahlen_warnings
    )
    add_dependencies(zahlen_project_light_app link_resources)
    zahlen_add_runtime_dlls(zahlen_project_light_app)
    zahlen_enable_reflection(zahlen_project_light_app)
    if(USE_SANITIZERS)
      zahlen_install_sanitizer_runtime(zahlen_project_light_app)
    endif()
  endif()
endif()

if(ZHLN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ZHLN_BUILD_SAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples")
  add_subdirectory(samples)
endif()

if(TARGET zahlen_serialization AND EXISTS
                                   "${CMAKE_CURRENT_SOURCE_DIR}/tools/fidelity")
  add_subdirectory(tools/fidelity tools/fidelity)
endif()
