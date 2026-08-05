# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    cmake_minimum_required(VERSION 3.28)
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
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
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
    )

    set(GAMEPLAY_LINK_LIBS
        zahlen_engine
        Jolt
    )
endif()

# ============================================================================
# AUTO-DISCOVER ALL .cppm MODULE INTERFACES
# ============================================================================
file(GLOB_RECURSE MODULE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm")

add_library(zahlen_modules STATIC)

target_sources(zahlen_modules
    PUBLIC
        FILE_SET CXX_MODULES FILES
            ${MODULE_SOURCES}
)

set_target_properties(zahlen_modules PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_SCAN_FOR_MODULES ON
    CXX_MODULE_STD ON
)

target_include_directories(zahlen_modules PUBLIC ${ENGINE_INCLUDE_DIRS})

target_compile_definitions(zahlen_modules PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_SHARED_LIBRARY
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:JPH_RELEASE>
)

if(NOT MSVC)
    target_compile_options(zahlen_modules PRIVATE
        -Wall -Wextra -Wpedantic
        -fno-exceptions
        -fno-rtti
        $<$<CXX_COMPILER_ID:GNU>:-fmodules-ts>
    )
endif()

if(COMPILER_HAS_REFLECTION OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    target_compile_options(zahlen_modules PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")
endif()

target_link_libraries(zahlen_modules PUBLIC
    zahlen_engine
    zahlen_ecs
    zahlen_audio
    Jolt
)

# ============================================================================
# DYNAMIC GAMEPLAY MODULE (libgameplay.so / gameplay.dll)
# ============================================================================
add_library(gameplay SHARED gameplay.cpp)

set_target_properties(gameplay PROPERTIES
    CXX_SCAN_FOR_MODULES ON
    CXX_MODULE_STD ON
    BUILD_RPATH "$ORIGIN;$ORIGIN/extern/JoltPhysics/Build;${CMAKE_CURRENT_BINARY_DIR};${CMAKE_CURRENT_BINARY_DIR}/extern/JoltPhysics/Build"
)

target_include_directories(gameplay PRIVATE ${ENGINE_INCLUDE_DIRS})

target_compile_definitions(gameplay PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_SHARED_LIBRARY
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:JPH_RELEASE>
)

if(NOT MSVC)
    target_compile_options(gameplay PRIVATE
        -Wall -Wextra -Wpedantic
        -fno-exceptions
        -fno-rtti
        $<$<CXX_COMPILER_ID:GNU>:-fmodules-ts>
    )
endif()

if(COMPILER_HAS_REFLECTION OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    target_compile_options(gameplay PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")
endif()

target_link_libraries(gameplay PRIVATE
    zahlen_modules
    ${GAMEPLAY_LINK_LIBS}
)

if(TARGET zahlen)
    add_custom_command(TARGET gameplay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Jolt> $<TARGET_FILE_DIR:zahlen>/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gameplay> $<TARGET_FILE_DIR:zahlen>/
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_SOURCE_DIR}/scripts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:gameplay> "${PROJECT_SOURCE_DIR}/scripts/"
        COMMENT "Copying gameplay dynamic library into scripts directory..."
    )
endif()
