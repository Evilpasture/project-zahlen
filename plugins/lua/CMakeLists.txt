# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# plugins/lua -- reflection-driven Lua state evaluation and patching (zahlen_lua).
#
# Peer to plugins/toml and plugins/json: Lua chunks are evaluated in a
# hermetic, instruction- and memory-bounded VM (no ffi/io/os/package/debug,
# no per-frame TickUpdate) to construct or transactionally patch reflected C++
# structs and ECS components. Lua headers stay private to ReflectLua.cpp so
# consumers never inherit <lua.h> macros.

set(TARGET_NAME zahlen_lua)

add_library(${TARGET_NAME} STATIC
    ReflectLua.cpp
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
    zahlen_warnings
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/plugins
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()
