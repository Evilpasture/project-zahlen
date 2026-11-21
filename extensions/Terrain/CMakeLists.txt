# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# Capability-only host terrain; no module, global slot table or graph adapter.
add_library(zahlen_terrain STATIC TerrainGeometry.cpp Terrain.cpp)
set_target_properties(zahlen_terrain PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zahlen_terrain PUBLIC zahlen_engine PRIVATE zahlen_warnings)
target_include_directories(zahlen_terrain PUBLIC ${PROJECT_SOURCE_DIR}/extensions)
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_terrain)
endif()

if(COMMAND zahlen_add_plugin)
    zahlen_add_plugin(zahlen_plugin_terrain terrain TerrainPlugin.cpp)
    target_link_libraries(zahlen_plugin_terrain PRIVATE zahlen_terrain)
    target_include_directories(zahlen_plugin_terrain PUBLIC
        ${PROJECT_SOURCE_DIR}/extensions)
endif()

configure_file(zahlen_plugin_terrain.manifest.toml
    ${CMAKE_BINARY_DIR}/lib/zahlen_plugin_terrain.manifest.toml COPYONLY)
