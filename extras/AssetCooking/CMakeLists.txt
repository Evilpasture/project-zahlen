# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Offline/host-side codecs. AssetPipeline defines this target even when the
# optional gameplay extras are disabled: zcook always needs it, and a host may
# opt in to decoding source environments before registering their pixels.
zahlen_transpile_sources(ASSET_COOKING_SOURCES RadianceDecoder.cpp RadianceEncoder.cpp)

add_library(zahlen_asset_cooking STATIC ${ASSET_COOKING_SOURCES})
set_target_properties(zahlen_asset_cooking PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(zahlen_asset_cooking PUBLIC ${PROJECT_SOURCE_DIR}/extras)
target_include_directories(zahlen_asset_cooking PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(zahlen_asset_cooking SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/extern/stb)
target_link_libraries(zahlen_asset_cooking
    PUBLIC zahlen_engine zahlen_filesystem
    PRIVATE zahlen_warnings
)

# ErrorCode's diagnostic names use the reflection configuration. stb_image's
# implementation is already compiled into zahlen_engine; do not duplicate it.
zahlen_enable_reflection(zahlen_asset_cooking)
