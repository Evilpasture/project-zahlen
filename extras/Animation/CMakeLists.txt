# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/Animation -- the procedural animation rig (zahlen_animation).
#
# Its own target rather than a corner of the extras aggregate because nothing in
# it needs anything the other domains carry. The four module interfaces
# (ZHLN.Rig, ZHLN.Animator, ZHLN.Locomotion, ZHLN.ProceduralAnimation) and the
# four implementation units behind them are rig maths over Jolt vectors, the IK
# solver in core, and the ECS. A consumer that wants the gait solver gets no
# simdjson, no sockets, no cgltf and no Lua runtime along with it.
#
# Discovery is local and non-recursive: ../CMakeLists.txt no longer globs this
# tree, so what compiles into this target is decided here and nowhere else.
#
# Public headers and module interfaces are reached as <Animation/...>.

set(TARGET_NAME zahlen_animation)

file(GLOB ZHLN_ANIMATION_MODULES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm"
)
file(GLOB ZHLN_ANIMATION_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(${TARGET_NAME} STATIC)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE.
    POSITION_INDEPENDENT_CODE ON
)

if(ZHLN_ANIMATION_MODULES)
    # 'module;' has to be the first thing the compiler sees, so no precompiled
    # header may ever be injected ahead of it. This target has no PCH of its
    # own; the property is what keeps that true if one is ever added here.
    set_source_files_properties(${ZHLN_ANIMATION_MODULES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    # BASE_DIRS is the extras root rather than this directory, which keeps the
    # BMI paths ("Animation/Animator.cppm" and friends) identical to the ones
    # the single extras archive used to produce.
    target_sources(${TARGET_NAME}
        PUBLIC
            FILE_SET CXX_MODULES
            TYPE CXX_MODULES
            BASE_DIRS ${PROJECT_SOURCE_DIR}/extras
            FILES ${ZHLN_ANIMATION_MODULES}
    )
endif()

if(ZHLN_ANIMATION_SOURCES)
    target_sources(${TARGET_NAME} PRIVATE ${ZHLN_ANIMATION_SOURCES})
endif()

# Core is a public dependency: Animator.cppm's global module fragment includes
# <Zahlen/IK.hpp>, Locomotion.cppm includes <Zahlen/Components.hpp>, and the
# exported signatures name ZHLN::Engine and ECS handles. A consumer needs core's
# include path and its symbols, not just this archive.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# Locomotion.cppm, ProceduralAnimation.cpp and ProceduralItemHandling.cpp all
# include <Zahlen/Components.hpp>, whose InputStateComponent sizes its key
# bitset with Reflect::EnumCount<KeyCode>(). Reflection is not a per-target
# feature -- without the flags that count is 0 instead of 72 and mouseX moves
# four bytes, which is a silent wrong-offset read rather than a compile error.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
