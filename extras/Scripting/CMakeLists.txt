# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/Scripting -- the engine's Lua-independent scripting layer.
#
# Core exposes IScriptRuntime and a ScriptRunner that forwards to it, and
# knows nothing about binding machinery. This target is the generic half of
# the scripting extra: the reflection-driven class table (ScriptBinder), the
# component registry, the value types and the ECS bridge. Nothing here
# includes a Lua header, so a host that wants the binder can link it without
# pulling in a Lua runtime.
#
# The LuaJIT-backed runtime is a separate target defined in Lua/ below; it is
# the only scripting code in this tree that links LuaJIT.

set(TARGET_NAME zahlen_scripting)

add_library(${TARGET_NAME} STATIC
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

# Public headers are included as <Scripting/...> by consumers, so the extras
# root is the only include path this target needs to publish.
target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

zahlen_enable_reflection(${TARGET_NAME})

add_subdirectory(Lua)
