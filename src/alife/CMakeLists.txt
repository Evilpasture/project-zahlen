# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# src/alife/CMakeLists.txt

set(ALIFE_SOURCES
    Factions.cpp
    GOAP.cpp
    Graph.cpp
    Simulator.cpp
    SpatialGrid.cpp
)

add_library(zahlen_alife OBJECT ${ALIFE_SOURCES})
set_target_properties(zahlen_alife PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Disable MSVC warnings for secure CRT functions if compiling on Windows
target_compile_definitions(zahlen_alife PRIVATE _CRT_SECURE_NO_WARNINGS)

# Include directories
target_include_directories(zahlen_alife PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
)

target_include_directories(zahlen_alife PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

# ALife inherently relies on the ECS and Jolt Physics (for RVec3, math, etc.)
target_link_libraries(zahlen_alife PUBLIC
    zahlen_ecs
    Jolt
)

if(NOT MSVC)
    target_compile_options(zahlen_alife PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
endif()
