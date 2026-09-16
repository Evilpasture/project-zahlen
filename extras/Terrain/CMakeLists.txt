# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/Terrain -- procedural terrain content generation (zahlen_terrain).
#
# FBM/warp/ridged-noise heightmaps, slope-and-altitude tinting, heightfield
# mesh baking and the TerrainComponent bookkeeping system: sample-world
# content creation, not engine substrate. Core keeps the pieces this composes
# (Physics::CreateHeightFieldShape and the mesh/meshlet/BLAS plumbing); this
# target owns the generation.
#
# Public headers are reached as <Terrain/...>.

set(TARGET_NAME zahlen_terrain)

file(GLOB ZHLN_TERRAIN_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(${TARGET_NAME} STATIC ${ZHLN_TERRAIN_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE.
    POSITION_INDEPENDENT_CODE ON
)

# Core is a public dependency: the spawners run on RenderContext, the ECS,
# PhysicsContext's heightfield support and the SystemGraphs extension seam.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

# TerrainFactory.cpp partitions baked heightfield meshes through the public
# <Zahlen/Meshlet.hpp>, whose inline BuildMeshlets calls meshopt_buildMeshlets.
# zahlen_engine links meshoptimizer PRIVATE (its own MeshBuilder is the other
# consumer), so the include path and the symbols have to be named here too --
# the same arrangement zcook uses for the identical reason.
target_link_libraries(${TARGET_NAME} PRIVATE
    meshoptimizer
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# TerrainSystem.cpp reaches Components.hpp, whose key bitset is sized by
# Reflect::EnumCount<KeyCode>(). Reflection is not a per-target feature --
# without the flags that count is 0 instead of 72 and any component after it
# moves, a silent wrong-offset read.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
