# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# plugins/BSP -- the Source engine BSP importer (zahlen_bsp).
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
# renderer tree. BSPScene additionally consumes the public Scene model; BSPImporter is
# the only file that names RenderContext.
#
# Public headers are reached as <BSP/...>.

set(TARGET_NAME zahlen_bsp)

file(GLOB ZHLN_BSP_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(${TARGET_NAME} STATIC)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

if(ZHLN_BSP_SOURCES)
    target_sources(${TARGET_NAME} PRIVATE ${ZHLN_BSP_SOURCES})
endif()

# Core is a public dependency: <BSP/BSPImporter.hpp> names ZHLN::ModelPrefab,
# RenderContext and AssetManager in its signatures, and <BSP/BSPScene.hpp>
# names the public Scene model.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/plugins
)

# BSPRead.cpp includes <Zahlen/Core/Reflection/Structs.hpp>, whose layout
# depends on reflection.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
