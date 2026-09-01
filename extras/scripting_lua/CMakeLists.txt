# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/scripting_lua -- the Lua scripting layer.
#
# This is the whole of the engine's scripting support, and none of it is core.
# Core exposes IScriptRuntime and a ScriptRunner that forwards to it; this
# target supplies the implementation: the LuaJIT state, the reflection-driven
# FFI definitions, the ScriptBinder class table, and the Fennel sources under
# scripts/. Core links no Lua, includes no lua headers, and has no C ABI.

set(TARGET_NAME zahlen_scripting_lua)

add_library(${TARGET_NAME} STATIC
    Scripting.cpp
    LuaScriptRuntime.cpp
    ScriptECSBridge.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)
target_link_libraries(${TARGET_NAME} PRIVATE
    LuaJIT
)

# Its own headers are included as <scripting_lua/...> by consumers and as
# "..." internally, so both the extras root and this directory are on the path.
target_include_directories(${TARGET_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/extern/LuaJIT/src
)

# Scripting.cpp drives two engine systems directly (AnimationSystem,
# InputSystem), so it needs core's private tree, not just its public headers.
# That coupling predates the move -- it was an internal include either way --
# but it is the one place where this extra does not sit on core's public API,
# and it should shrink rather than grow.
target_include_directories(${TARGET_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src)

zahlen_enable_reflection(${TARGET_NAME})

# --- Fennel bootstrap ------------------------------------------------------
# The Lua-side compiler bootstrap lives with the scripts it compiles.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/core/fennel.lua")
    message(STATUS "fennel.lua not found. Downloading from official fennel-lang.org...")
    file(DOWNLOAD
        "https://fennel-lang.org/downloads/fennel-1.6.1.lua"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/core/fennel.lua"
        SHOW_PROGRESS
        STATUS DOWNLOAD_STATUS
    )
    list(GET DOWNLOAD_STATUS 0 DOWNLOAD_CODE)
    if(NOT DOWNLOAD_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download fennel.lua: ${DOWNLOAD_STATUS}")
    endif()
endif()

# --- Fennel sources -------------------------------------------------------
# Compiled into ${CMAKE_BINARY_DIR}/scripts, which is where the runtime looks
# for them, so relocating the sources does not change any runtime path. The
# zahlen_compile_fennel helper is defined in the root CMakeLists.

if(IS_SYMLINK "${CMAKE_BINARY_DIR}/scripts")
    message(STATUS "Removing stale scripts directory symlink: ${CMAKE_BINARY_DIR}/scripts")
    file(REMOVE "${CMAKE_BINARY_DIR}/scripts")
endif()

zahlen_compile_fennel(${TARGET_NAME} COMPILED_SCRIPT_LUA_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/core/memoryview.fnl"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/core/zahlen.fnl"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/core/ecs.fnl"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/core/dialogue.fnl"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/core/inventory.fnl"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/core/ffi_cdef.fnl"
)

# Static, non-Fennel script files are linked (or copied on Windows) alongside
# the compiled ones so the tree the runtime sees is complete.
file(GLOB_RECURSE STATIC_SCRIPT_FILES
    LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.lua"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/*.txt"
)

set(STATIC_SCRIPT_SYMLINKS "")
foreach(STATIC_SRC IN LISTS STATIC_SCRIPT_FILES)
    file(RELATIVE_PATH REL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/scripts" "${STATIC_SRC}")
    get_filename_component(REL_DIR "${REL_FILE}" DIRECTORY)

    set(OUT_FILE "${CMAKE_BINARY_DIR}/scripts/${REL_FILE}")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/scripts/${REL_DIR}")

    if(WIN32)
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
add_custom_target(zahlen_fennel_compilation ALL DEPENDS ${COMPILED_SCRIPT_LUA_FILES})
add_dependencies(zahlen_fennel_compilation sync_static_scripts)
add_dependencies(${TARGET_NAME} zahlen_fennel_compilation)
