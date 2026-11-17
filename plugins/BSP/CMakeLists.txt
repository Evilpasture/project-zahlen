# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# plugins/BSP -- Source format decoding and asset authoring.
# zahlen_bsp_assets is capability-only; zahlen_bsp retains legacy world/scene APIs.
#
# Same shape as plugins/glTF: an extra that produces data core then consumes.
# It imports a .bsp into the plain ZHLN::ModelPrefab (native vertex streams,
# materials, colliders, lights) and caches it under HashAssetPath; Core's
# PrefabFactory::LoadModelPrefab reads that cache and the instantiators
# (PrefabFactory::InstantiatePrefab, Scene::Instantiate) do the rest. No
# registration, and nothing in src/ includes it.
#
# The format/geometry layers (BSPTypes, BSPRead, BSPGeometry) are engine-free
# on purpose: they can be built and validated without configuring the full
# renderer tree. BSPScene additionally consumes the consumer Scene document; the
# remaining Engine-based entry points stay in the separate legacy archive.
#
# Public headers are reached as <BSP/...>.

set(TARGET_NAME zahlen_bsp)

# Native-free Source decoding, material policy and capability authoring. Keep
# this archive separate so a COM host cannot accidentally pull the legacy
# Engine/PrefabFactory entry points through its link dependency graph.
add_library(zahlen_bsp_assets STATIC
    BSPBakeField.cpp BSPGeometry.cpp BSPLighting.cpp BSPRead.cpp
    BSPVradLightmap.cpp SourceVFS.cpp StudioModelLoader.cpp VMTParser.cpp
    VTFDecoder.cpp VMTLighting.cpp StudioModelAssets.cpp BSPPreparation.cpp BSPAssets.cpp BSPScene.cpp)
set_target_properties(zahlen_bsp_assets PROPERTIES CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(zahlen_bsp_assets PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_include_directories(zahlen_bsp_assets PRIVATE )
target_link_libraries(zahlen_bsp_assets PUBLIC zahlen_engine zahlen_scene PRIVATE zahlen_image zahlen_warnings)
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_bsp_assets)
endif()

set(ZHLN_BSP_SOURCES BSPImporter.cpp BSPInteraction.cpp StudioModelImporter.cpp)

add_library(${TARGET_NAME} STATIC)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_sources(${TARGET_NAME} PRIVATE ${ZHLN_BSP_SOURCES})

# Core is a public dependency: <BSP/BSPImporter.hpp> names ZHLN::ModelPrefab,
# RenderContext and AssetManager in its signatures, and <BSP/BSPScene.hpp>
# names the public Scene model.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine zahlen_bsp_assets zahlen_scene
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/plugins
)

target_include_directories(${TARGET_NAME} PRIVATE

)

# BSPRead.cpp includes <Zahlen/Core/Reflection/Structs.hpp>, whose layout
# depends on reflection.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

# Jolt is direct: this library's own objects reference JPH symbols (shapes,
# settings, factories), so it must be on the link line of whatever consumes
# them. It used to arrive only through zahlen_engine's PUBLIC Jolt, which put
# libJolt.so on all 18 executables -- including the ones that never touch it.
target_link_libraries(${TARGET_NAME} PRIVATE Jolt zahlen_image zahlen_warnings)
