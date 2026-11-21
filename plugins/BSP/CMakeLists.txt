# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# plugins/BSP -- Source format decoding, material policy and COM asset authoring.
# Public headers are reached as <BSP/...>.

add_library(zahlen_bsp_assets STATIC
    BSPBakeField.cpp BSPGeometry.cpp BSPInteraction.cpp BSPLighting.cpp BSPRead.cpp
    BSPVradLightmap.cpp SourceVFS.cpp StudioModelLoader.cpp VMTParser.cpp
    VTFDecoder.cpp VMTLighting.cpp StudioModelAssets.cpp BSPPreparation.cpp
    BSPAssets.cpp BSPScene.cpp)
set_target_properties(zahlen_bsp_assets PROPERTIES
    CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(zahlen_bsp_assets PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_link_libraries(zahlen_bsp_assets
    PUBLIC zahlen_engine zahlen_scene
    PRIVATE zahlen_image zahlen_warnings)
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_bsp_assets)
endif()
