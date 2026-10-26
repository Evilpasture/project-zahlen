# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/Scripting/Lua -- the LuaJIT-backed scripting runtime.
#
# This is the whole of the engine's Lua support: the LuaJIT state
# (LuaScriptRuntime), the C ABI and command dispatch scripts call
# (Scripting.cpp, ScriptingABI.* -- the shim over the generic binder in
# ../), and the Fennel sources under scripts/. Core links no Lua, includes no
# lua headers, and has no C ABI. The generic, Lua-independent half of the
# scripting layer lives in extensions/Scripting/ (zahlen_scripting), which this
# target links.

set(TARGET_NAME zahlen_scripting_lua)

add_library(${TARGET_NAME} STATIC
    LuaScriptRuntime.cpp
    Scripting.cpp
    ScriptingABI.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_scripting
)
# Scripting.cpp dispatches SpawnTerrain into the terrain extra (ZHLN::Terrain
# spawners left the core factory) and edits IK::TwoBoneIKComponent chains for
# the AddIKChain/SetIKTarget commands (the component moved to extensions/Animation
# with its solver); InitPlayer installs the third-person rig from
# extensions/Camera. The declarations arrive through the extensions include root
# published by zahlen_scripting; the definitions live in these domains.
#
# No gameplay/ target is linked here, and none may be added: SetupRagdoll was
# the one command that reached into gameplay/RagdollAuthoring, and it is now a
# command that module contributes by name through ScriptCommandRegistry. The
# DataModel bindings moved to gameplay/ProjectLight/LuaBindings. Between them
# those two links were what made this target and the gameplay layer circular.
# configure/check_layer_dag.py is what keeps them from coming back.
target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_terrain
    zahlen_animation
    zahlen_camera
)
target_link_libraries(${TARGET_NAME} PRIVATE
    LuaJIT
)

# Public headers are included as <Scripting/Lua/...> by consumers and as
# <Scripting/...> internally, so the extensions root is published.
target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)
target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/LuaJIT/src
)

# Scripting.cpp drives two engine systems directly (AnimationSystem,
# InputSystem), so it needs core's private tree, not just its public headers.
# That coupling predates the move -- it was an internal include either way --
# but it is the one place where this extra does not sit on core's public API,
# and it should shrink rather than grow.
target_include_directories(${TARGET_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src)

zahlen_enable_reflection(${TARGET_NAME})

# --- FFI cdef generator ----------------------------------------------------
# The component layouts the scripts see are read out of ZHLN::Components through
# reflection at build time rather than maintained by hand in ffi_cdef.fnl. This
# needs real static reflection: without it ForEachNestedType expands to nothing,
# and the generator exits non-zero rather than writing a plausible file with no
# components in it.
# GenFFICdef.cpp only reflects compile-time layouts and never calls engine
# runtime functions, so build it header-only without waiting on zahlen_engine,
# zahlen_render, or Jolt.
add_executable(zahlen_gen_ffi_cdef
    tools/GenFFICdef.cpp
)
set_target_properties(zahlen_gen_ffi_cdef PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_options(zahlen_gen_ffi_cdef PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
)
target_compile_definitions(zahlen_gen_ffi_cdef PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_SHARED_LIBRARY
)
target_include_directories(zahlen_gen_ffi_cdef PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
# <Scripting/Lua/tools/FFICTypeMap.hpp>, CharacterController, Camera, and Jolt
# headers resolve directly without linking runtime archives.
target_include_directories(zahlen_gen_ffi_cdef SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extensions
    ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
)
target_link_libraries(zahlen_gen_ffi_cdef PRIVATE
    zahlen_warnings
    $<$<AND:$<BOOL:${MINGW}>,$<CXX_COMPILER_ID:GNU>>:stdc++exp>
)
zahlen_enable_reflection(zahlen_gen_ffi_cdef)

set(GEN_FFI_CDEF_DIR "${CMAKE_BINARY_DIR}/scripts/core")
set(GEN_FFI_CDEF_OUTPUT "${GEN_FFI_CDEF_DIR}/ffi_cdef_generated.lua")
file(MAKE_DIRECTORY "${GEN_FFI_CDEF_DIR}")

add_custom_command(
    OUTPUT ${GEN_FFI_CDEF_OUTPUT}
    COMMAND zahlen_gen_ffi_cdef ${GEN_FFI_CDEF_OUTPUT}
    DEPENDS zahlen_gen_ffi_cdef
    COMMENT "Generating ffi_cdef_generated.lua from ZHLN::Components"
    VERBATIM
)

# Audit the generated layouts against Components.hpp. It compiles both sides and
# compares sizeof and every offset, so a mapping the generator gets wrong fails
# here rather than corrupting memory from a script at runtime.
# The probe has to be built the way the engine is. Some members are sized
# by reflection -- InputStateComponent::keys is a bitset sized by
# EnumCount<KeyCode>(), 16 bytes with reflection and 1 without -- so auditing
# against a differently-configured compile reports drift that is not there.
if(COMPILER_HAS_REFLECTION)
    set(FFI_LAYOUT_REFLECTION_FLAGS
        "--cxx-flag=-freflection"
        "--cxx-flag=-D__cpp_impl_reflection=202603L"
    )
else()
    set(FFI_LAYOUT_REFLECTION_FLAGS "")
endif()

add_custom_target(check_ffi_layout
    COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_ffi_layout.py"
            --cdef ${GEN_FFI_CDEF_OUTPUT}
            ${FFI_LAYOUT_REFLECTION_FLAGS}
            --cxx-compiler ${CMAKE_CXX_COMPILER}
            --cxx-include ${PROJECT_SOURCE_DIR}/include
            --cxx-include ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
    DEPENDS ${GEN_FFI_CDEF_OUTPUT}
    COMMENT "Auditing generated FFI layouts against Components.hpp"
    VERBATIM
)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/core/ffi_cdef.fnl"
)

# Static, non-Fennel script files are linked (or copied on Windows) alongside
# the compiled ones so the tree the runtime sees is complete.
# The scripts that ship as files rather than compiled Fennel: linked into the
# build tree on POSIX, copied on Windows. Listed, not discovered -- a script
# that stops being linked is a visible removal from this list rather than a file
# the build quietly stopped carrying.
set(STATIC_SCRIPT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/core/fennel.lua"
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
add_custom_target(zahlen_fennel_compilation ALL
    DEPENDS ${COMPILED_SCRIPT_LUA_FILES} ${GEN_FFI_CDEF_OUTPUT}
)
add_dependencies(zahlen_fennel_compilation sync_static_scripts)
add_dependencies(${TARGET_NAME} zahlen_fennel_compilation)
