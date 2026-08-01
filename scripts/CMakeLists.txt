# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# scripts/CMakeLists.txt
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
    set(CMAKE_CXX_MODULE_STD ON)
    project(ZahlenGameplay LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 26)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    set(ZAHLEN_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Path to Zahlen Engine SDK")

    set(ENGINE_INCLUDE_DIRS
        ${ZAHLEN_SDK_DIR}/include
        ${ZAHLEN_SDK_DIR}/src
        ${ZAHLEN_SDK_DIR}/extern/JoltPhysics
    )

    find_library(ZAHLEN_ENGINE_LIB NAMES zahlen_engine HINTS ${ZAHLEN_SDK_DIR}/build ${ZAHLEN_SDK_DIR}/bin)
    find_library(JOLT_LIB NAMES Jolt HINTS ${ZAHLEN_SDK_DIR}/build/extern/JoltPhysics/Build ${ZAHLEN_SDK_DIR}/bin)

    set(GAMEPLAY_LINK_LIBS ${ZAHLEN_ENGINE_LIB} ${JOLT_LIB})
else()
    set(ENGINE_INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/extern/JoltPhysics
    )

    set(GAMEPLAY_LINK_LIBS
        zahlen_engine
        Jolt
    )
endif()

# ============================================================================
# C++ MODULE SUBDIRECTORIES
# ============================================================================
add_subdirectory(MainMenu)
add_subdirectory(Lightning)
# ============================================================================
# DYNAMIC GAMEPLAY MODULE (libgameplay.so / gameplay.dll)
# ============================================================================
add_library(gameplay SHARED gameplay.cpp)

set_target_properties(gameplay PROPERTIES
    CXX_SCAN_FOR_MODULES ON
    CXX_MODULE_STD ON
)
add_dependencies(gameplay zahlen_main_menu)

target_include_directories(gameplay PRIVATE ${ENGINE_INCLUDE_DIRS})

target_compile_definitions(gameplay PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_SHARED_LIBRARY
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:JPH_RELEASE>
)

set_target_properties(gameplay PROPERTIES
    BUILD_RPATH "$ORIGIN;$ORIGIN/extern/JoltPhysics/Build;${CMAKE_CURRENT_BINARY_DIR};${CMAKE_CURRENT_BINARY_DIR}/extern/JoltPhysics/Build"
)

if(NOT MSVC)
    set(GCC_GAMEPLAY_FLAGS
        "--param=destructive-interference-size=64"
        "-Wno-interference-size"
        "-fno-exceptions"
        "-fno-rtti"
        $<$<CXX_COMPILER_ID:GNU>:-fmodules-ts>
    )

    target_compile_options(gameplay PRIVATE
        $<$<AND:$<CXX_COMPILER_ID:GNU>,$<COMPILE_LANGUAGE:CXX>>:${GCC_GAMEPLAY_FLAGS}>
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
    )
endif()

if(COMPILER_HAS_REFLECTION OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    target_compile_options(gameplay PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")
endif()

target_link_libraries(gameplay PRIVATE
    zahlen_main_menu
    zahlen_lightning
    ${GAMEPLAY_LINK_LIBS}
)

if(TARGET zahlen)
    add_custom_command(TARGET gameplay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Jolt> $<TARGET_FILE_DIR:zahlen>/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gameplay> $<TARGET_FILE_DIR:zahlen>/
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/scripts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gameplay> "${CMAKE_SOURCE_DIR}/scripts/"
        COMMENT "Copying gameplay dynamic library into scripts directory..."
    )
endif()
