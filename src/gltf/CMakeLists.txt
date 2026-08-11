# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(zahlen_gltf STATIC
    GLTFImporter.cpp
    ${PROJECT_SOURCE_DIR}/extern/cgltf_impl.c
)
set_target_properties(zahlen_gltf PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(zahlen_gltf PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/extern/cgltf
    ${PROJECT_SOURCE_DIR}/extern/stb
)

# Pull in PkgConfig/Homebrew simdjson include paths
if(SIMDJSON_INCLUDE_DIRS)
    target_include_directories(zahlen_gltf PRIVATE ${SIMDJSON_INCLUDE_DIRS})
endif()

target_link_libraries(zahlen_gltf PRIVATE
    zahlen_render
    zahlen_physics
    zahlen_threading
    Jolt
    PkgConfig::SIMDJSON # <-- ADD THIS
)

if(NOT MSVC)
    target_compile_options(zahlen_gltf PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
endif()
