# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/Animation -- skeletal animation domain + procedural animation rig.
#
# Split into two targets so header-only / non-module consumers (samples, render
# tests, scripting) get AnimationContext, AnimationSystem, and TwoBoneIK without
# pulling in C++20 module BMIs or character-controller/camera dependencies:
#   - zahlen_skeletal_animation: AnimationContext, AnimationSystem, TwoBoneIK
#   - zahlen_animation:          ZHLN.Rig, ZHLN.Animator, ZHLN.Locomotion,
#                                ZHLN.ProceduralAnimation + procedural solvers
#
# Public headers and module interfaces are reached as <Animation/...>.

# ---------------------------------------------------------------------------
# 1. Skeletal animation domain + ECS bridge + TwoBoneIK (no C++20 modules)
# ---------------------------------------------------------------------------
set(SKELETAL_TARGET zahlen_skeletal_animation)

add_library(${SKELETAL_TARGET} STATIC
    AnimationContext.cpp
    AnimationSystem.cpp
    TwoBoneIK.cpp
)

set_target_properties(${SKELETAL_TARGET} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(${SKELETAL_TARGET} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

target_link_libraries(${SKELETAL_TARGET} PUBLIC
    zahlen_engine
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${SKELETAL_TARGET})
endif()

target_link_libraries(${SKELETAL_TARGET} PRIVATE zahlen_warnings)

# ---------------------------------------------------------------------------
# 2. Procedural animation rig + C++20 modules (re-exports skeletal animation)
# ---------------------------------------------------------------------------
set(TARGET_NAME zahlen_animation)

set(ZHLN_ANIMATION_MODULES
    Animator.cppm
    Locomotion.cppm
    ProceduralAnimation.cppm
    Rig.cppm
)

set(ZHLN_ANIMATION_SOURCES
    ProceduralAnimation.cpp
    ProceduralGait.cpp
    ProceduralItemHandling.cpp
    ProceduralSecondaryPhysics.cpp
)

add_library(${TARGET_NAME} STATIC)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON
    POSITION_INDEPENDENT_CODE ON
)

set_source_files_properties(${ZHLN_ANIMATION_MODULES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_sources(${TARGET_NAME}
    PUBLIC
        FILE_SET CXX_MODULES
        TYPE CXX_MODULES
        BASE_DIRS ${PROJECT_SOURCE_DIR}/extensions
        FILES ${ZHLN_ANIMATION_MODULES}
)

target_sources(${TARGET_NAME} PRIVATE ${ZHLN_ANIMATION_SOURCES})

target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_skeletal_animation
    zahlen_engine
    zahlen_character_controller
    zahlen_camera
    zahlen_freecam
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
