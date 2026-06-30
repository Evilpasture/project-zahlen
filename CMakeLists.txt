# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.25)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(AppleShim)

# 1. Force the compiler selection BEFORE project()
if(WIN32 AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
endif()

project(zahlen LANGUAGES C CXX ASM)

# --- RETRIEVE GIT COMMIT HASH ---
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE ZHLN_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

if(NOT ZHLN_GIT_HASH)
    set(ZHLN_GIT_HASH "unknown")
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

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- DETECT C++26 REFLECTION SUPPORT ---
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-freflection" COMPILER_HAS_REFLECTION)

if(COMPILER_HAS_REFLECTION)
    message(STATUS "C++26 Static Reflection supported via -freflection")
else()
    message(STATUS "C++26 Static Reflection not supported by current compiler. Using manual macro fallback.")
endif()

execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
    OUTPUT_VARIABLE ZHLN_TARGET_TRIPLE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT ZHLN_TARGET_TRIPLE)
    set(ZHLN_TARGET_TRIPLE "${CMAKE_SYSTEM_PROCESSOR}-unknown-${CMAKE_SYSTEM_NAME}")
    string(TOLOWER "${ZHLN_TARGET_TRIPLE}" ZHLN_TARGET_TRIPLE)
endif()

if(NOT MSVC)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=mold -Wl,--version ERROR_QUIET OUTPUT_VARIABLE MOLD_VERSION)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=lld -Wl,--version ERROR_QUIET OUTPUT_VARIABLE LLD_VERSION)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=gold -Wl,--version ERROR_QUIET OUTPUT_VARIABLE GOLD_VERSION)

    if(MOLD_VERSION MATCHES "mold")
        message(STATUS "Using MOLD linker")
        add_link_options(-fuse-ld=mold)
        set(ZHLN_LINKER "mold")
    elseif(LLD_VERSION MATCHES "LLD")
        message(STATUS "Using LLD linker")
        add_link_options(-fuse-ld=lld)
        set(ZHLN_LINKER "lld")
    elseif(GOLD_VERSION MATCHES "GNU gold")
        message(STATUS "Using GOLD linker")
        add_link_options(-fuse-ld=gold)
        set(ZHLN_LINKER "gold")
    else()
        message(STATUS "Using default BFD linker")
        set(ZHLN_LINKER "bfd")
    endif()
    if(UNIX AND NOT APPLE)
        add_link_options(-rdynamic)
    endif()
else()
    set(ZHLN_LINKER "link.exe")
endif()

option(USE_SANITIZERS "Enable ASan and UBSan" OFF)
if(USE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    add_definitions(-D__ASAN_ENABLED__)
endif()

option(ZHLN_DEV_MODE "Enable raw loose-file asset loading for development" ON)
if(ZHLN_DEV_MODE)
    add_compile_definitions(ZHLN_DEV_MODE)
endif()

set(MI_OVERRIDE ON CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE) # Build static lib for easy embedding
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/mimalloc EXCLUDE_FROM_ALL)



set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/glfw SYSTEM)

add_subdirectory(${CMAKE_SOURCE_DIR}/extern/LuaJIT SYSTEM)

set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
set(USE_STD_INCLUDES ON CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS ON CACHE BOOL "Build Jolt as shared library" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/JoltPhysics/Build SYSTEM)

add_subdirectory(${CMAKE_SOURCE_DIR}/extern/VulkanMemoryAllocator SYSTEM)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/SPIRV-Reflect)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/imgui)
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_subdirectory(${CMAKE_SOURCE_DIR}/src/render)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/threading)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/physics)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/ecs)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/alife)
add_subdirectory(${CMAKE_SOURCE_DIR}/src/audio)

option(BUILD_TESTING "Build the test suite" OFF)

if(BUILD_TESTING)
    message(STATUS "Building the test suites...")
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(zahlen_ssr_test src/ssr_test.cpp)
    target_link_libraries(zahlen_ssr_test PRIVATE GTest::gtest_main Jolt)
    target_include_directories(zahlen_ssr_test PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_definitions(zahlen_ssr_test PRIVATE JPH_DOUBLE_PRECISION)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(zahlen_ssr_test PRIVATE -Wno-attributes)
    endif()
    add_test(NAME ssr_cpu_test COMMAND zahlen_ssr_test)
endif()

set(ENGINE_SOURCES
    src/engine/AssertHandler.cpp
    src/engine/RenderFrame.cpp
    src/engine/RenderInit.cpp
    src/engine/RenderResources.cpp
    src/engine/Engine.cpp
    src/engine/AssetFactory.cpp
    src/engine/MeshBuilder.cpp
    src/engine/glTFImporter.cpp
    src/engine/system/AnimationSystem.cpp
    src/engine/AssetManager.cpp
    src/engine/Window.cpp
    src/engine/Input.cpp
    src/engine/Resources.c
    src/engine/Platform.cpp
    src/engine/Scripting.cpp
    src/engine/LuaScriptRuntime.cpp
    src/engine/ConsoleUI.cpp
    src/engine/ProfilerUI.cpp
    src/engine/DiagnosticUI.cpp
    src/engine/Text.cpp
    src/engine/system/RenderSystem.cpp
    src/engine/system/MovementSystem.cpp
    src/engine/system/AudioSystem.cpp
    src/engine/system/ArticulationSystem.cpp
    src/engine/system/CullingSystem.cpp
    src/engine/Sky.cpp
    src/engine/BRDFLUT.cpp
    src/engine/GizmoUI.cpp
    src/engine/CommandLine.cpp
    src/engine/system/TargetCameraSystem.cpp
    src/engine/TTYBackend.cpp
    src/engine/system/TransformSystem.cpp
    src/engine/system/PhysicsStateSystem.cpp
    src/engine/system/LightingSystem.cpp
    src/engine/system/InputSystem.cpp
    src/engine/system/CameraSystem.cpp
    src/engine/RenderDocInit.cpp
    src/engine/system/InteractionSystem.cpp
    src/engine/system/UIRenderSystem.cpp
    src/engine/system/UIInteractionSystem.cpp
    src/engine/RenderProcedural.cpp
    src/engine/RenderPasses.cpp
    src/engine/RenderSetup.cpp
)

find_library(ZSTD_LIBRARY NAMES zstd zstd_static HINTS "/opt/homebrew/lib" "/usr/local/lib" "/usr/lib")
if(NOT ZSTD_LIBRARY)
    message(FATAL_ERROR "Could not find zstd library. Please install it (e.g., 'brew install zstd').")
endif()

if(APPLE)
    # Ensure CMake searches Homebrew's prefix for packages, headers, and libraries
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

# Find PkgConfig on both Linux and macOS
find_package(PkgConfig REQUIRED)

# Linux-only dependencies
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)
    pkg_check_modules(LIBSEAT REQUIRED IMPORTED_TARGET libseat)
endif()

# Dependencies needed on both Linux and macOS
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
endif()

set(COMPILER_FLAGS_LIST "${CMAKE_CXX_FLAGS}")
if(CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" UPPER_BUILD_TYPE)
    set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} ${CMAKE_CXX_FLAGS_${UPPER_BUILD_TYPE}}")
else()
    set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} ${CMAKE_CXX_FLAGS_DEBUG}")
endif()

if(NOT MSVC)
    set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} --param=destructive-interference-size=64")
    endif()
endif()

string(STRIP "${COMPILER_FLAGS_LIST}" COMPILER_FLAGS_LIST)
string(REGEX REPLACE " +" " " COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST}")

add_library(zahlen_engine SHARED ${ENGINE_SOURCES})

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
    ZHLN_COMPILED_SCRIPTS_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

target_sources(zahlen_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/stbi_impl.c
    ${CMAKE_SOURCE_DIR}/extern/cgltf_impl.c
)

target_link_libraries(zahlen_engine PRIVATE
    zahlen_render
    zahlen_physics
    zahlen_threading
    zahlen_ecs
    zahlen_alife
    zahlen_audio
    glfw
    LuaJIT
    spirv_reflect
    imgui
    mimalloc-static
    ${ZSTD_LIBRARY}
)

# Link Linux-only dependencies
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(zahlen_engine PRIVATE PkgConfig::LIBEVDEV PkgConfig::LIBSEAT)
endif()

# Link Fontconfig on both Linux and macOS
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    target_link_libraries(zahlen_engine PRIVATE PkgConfig::FONTCONFIG)
endif()

target_link_libraries(zahlen_engine PUBLIC Jolt)

target_include_directories(zahlen_engine SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/JoltPhysics
    ${CMAKE_SOURCE_DIR}/third_party/renderdoc
)
target_include_directories(zahlen_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/extern/LuaJIT/src
)
target_include_directories(zahlen_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/cgltf
    ${CMAKE_SOURCE_DIR}/extern/stb
)

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/resources/shaders/SMAA.hlsl")
    message(STATUS "SMAA.hlsl not found. Downloading from official iryoku/smaa repository...")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/iryoku/smaa/master/SMAA.hlsl"
        "${CMAKE_SOURCE_DIR}/resources/shaders/SMAA.hlsl"
        SHOW_PROGRESS
        STATUS DOWNLOAD_STATUS
    )
    list(GET DOWNLOAD_STATUS 0 DOWNLOAD_CODE)
    if(NOT DOWNLOAD_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download SMAA.hlsl: ${DOWNLOAD_STATUS}")
    endif()
endif()

# --- AUTOMATIC FENNEL BOOTSTRAP DOWNLOADER ---
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/scripts/core/fennel.lua")
    message(STATUS "fennel.lua not found. Downloading from official fennel-lang.org...")
    file(DOWNLOAD
        "https://fennel-lang.org/downloads/fennel-1.6.1.lua"
        "${CMAKE_SOURCE_DIR}/scripts/core/fennel.lua"
        SHOW_PROGRESS
        STATUS DOWNLOAD_STATUS
    )
    list(GET DOWNLOAD_STATUS 0 DOWNLOAD_CODE)
    if(NOT DOWNLOAD_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download fennel.lua: ${DOWNLOAD_STATUS}")
    endif()
endif()

function(compile_shaders TARGET_NAME)
    set(SHADER_FILES ${ARGN})

    find_program(DXC_EXECUTABLE NAMES dxc PATHS "$ENV{VULKAN_SDK}/bin" "D:/Vulkan-SDK/1.4.341.1/bin")
    if(NOT DXC_EXECUTABLE)
        message(FATAL_ERROR "DXC not found!")
    endif()

    set(GEN_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
    file(MAKE_DIRECTORY ${GEN_INCLUDE_DIR})

    set(ALL_SPV_OUTPUTS "")
    set(STAGE_EXTS    "VS"      "PS"      "CS")
    set(STAGE_ENTRIES "VSMain"  "PSMain"  "CSMain")
    set(STAGE_PROFILES "vs_6_5" "ps_6_5"  "cs_6_5")

    foreach(HLSL_SRC IN LISTS SHADER_FILES)
        get_filename_component(FILE_NAME_WE ${HLSL_SRC} NAME_WLE)
        get_filename_component(FILE_NAME ${HLSL_SRC} NAME)

        foreach(i RANGE 1)
            list(GET STAGE_EXTS     ${i} EXT)
            list(GET STAGE_ENTRIES  ${i} ENTRY)
            list(GET STAGE_PROFILES ${i} PROFILE)

            set(OUTPUT_SPV "${GEN_INCLUDE_DIR}/${FILE_NAME}.${EXT}.spv")

            add_custom_command(
                OUTPUT ${OUTPUT_SPV}
                COMMAND ${DXC_EXECUTABLE} -T ${PROFILE} -E ${ENTRY} -spirv -fspv-target-env=vulkan1.3 -I "${CMAKE_SOURCE_DIR}/resources/shaders" -I "${CMAKE_SOURCE_DIR}/include" ${HLSL_SRC} -Fo ${OUTPUT_SPV}
                DEPENDS ${HLSL_SRC} "${CMAKE_SOURCE_DIR}/resources/shaders/common.hlsl" "${CMAKE_SOURCE_DIR}/include/SharedMath.hpp"
                COMMENT "DXC: Generating ${FILE_NAME}.${EXT}.spv"
                VERBATIM
            )
            list(APPEND ALL_SPV_OUTPUTS ${OUTPUT_SPV})

            string(MAKE_C_IDENTIFIER "SHADER_${FILE_NAME}_${EXT}_PATH" MACRO_NAME)
            string(TOUPPER ${MACRO_NAME} MACRO_NAME)

            target_compile_definitions(${TARGET_NAME} PRIVATE ${MACRO_NAME}="${OUTPUT_SPV}")
        endforeach()
    endforeach()

    add_custom_target(${TARGET_NAME}_shader_gen ALL DEPENDS ${ALL_SPV_OUTPUTS})
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_shader_gen)

    set_source_files_properties(src/engine/Resources.c PROPERTIES OBJECT_DEPENDS "${ALL_SPV_OUTPUTS}")
endfunction()

function(compile_hlsl SHADER_PATH ENTRY STAGE OUTPUT_VAR)
    find_program(DXC_EXECUTABLE NAMES dxc PATHS "$ENV{VULKAN_SDK}/bin" "D:/Vulkan-SDK/1.4.341.1/bin")
    if(NOT DXC_EXECUTABLE)
        message(FATAL_ERROR "DXC not found!")
    endif()

    get_filename_component(FILE_NAME ${SHADER_PATH} NAME)

    set(OUTPUT_SPV "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders/${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv")

    set(EXTRA_ARGS ${ARGN})

    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${DXC_EXECUTABLE} -T ${STAGE} -E ${ENTRY} -spirv -fspv-target-env=vulkan1.3 -I "${CMAKE_SOURCE_DIR}/resources/shaders" -I "${CMAKE_SOURCE_DIR}/include" ${EXTRA_ARGS} ${SHADER_PATH} -Fo ${OUTPUT_SPV}
        DEPENDS ${SHADER_PATH} "${CMAKE_SOURCE_DIR}/include/SharedMath.hpp" "${CMAKE_SOURCE_DIR}/resources/shaders/common.hlsl"
        COMMENT "DXC: Generating ${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv"
        VERBATIM
    )
    set(${OUTPUT_VAR} ${OUTPUT_SPV} PARENT_SCOPE)
endfunction()

compile_shaders(zahlen_engine
    "${CMAKE_SOURCE_DIR}/resources/shaders/basic.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/blit.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/taa.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/ui.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/fxaa.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/ambient.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/lighting.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/bloom_threshold.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/bloom_blur.hlsl"
    "${CMAKE_SOURCE_DIR}/resources/shaders/punctual_shadows.hlsl"
)

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/culling.hlsl" CSMain cs_6_0 CULLING_COMP_SPV)
add_custom_target(zahlen_engine_culling_shader ALL DEPENDS ${CULLING_COMP_SPV})
add_dependencies(zahlen_engine zahlen_engine_culling_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_CULLING_HLSL_CS_PATH="${CULLING_COMP_SPV}")
set_source_files_properties(src/engine/Resources.c PROPERTIES OBJECT_DEPENDS "${CULLING_COMP_SPV}")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/basic.hlsl" PSShadow ps_6_0 SHADOW_FRAG_SPV)
add_custom_target(zahlen_engine_shadow_shader ALL DEPENDS ${SHADOW_FRAG_SPV})
add_dependencies(zahlen_engine zahlen_engine_shadow_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_SHADOW_HLSL_PS_PATH="${SHADOW_FRAG_SPV}")
set_source_files_properties(src/engine/Resources.c PROPERTIES OBJECT_DEPENDS "${SHADOW_FRAG_SPV}")

# Reflections shaders (RT vs NoRT)
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/reflection.hlsl" VSMain vs_6_5 REFLECTION_VS_SPV)
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/reflection.hlsl" PSMain ps_6_5 REFLECTION_PS_SPV)
add_custom_target(zahlen_engine_reflection_shader ALL DEPENDS ${REFLECTION_VS_SPV} ${REFLECTION_PS_SPV})
add_dependencies(zahlen_engine zahlen_engine_reflection_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_REFLECTION_HLSL_VS_PATH="${REFLECTION_VS_SPV}" SHADER_REFLECTION_HLSL_PS_PATH="${REFLECTION_PS_SPV}")
set_property(SOURCE src/engine/Resources.c APPEND PROPERTY OBJECT_DEPENDS "${REFLECTION_VS_SPV}" "${REFLECTION_PS_SPV}")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/reflection.hlsl" VSMain vs_6_5 REFLECTION_NORT_VS_SPV "-DDISABLE_RTR")
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/reflection.hlsl" PSMain ps_6_5 REFLECTION_NORT_PS_SPV "-DDISABLE_RTR")
add_custom_target(zahlen_engine_reflection_nort_shader ALL DEPENDS ${REFLECTION_NORT_VS_SPV} ${REFLECTION_NORT_PS_SPV})
add_dependencies(zahlen_engine zahlen_engine_reflection_nort_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_REFLECTION_NORT_HLSL_VS_PATH="${REFLECTION_NORT_VS_SPV}" SHADER_REFLECTION_NORT_HLSL_PS_PATH="${REFLECTION_NORT_PS_SPV}")
set_property(SOURCE src/engine/Resources.c APPEND PROPERTY OBJECT_DEPENDS "${REFLECTION_NORT_VS_SPV}" "${REFLECTION_NORT_PS_SPV}")


compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaEdgeVS vs_6_5 SMAA_EDGE_VS_SPV "-DEDGE_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=0")
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaEdgePS ps_6_5 SMAA_EDGE_PS_SPV "-DEDGE_PASS -DSMAA_INCLUDE_VS=0 -DSMAA_INCLUDE_PS=1")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaWeightVS vs_6_5 SMAA_WEIGHT_VS_SPV "-DWEIGHT_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=0")
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaWeightPS ps_6_5 SMAA_WEIGHT_PS_SPV "-DWEIGHT_PASS -DSMAA_INCLUDE_VS=0 -DSMAA_INCLUDE_PS=1")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaBlendVS vs_6_5 SMAA_BLEND_VS_SPV "-DBLEND_PASS -DSMAA_INCLUDE_VS=1 -DSMAA_INCLUDE_PS=0")
compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/smaa_wrap.hlsl" SmaaBlendPS ps_6_5 SMAA_BLEND_PS_SPV "-DBLEND_PASS -DSMAA_INCLUDE_VS=0 -DSMAA_INCLUDE_PS=1")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/cluster_bounds.hlsl" CSMain cs_6_0 CLUSTER_BOUNDS_SPV)
add_custom_target(zahlen_engine_cluster_bounds ALL DEPENDS ${CLUSTER_BOUNDS_SPV})
add_dependencies(zahlen_engine zahlen_engine_cluster_bounds)
target_compile_definitions(zahlen_engine PRIVATE SHADER_CLUSTER_BOUNDS_CS_PATH="${CLUSTER_BOUNDS_SPV}")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/cluster_culling.hlsl" CSMain cs_6_0 CLUSTER_CULLING_SPV)
add_custom_target(zahlen_engine_cluster_cull ALL DEPENDS ${CLUSTER_CULLING_SPV})
add_dependencies(zahlen_engine zahlen_engine_cluster_cull)
target_compile_definitions(zahlen_engine PRIVATE SHADER_CLUSTER_CULLING_CS_PATH="${CLUSTER_CULLING_SPV}")

# Create a formal custom target to block parallel-compile race conditions
add_custom_target(zahlen_engine_smaa_shaders ALL DEPENDS
    ${SMAA_EDGE_VS_SPV}
    ${SMAA_EDGE_PS_SPV}
    ${SMAA_WEIGHT_VS_SPV}
    ${SMAA_WEIGHT_PS_SPV}
    ${SMAA_BLEND_VS_SPV}
    ${SMAA_BLEND_PS_SPV}
)
add_dependencies(zahlen_engine zahlen_engine_smaa_shaders)

target_compile_definitions(zahlen_engine PRIVATE
    SHADER_SMAA_EDGE_VS_PATH="${SMAA_EDGE_VS_SPV}"
    SHADER_SMAA_EDGE_PS_PATH="${SMAA_EDGE_PS_SPV}"
    SHADER_SMAA_WEIGHT_VS_PATH="${SMAA_WEIGHT_VS_SPV}"
    SHADER_SMAA_WEIGHT_PS_PATH="${SMAA_WEIGHT_PS_SPV}"
    SHADER_SMAA_BLEND_VS_PATH="${SMAA_BLEND_VS_SPV}"
    SHADER_SMAA_BLEND_PS_PATH="${SMAA_BLEND_PS_SPV}"
)

set_property(SOURCE src/engine/Resources.c APPEND PROPERTY OBJECT_DEPENDS
    "${SMAA_EDGE_VS_SPV}"
    "${SMAA_EDGE_PS_SPV}"
    "${SMAA_WEIGHT_VS_SPV}"
    "${SMAA_WEIGHT_PS_SPV}"
    "${SMAA_BLEND_VS_SPV}"
    "${SMAA_BLEND_PS_SPV}"
)

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/skinning.hlsl" CSMain cs_6_0 SKINNING_COMP_SPV)
add_custom_target(zahlen_engine_skinning_shader ALL DEPENDS ${SKINNING_COMP_SPV})
add_dependencies(zahlen_engine zahlen_engine_skinning_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_SKINNING_HLSL_CS_PATH="${SKINNING_COMP_SPV}")
set_source_files_properties(src/engine/Resources.c PROPERTIES OBJECT_DEPENDS "${SKINNING_COMP_SPV}")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/basic.hlsl" PSForward ps_6_0 FORWARD_FRAG_SPV "-DFORWARD_PASS")
add_custom_target(zahlen_engine_forward_shader ALL DEPENDS ${FORWARD_FRAG_SPV})
add_dependencies(zahlen_engine zahlen_engine_forward_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_FORWARD_HLSL_PS_PATH="${FORWARD_FRAG_SPV}")
set_property(SOURCE src/engine/Resources.c APPEND PROPERTY OBJECT_DEPENDS "${FORWARD_FRAG_SPV}")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/hang_gpu.hlsl" CSMain cs_6_0 HANG_GPU_COMP_SPV)
add_custom_target(zahlen_engine_hang_gpu_shader ALL DEPENDS ${HANG_GPU_COMP_SPV})
add_dependencies(zahlen_engine zahlen_engine_hang_gpu_shader)
target_compile_definitions(zahlen_engine PRIVATE SHADER_HANG_GPU_HLSL_CS_PATH="${HANG_GPU_COMP_SPV}")
set_property(SOURCE src/engine/Resources.c APPEND PROPERTY OBJECT_DEPENDS "${HANG_GPU_COMP_SPV}")

compile_hlsl("${CMAKE_SOURCE_DIR}/resources/shaders/procedural_bake.hlsl" CSMain cs_6_0 PROCEDURAL_BAKE_CS_SPV)
add_custom_target(zahlen_engine_procedural_bake ALL DEPENDS ${PROCEDURAL_BAKE_CS_SPV})
add_dependencies(zahlen_engine zahlen_engine_procedural_bake)
target_compile_definitions(zahlen_engine PRIVATE SHADER_PROCEDURAL_BAKE_CS_PATH="${PROCEDURAL_BAKE_CS_SPV}")

# --- FENNEL COMPILER INTEGRATION ---
find_program(FENNEL_BIN NAMES fennel)

function(compile_fennel TARGET_NAME OUTPUT_LUA_FILES_VAR)
    set(LUA_OUTPUTS "")
    foreach(FNL_SRC IN LISTS ARGN)
        get_filename_component(FILE_NAME_WE ${FNL_SRC} NAME_WLE)
        get_filename_component(FILE_DIR ${FNL_SRC} DIRECTORY)

        # Maintain relative directory structures from the source folder
        file(RELATIVE_PATH REL_PATH "${CMAKE_SOURCE_DIR}" "${FILE_DIR}")
        set(OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/${REL_PATH}")
        file(MAKE_DIRECTORY "${OUT_DIR}")

        set(OUTPUT_LUA "${OUT_DIR}/${FILE_NAME_WE}.lua")

        if(FENNEL_BIN)
            add_custom_command(
                OUTPUT ${OUTPUT_LUA}
                COMMAND ${FENNEL_BIN} --use-bit-lib --compile ${FNL_SRC} > ${OUTPUT_LUA}
                DEPENDS ${FNL_SRC}
                COMMENT "Fennel: Compiling ${FNL_SRC} -> ${OUTPUT_LUA}"
                VERBATIM
            )
            list(APPEND LUA_OUTPUTS ${OUTPUT_LUA})
        else()
            message(FATAL_ERROR "Fennel compiler ('fennel') not found in PATH. Cannot compile ${FNL_SRC}")
        endif()
    endforeach()
    set(${OUTPUT_LUA_FILES_VAR} ${LUA_OUTPUTS} PARENT_SCOPE)
endfunction()

# --- PREVENT CIRCULAR DIRECTORY SYMLINKS ---
# If the build directory has a stale directory symlink, remove it so it can be
# generated as a real folder with individual script links instead.
if(IS_SYMLINK "${CMAKE_CURRENT_BINARY_DIR}/scripts")
    message(STATUS "Removing stale scripts directory symlink: ${CMAKE_CURRENT_BINARY_DIR}/scripts")
    file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/scripts")
endif()

# 1. Compile all Fennel files directly into the build folder
compile_fennel(zahlen_engine COMPILED_LUA_FILES
    "${CMAKE_SOURCE_DIR}/scripts/core/memoryview.fnl"
    "${CMAKE_SOURCE_DIR}/scripts/core/zahlen.fnl"
    "${CMAKE_SOURCE_DIR}/scripts/core/ecs.fnl"
    "${CMAKE_SOURCE_DIR}/scripts/core/dialogue.fnl"
    "${CMAKE_SOURCE_DIR}/scripts/core/inventory.fnl"
    "${CMAKE_SOURCE_DIR}/scripts/core/ffi_cdef.fnl"
    "${CMAKE_SOURCE_DIR}/scripts/gameplay.fnl"
    "${CMAKE_SOURCE_DIR}/scripts/main_menu.fnl"
    "${CMAKE_SOURCE_DIR}/scripts/dialogue_db.fnl"
)

# 2. Selectively copy or link static non-Fennel files (e.g. boot.lua, fennel.lua)
file(GLOB_RECURSE STATIC_SCRIPT_FILES
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/scripts/*.lua"
    "${CMAKE_SOURCE_DIR}/scripts/*.sh"
    "${CMAKE_SOURCE_DIR}/scripts/*.txt"
)

set(STATIC_SCRIPT_SYMLINKS "")
foreach(STATIC_SRC IN LISTS STATIC_SCRIPT_FILES)
    file(RELATIVE_PATH REL_FILE "${CMAKE_SOURCE_DIR}/scripts" "${STATIC_SRC}")
    get_filename_component(REL_DIR "${REL_FILE}" DIRECTORY)

    set(OUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/scripts/${REL_FILE}")
    set(OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/scripts/${REL_DIR}")

    file(MAKE_DIRECTORY "${OUT_DIR}")

    if(WIN32)
        # Windows symlinking often requires elevation; copy is safer for local dev
        add_custom_command(
            OUTPUT "${OUT_FILE}"
            COMMAND ${CMAKE_COMMAND} -E copy "${STATIC_SRC}" "${OUT_FILE}"
            DEPENDS "${STATIC_SRC}"
            COMMENT "Copying static script: ${REL_FILE}"
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT "${OUT_FILE}"
            COMMAND ${CMAKE_COMMAND} -E create_symlink "${STATIC_SRC}" "${OUT_FILE}"
            DEPENDS "${STATIC_SRC}"
            COMMENT "Symlinking static script: ${REL_FILE}"
            VERBATIM
        )
    endif()
    list(APPEND STATIC_SCRIPT_SYMLINKS "${OUT_FILE}")
endforeach()

add_custom_target(sync_static_scripts DEPENDS ${STATIC_SCRIPT_SYMLINKS})

# 3. Create a target blocking engine compilation until all scripts are generated
add_custom_target(zahlen_fennel_compilation ALL DEPENDS ${COMPILED_LUA_FILES})
add_dependencies(zahlen_fennel_compilation sync_static_scripts)
add_dependencies(zahlen_engine zahlen_fennel_compilation)

add_executable(zahlen
    src/main.cpp
    src/game_main.cpp
    src/editor_main.cpp
)
target_link_libraries(zahlen PRIVATE zahlen_engine imgui)
target_compile_definitions(zahlen PRIVATE JPH_DOUBLE_PRECISION)

add_executable(zcook
    src/zcook/main.cpp
    src/zcook/Transform.cpp
    src/zcook/GLB.cpp
    src/zcook/Cook.cpp
)
target_link_libraries(zcook PRIVATE zahlen_engine)
target_include_directories(zcook PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/cgltf
    ${CMAKE_SOURCE_DIR}/src/zcook
)

# Generates the safe, non-colliding assets.ninja blueprint
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/assets.ninja"
    COMMAND python3 "${CMAKE_SOURCE_DIR}/configure_assets.py" "${CMAKE_CURRENT_BINARY_DIR}/assets.ninja" "$<TARGET_FILE:zcook>" "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS "${CMAKE_SOURCE_DIR}/configure_assets.py"
            "${CMAKE_SOURCE_DIR}/tools/export_metadata.py" # Track changes to the exporter script
    COMMENT "Scanning asset folders and generating assets.ninja..."
)

# Executes the Ninja asset build pipeline in parallel
add_custom_target(cook_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:zahlen>/data"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/build_assets"
    COMMAND ninja -f "${CMAKE_CURRENT_BINARY_DIR}/assets.ninja"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/data/base.pak" "$<TARGET_FILE_DIR:zahlen>/data/base.pak"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/assets.ninja"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Cooking assets in parallel using Ninja..."
    USES_TERMINAL
)

add_dependencies(cook_assets zcook)

if(NOT MSVC)
    # Define flags that should ONLY apply to GNU/GCC
    set(GCC_ONLY_FLAGS
        "--param=destructive-interference-size=64"
        "-Wno-interference-size"
        "-fno-exceptions"
        "-fno-rtti"
    )

    target_compile_options(zahlen_engine PRIVATE
        $<$<AND:$<CXX_COMPILER_ID:GNU>,$<COMPILE_LANGUAGE:CXX>>:${GCC_ONLY_FLAGS}>

        # General warnings (always applied to non-MSVC)
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
    )
    if(WIN32)
        target_link_options(zahlen_engine PRIVATE -Xlinker /subsystem:console)
        target_compile_options(zahlen_engine PRIVATE -Wno-microsoft-anon-tag -Wno-unknown-pragmas -Wno-unused-parameter)
        target_compile_options(zahlen_engine PRIVATE -Wno-microsoft-anon-tag -Wno-unknown-pragmas -Wno-unused-parameter -Wno-microsoft-enum-forward-reference)
        target_compile_options(zahlen_engine PRIVATE -D_CRT_SECURE_NO_WARNINGS)
    endif()
endif()

target_compile_options(zahlen_render PRIVATE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-freflection>
)

add_custom_target(link_resources ALL
    COMMAND ${CMAKE_COMMAND}
        -D SRC="${CMAKE_SOURCE_DIR}/resources"
        -D DST="${CMAKE_CURRENT_BINARY_DIR}/resources"
        -P "${CMAKE_SOURCE_DIR}/cmake/CreateSymlinks.cmake"
    COMMENT "Creating zero-copy symlinks for resources..."
)

add_dependencies(zahlen cook_assets)
add_dependencies(zahlen link_resources)

if(USE_SANITIZERS)
    get_filename_component(CLANG_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    find_file(ASAN_DLL
        NAMES "clang_rt.asan_dynamic-x86_64.dll"
        PATHS "${CLANG_BIN_DIR}/../lib/clang/23/lib/windows" "${CLANG_BIN_DIR}/../lib/clang/22/lib/windows"
    )
    if(ASAN_DLL)
        add_custom_command(TARGET zahlen POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy "${ASAN_DLL}" "$<TARGET_FILE_DIR:zahlen>"
            COMMENT "Copying ASan Runtime for zahlen..."
        )
    endif()
endif()

# --- APPLY TARGET-SPECIFIC REFLECTION FLAGS ---
if(COMPILER_HAS_REFLECTION)
    # Target-specific C++ only flags via generator expressions ($<COMPILE_LANGUAGE:CXX>)
    target_compile_options(zahlen_render PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")
    target_compile_options(zahlen_engine PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")
    target_compile_options(zahlen PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")

    # Propagate the preprocessor definition safely
    target_compile_definitions(zahlen_render PRIVATE __cpp_reflection=202600L)
    target_compile_definitions(zahlen_engine PRIVATE __cpp_reflection=202600L)
    target_compile_definitions(zahlen PRIVATE __cpp_reflection=202600L)
endif()
