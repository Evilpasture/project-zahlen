# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# CPU document decoding and capability authoring. No native importer or module
# object is pulled into a COM host. This is the sole cgltf implementation;
# native compatibility and tests consume it through this archive.
add_library(zahlen_gltf_assets STATIC
    GLTFDocument.cpp GLTFAssets.cpp TangentGenerator.cpp cgltf_impl.c)
set_target_properties(zahlen_gltf_assets PROPERTIES
    CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(zahlen_gltf_assets PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_include_directories(zahlen_gltf_assets SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/cgltf )
target_link_libraries(zahlen_gltf_assets PUBLIC zahlen_engine
    PRIVATE zahlen_serialization zahlen_image zahlen_warnings)
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_gltf_assets)
endif()

# Retained inspector/native compatibility, sharing the CPU document decoder.
set(TARGET_NAME zahlen_gltf)
set(ZHLN_GLTF_MODULES glTF.cppm)
add_library(${TARGET_NAME} STATIC)
set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON POSITION_INDEPENDENT_CODE ON)
set_source_files_properties(${ZHLN_GLTF_MODULES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_sources(${TARGET_NAME} PUBLIC
    FILE_SET CXX_MODULES TYPE CXX_MODULES
    BASE_DIRS ${PROJECT_SOURCE_DIR}/plugins FILES ${ZHLN_GLTF_MODULES})
target_sources(${TARGET_NAME} PRIVATE GLTFImporter.cpp glTF.cpp)
target_include_directories(${TARGET_NAME} PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_link_libraries(${TARGET_NAME} PUBLIC zahlen_gltf_assets zahlen_engine
    PRIVATE Jolt zahlen_warnings zahlen_native_host_ui)
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()
