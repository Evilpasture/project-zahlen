# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/glTF -- the glTF/GLB importer and the drop-a-file inspector
# (zahlen_gltf).
#
# This is the extra that owns vendored third-party code, and owning it in a
# target of its own is what keeps that code off everyone else's include path:
#
#   * cgltf         -- the container parser, header plus extern/cgltf_impl.c. It
#                      used to live in the src/gltf static library; this target
#                      compiles the one copy in the tree, so nothing else may.
#   * stb_image     -- header only. STB_IMAGE_IMPLEMENTATION is
#                      extern/stbi_impl.c, which is part of zahlen_engine, so
#                      the decode symbols arrive through the engine link below.
#   * meshoptimizer -- Zahlen/Meshlet.hpp's inline BuildMeshlets() calls into it;
#                      zahlen_engine links it PRIVATE, so it does not propagate
#                      and this target links it again.
#
# The importer is the one extra that produces data core then consumes: it fills
# the prefab cache, and CreativeWorksFactory::LoadModelPrefab reads that cache.
# No registration, and nothing in src/ includes it.
#
# ZHLN.glTF (glTF.cppm) is the inspector's interface and glTF.cpp is its module
# implementation unit. GLTFImporter.cpp is an ordinary translation unit, not a
# module unit, and it is the file that needs the two other targets below.
#
# Public headers and module interfaces are reached as <glTF/...>.

set(TARGET_NAME zahlen_gltf)

file(GLOB ZHLN_GLTF_MODULES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm"
)
file(GLOB ZHLN_GLTF_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(${TARGET_NAME} STATIC)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Matters more here than anywhere else in extras/: extern/cgltf_impl.c is a C
    # translation unit compiled into this archive, and samples and tests link it
    # straight into a PIE executable.
    POSITION_INDEPENDENT_CODE ON
)

if(ZHLN_GLTF_MODULES)
    set_source_files_properties(${ZHLN_GLTF_MODULES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    target_sources(${TARGET_NAME}
        PUBLIC
            FILE_SET CXX_MODULES
            TYPE CXX_MODULES
            BASE_DIRS ${PROJECT_SOURCE_DIR}/extras
            FILES ${ZHLN_GLTF_MODULES}
    )
endif()

if(ZHLN_GLTF_SOURCES)
    target_sources(${TARGET_NAME} PRIVATE ${ZHLN_GLTF_SOURCES})
endif()

# The cgltf implementation. PRIVATE, and exactly one target in the tree compiles
# it: a second copy is a duplicate definition at link time, which is why
# tests/render/CMakeLists.txt and tests/extras/CMakeLists.txt deliberately do
# not list it either.
target_sources(${TARGET_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/extern/cgltf_impl.c)

target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/cgltf
    ${PROJECT_SOURCE_DIR}/extern/stb
)

target_link_libraries(${TARGET_NAME} PRIVATE meshoptimizer)

# Core is a public dependency: <glTF/GLTFImporter.hpp> names ZHLN::ModelPrefab,
# RenderContext and CreativeWorksManager in its signatures.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

# GLTFImporter.cpp includes <json/JSONSchema.hpp> to validate the container it is
# about to parse. That is an implementation detail of the .cpp -- neither
# <glTF/GLTFImporter.hpp> nor glTF.cppm mentions JSON -- so the link is PRIVATE:
# a consumer of the importer needs no JSON reader of its own, and a consumer
# that wants one links zahlen_serialization itself.
target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_serialization
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# glTF.cpp includes <Zahlen/Components.hpp>, whose layout depends on reflection.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
