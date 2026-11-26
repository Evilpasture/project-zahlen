# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# plugins/lua -- reflection-driven Lua state evaluation, patching, and
# zero-co-ownership C API interop (zahlen_lua).
#
# Peer to plugins/toml and plugins/json: Lua chunks are evaluated in a
# hermetic, instruction- and memory-bounded VM (no ffi/io/os/package/debug,
# no per-frame TickUpdate) to construct or transactionally patch reflected C++
# structs and ECS components, or expose POD userdata/listeners via LuaInterop.
# Lua headers stay strictly private to Lua.cpp so consumers never
# include <lua.h> or touch raw lua_State*.

set(TARGET_NAME zahlen_lua)

add_library(${TARGET_NAME} STATIC
    Lua.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine zahlen_scene
)

target_link_libraries(${TARGET_NAME} PRIVATE
    LuaJIT
    zahlen_warnings
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/plugins
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

if(COMMAND zahlen_add_plugin)
    zahlen_add_plugin(zahlen_plugin_lua lua LuaPlugin.cpp)
    target_link_libraries(zahlen_plugin_lua PRIVATE zahlen_lua LuaJIT)
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/zahlen_plugin_lua.manifest.toml
    ${CMAKE_BINARY_DIR}/lib/zahlen_plugin_lua.manifest.toml
    COPYONLY
)
