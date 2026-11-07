# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# gameplay/ProjectLight/LuaBindings -- the DataModel <-> LuaJIT adapter
# (zahlen_project_light_lua).
#
# Links ProjectLight's canonical DataModel with plugins/lua (zahlen_lua) and
# LuaJIT directly. Configured straight from gameplay/ProjectLight/CMakeLists.txt
# because plugins/ precedes gameplay/ in the layer DAG.

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

# No zahlen_lua edge. DataModelBindings.hpp forward-declares lua_State and
# includes nothing from plugins/lua; the only consumer of the Lua C API here is
# DataModelBindings.cpp, which includes <lua.h> and gets it from the LuaJIT
# import below. A consumer that imports ReflectLua links zahlen_lua itself
# (tests/extras TestLuaDataModel does).
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_datamodel
)
target_link_libraries(${TARGET_NAME} PRIVATE LuaJIT)
target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/gameplay/ProjectLight
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

