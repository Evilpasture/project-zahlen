# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

function(_zhln_configure_animation_target tgt)
    set_target_properties(${tgt} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES ON
        POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(${tgt} PUBLIC ${PROJECT_SOURCE_DIR}/extensions)
    target_link_libraries(${tgt} PRIVATE zahlen_warnings)
    if(COMMAND zahlen_enable_reflection)
        zahlen_enable_reflection(${tgt})
    endif()
endfunction()

add_library(zahlen_animation_controls STATIC Animator.cpp)
set_source_files_properties(Animator.cppm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_sources(zahlen_animation_controls
    PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES
    BASE_DIRS ${PROJECT_SOURCE_DIR}/extensions/Animation
    FILES Animator.cppm
)
target_link_libraries(zahlen_animation_controls PUBLIC zahlen_engine)
_zhln_configure_animation_target(zahlen_animation_controls)

add_library(zahlen_animation STATIC
    Context.cpp
    AnimationSystem.cpp
    TwoBoneIK.cpp
)
set_source_files_properties(Rig.cppm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_sources(zahlen_animation
    PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES
    BASE_DIRS ${PROJECT_SOURCE_DIR}/extensions/Animation
    FILES Rig.cppm
)
target_link_libraries(zahlen_animation PUBLIC zahlen_engine zahlen_animation_controls)
_zhln_configure_animation_target(zahlen_animation)

add_library(zahlen_locomotion STATIC LocomotionLifecycle.cpp)
set_source_files_properties(Locomotion.cppm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_sources(zahlen_locomotion
    PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES
    BASE_DIRS ${PROJECT_SOURCE_DIR}/extensions/Animation
    FILES Locomotion.cppm
)
target_link_libraries(zahlen_locomotion PUBLIC
    zahlen_animation
    zahlen_character_controller
    zahlen_camera
    zahlen_freecam
)
_zhln_configure_animation_target(zahlen_locomotion)

add_library(zahlen_procedural_animation STATIC
    ProceduralAnimation.cpp
    ProceduralGait.cpp
    ProceduralItemHandling.cpp
    ProceduralSecondaryPhysics.cpp
)
set_source_files_properties(ProceduralAnimation.cppm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_sources(zahlen_procedural_animation
    PUBLIC FILE_SET CXX_MODULES TYPE CXX_MODULES
    BASE_DIRS ${PROJECT_SOURCE_DIR}/extensions/Animation
    FILES ProceduralAnimation.cppm
)
target_link_libraries(zahlen_procedural_animation PUBLIC
    zahlen_animation
    zahlen_character_controller
)
_zhln_configure_animation_target(zahlen_procedural_animation)
