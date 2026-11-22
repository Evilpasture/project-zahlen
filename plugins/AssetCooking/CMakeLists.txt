# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# Offline/host-side codecs and bakers: the single reusable cooking library
# (Zahlen::AssetCooking). Configured from plugins/CMakeLists.txt, or from
# the root when only the cooker CLI is enabled -- never from AssetPipeline.
# A host may opt in to decoding source environments or baking meshes,
# animations, fonts and archives without pulling in the zcook CLI.
set(ASSET_COOKING_SOURCES EnvironmentPreparation.cpp RadianceDecoder.cpp RadianceEncoder.cpp MeshCook.cpp AnimCook.cpp PakWriter.cpp)

# The font baker is the only file that touches stb_truetype: keep the header
# search root on an OBJECT library so it never leaks into other sources.
add_library(zahlen_asset_cooking_font OBJECT FontBake.cpp)
target_link_libraries(zahlen_asset_cooking_font PRIVATE zahlen_engine)
target_include_directories(zahlen_asset_cooking_font PRIVATE ${PROJECT_SOURCE_DIR}/plugins)
target_include_directories(zahlen_asset_cooking_font SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/extern/stb)
set_target_properties(zahlen_asset_cooking_font PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
zahlen_enable_reflection(zahlen_asset_cooking_font)
list(APPEND ASSET_COOKING_SOURCES $<TARGET_OBJECTS:zahlen_asset_cooking_font>)

add_library(zahlen_asset_cooking STATIC ${ASSET_COOKING_SOURCES})
set_target_properties(zahlen_asset_cooking PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(zahlen_asset_cooking PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_include_directories(zahlen_asset_cooking PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zahlen_asset_cooking PRIVATE zahlen_image)
# zahlen_filesystem is not linked: it is PRIVATE to zahlen_engine, so its
# objects are already in libzahlen_engine.so, and RadianceDecoder.cpp only takes
# a const FS::VirtualFileSystem& -- an ZHLN_API type whose symbols the .so
# exports. Linking the archive here put a second copy of VirtualFileSystem and
# of the TaskSystem scheduler state in front of the .so on every consumer's link
# line (ztest_cpu, FidelityHarness, zcook, GPU_Lighting).
target_link_libraries(zahlen_asset_cooking
    PUBLIC zahlen_engine
    PRIVATE zahlen_warnings
)

# ErrorCode's diagnostic names use the reflection configuration.
zahlen_enable_reflection(zahlen_asset_cooking)
