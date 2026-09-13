# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/ALife -- the artificial-life simulation: factions, GOAP planning, a
# navigation graph, a spatial grid and the simulator that ticks them
# (zahlen_alife).
#
# The plainest target in extras/: no module interfaces, no third-party
# dependency, no serializer. It is simulation over Jolt vectors and the ECS
# component model -- it names no renderer type, no asset-pipeline type and no
# document type of its own. (Core's <Zahlen/Engine.hpp> does pull in
# <Zahlen/Render.hpp>, so the engine link below is unavoidable; what is true is
# that nothing in this directory asks for any of it.) Splitting it out is what
# lets a headless simulation harness link the simulator without dragging a glTF
# importer and a JSON parser into the link line.
#
# Public headers are reached as <ALife/...>.

set(TARGET_NAME zahlen_alife)

file(GLOB ZHLN_ALIFE_SOURCES
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

if(ZHLN_ALIFE_SOURCES)
    target_sources(${TARGET_NAME} PRIVATE ${ZHLN_ALIFE_SOURCES})
endif()

# Core is a public dependency: Simulator.hpp and Graph.hpp name ZHLN::Engine and
# ZHLN::Entity, and Simulator.cpp registers systems with the ECS, so a consumer
# needs core's include path and its symbols rather than just this archive.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# Nothing here reflects directly. The flags are still applied, the same way they
# are for every other target that sees a core header: ZHLN_ANNOTATION and
# Reflect::EnumCount resolve differently with and without __cpp_impl_reflection,
# and a target that disagrees with the core library it links is a silent
# wrong-offset read rather than a compile error.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
