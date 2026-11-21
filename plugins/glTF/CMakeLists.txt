# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# CPU document decoding and COM asset authoring. Sole cgltf implementation.
add_library(zahlen_gltf_assets STATIC
    GLTFDocument.cpp GLTFAssets.cpp TangentGenerator.cpp cgltf_impl.c)
set_target_properties(zahlen_gltf_assets PROPERTIES
    CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(zahlen_gltf_assets PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_include_directories(zahlen_gltf_assets SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/cgltf)
target_link_libraries(zahlen_gltf_assets PUBLIC zahlen_engine
    PRIVATE zahlen_serialization zahlen_image zahlen_warnings)
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_gltf_assets)
endif()
