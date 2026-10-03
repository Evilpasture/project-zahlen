# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Asset-format and codec plugins. Each plugin owns its target and its optional
# third-party dependencies; this file only establishes the shared serializer
# target and the dependency order between plugins.
#
# AssetCooking is configured separately by cmake/AssetPipeline.cmake because
# zcook requires its host-side codecs even when ZHLN_BUILD_EXTRAS=OFF.
#
# Plugins may consume the engine's public API, but the reverse dependency is
# forbidden. The configure-time layer-boundary check scans plugins/,
# extensions/, and gameplay/ and enforces that one-way rule.

# JSON and TOML share one target because TOML scene/UI adapters consume the JSON
# document layer, while neither format should define a second serializer
# library. Their own subdirectories list the implementation sources.
add_library(zahlen_serialization STATIC)
add_subdirectory(json)
add_subdirectory(toml)

set_target_properties(zahlen_serialization PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(zahlen_serialization PUBLIC zahlen_engine)
target_include_directories(zahlen_serialization PUBLIC
    ${PROJECT_SOURCE_DIR}/plugins
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_serialization)
endif()
target_link_libraries(zahlen_serialization PRIVATE zahlen_warnings)

# The importer consumes the serializer; Fonts consumes both engine and
# serialization; SVG remains independently optional when resvg is unavailable.
add_subdirectory(glTF)
add_subdirectory(Fonts)
add_subdirectory(SVG)
