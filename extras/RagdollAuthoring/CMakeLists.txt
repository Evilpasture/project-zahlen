# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/RagdollAuthoring -- procedural ragdoll authoring
# (zahlen_ragdoll_authoring).
#
# The heuristics that decide collider shapes, masses and joint limits for a
# humanoid biped from joint names. Authoring is data production, not
# simulation: the core ArticulationSystem only executes the authored
# Physics::RagdollPartParams list this module produces (or a cooked asset or
# scene document replaces it with).
#
# Public headers are reached as <RagdollAuthoring/...>.

set(TARGET_NAME zahlen_ragdoll_authoring)

file(GLOB ZHLN_RAGDOLL_AUTHORING_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(${TARGET_NAME} STATIC ${ZHLN_RAGDOLL_AUTHORING_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE.
    POSITION_INDEPENDENT_CODE ON
)

# Core is a public dependency: the module resolves rigs through the ECS and
# PhysicsContext, and attaches the result through the articulation system.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# The articulation system is a core system class, not a public API: the
# module attaches through its header, the same way the scripting extra drives
# the animation and lighting systems.
target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
