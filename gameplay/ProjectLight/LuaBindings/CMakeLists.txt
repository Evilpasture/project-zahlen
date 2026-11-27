# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# gameplay/ProjectLight/LuaBindings -- the DataModel <-> Lua::VirtualMachine adapter
# (zahlen_project_light_lua).
#
# Links ProjectLight's canonical DataModel with plugins/lua (zahlen_lua) via
# <lua/Host.hpp>. Never includes <lua.h> or links LuaJIT directly; the
# classic Lua C API remains strictly private to plugins/lua/.

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

target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_datamodel
)
target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_lua
    zahlen_warnings
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/gameplay/ProjectLight
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()
