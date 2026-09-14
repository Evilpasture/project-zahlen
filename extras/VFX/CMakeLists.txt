# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/VFX -- the visual-effects toolkits (zahlen_vfx).
#
# Three module interfaces: ZHLN.CombatFX and ZHLN.Explosions are interface-only
# (everything they export is defined in the interface), ZHLN.Lightning is backed
# by Lightning.cpp, its module implementation unit. The unit stays in this target
# with its interface: CMake orders an implementation unit against the BMI it
# belongs to, and that edge only exists inside one target.
#
# These are the extras that do reach for the renderer -- BufferHandle, MaterialID
# and the creative-works factory all appear in the exported structs -- so this
# target links core publicly and is deliberately not something a CPU-only
# consumer has to pull in.
#
# Public headers and module interfaces are reached as <VFX/...>.

set(TARGET_NAME zahlen_vfx)

file(GLOB ZHLN_VFX_MODULES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm"
)
file(GLOB ZHLN_VFX_SOURCES
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

if(ZHLN_VFX_MODULES)
    set_source_files_properties(${ZHLN_VFX_MODULES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    target_sources(${TARGET_NAME}
        PUBLIC
            FILE_SET CXX_MODULES
            TYPE CXX_MODULES
            BASE_DIRS ${PROJECT_SOURCE_DIR}/extras
            FILES ${ZHLN_VFX_MODULES}
    )
endif()

if(ZHLN_VFX_SOURCES)
    target_sources(${TARGET_NAME} PRIVATE ${ZHLN_VFX_SOURCES})
endif()

# Core is a public dependency: Lightning.cppm's exported LightningComponent
# carries BufferHandle, MaterialID and Entity members, and CombatFX.cppm and
# Explosion.cppm include <Zahlen/Components.hpp> and <Zahlen/Render.hpp>. A
# consumer needs core's include path and its symbols, not just this archive.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# CombatFX.cppm, Explosion.cppm and Lightning.cpp all include
# <Zahlen/Components.hpp>, whose InputStateComponent sizes its key bitset with
# Reflect::EnumCount<KeyCode>(). Build without the reflection flags and that
# count is 0 instead of 72, which shifts every following member.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
