# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# gameplay/ProjectLight/LuaBindings -- the DataModel <-> LuaJIT adapter
# (zahlen_project_light_lua).
#
# This target is where the two halves meet, and it is the only place they do:
# it links the generic scripting runtime (which knows nothing about ProjectLight)
# and the DataModel (which knows nothing about Lua), and it depends on LuaJIT
# directly rather than through the runtime that embeds it.
#
# The source used to sit in extensions/Scripting/Lua/ and be compiled into
# zahlen_scripting_lua, which is what forced that target to link
# zahlen_datamodel: the generic script host could not be built without the
# ProjectLight object graph. The dependency is real -- this adapter needs both --
# but it is *this* target's dependency, one layer up, pointing the way the layer
# DAG points. configure/check_layer_dag.py enforces it.
#
# Configured from the root CMakeLists rather than from gameplay/ProjectLight/:
# the build order (DataModel and the Lua runtime both configured first) is the
# composition's business, the way extensions/Scripting/Lua already is.

set(TARGET_NAME zahlen_project_light_lua)

add_library(${TARGET_NAME} STATIC
    DataModelBindings.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# zahlen_scripting_lua is the LuaJIT-backed runtime; the ABI and the state it
# owns are what this adapter installs its userdata into.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_scripting_lua
    zahlen_datamodel
)
target_link_libraries(${TARGET_NAME} PRIVATE LuaJIT)
target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

# Public headers are included as <LuaBindings/...>; the ProjectLight root is
# what zahlen_datamodel publishes, so this publishes its own directory on top.
target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/gameplay/ProjectLight
)
target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/LuaJIT/src
)

zahlen_enable_reflection(${TARGET_NAME})
