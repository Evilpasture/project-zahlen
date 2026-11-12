# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/VFX -- the visual-effects toolkits (zahlen_vfx).
#
# Three module interfaces: CombatFX and Explosions export their content from
# interfaces; Lightning has a module implementation. The separate typed
# update units import their respective module interfaces. All live in this
# target so CMake can order implementation/importers against the BMIs.
#
# These are the extensions that do reach for the renderer -- BufferID, MaterialID
# and the creative-works factory all appear in the exported structs -- so this
# target links core publicly and is deliberately not something a CPU-only
# consumer has to pull in.
#
# Public headers and module interfaces are reached as <VFX/...>.

set(TARGET_NAME zahlen_vfx)

# Module interface units, named.
set(ZHLN_VFX_MODULES
    CombatFX.cppm
    Explosion.cppm
    Lightning.cppm
)

# The ordinary translation units of this target, named.
set(ZHLN_VFX_SOURCES
    Lightning.cpp
    LightningUpdate.cpp
    Explosion.cpp
)

add_library(${TARGET_NAME} STATIC)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON
    POSITION_INDEPENDENT_CODE ON
)

set_source_files_properties(${ZHLN_VFX_MODULES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_sources(${TARGET_NAME}
    PUBLIC
        FILE_SET CXX_MODULES
        TYPE CXX_MODULES
        BASE_DIRS ${PROJECT_SOURCE_DIR}/extensions
        FILES ${ZHLN_VFX_MODULES}
)

target_sources(${TARGET_NAME} PRIVATE ${ZHLN_VFX_SOURCES})

# Core is a public dependency: Lightning.cppm's exported LightningComponent
# carries BufferID, MaterialID and Entity members, and CombatFX.cppm and
# Explosion.cppm include <Zahlen/ECS/Components.hpp> and <Zahlen/Render/Render.hpp>. A
# consumer needs core's include path and its symbols, not just this archive.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

# CombatFX.cppm, Explosion.cppm and Lightning.cpp all include
# <Zahlen/ECS/Components.hpp>, whose InputStateComponent sizes its key bitset with
# Reflect::EnumCount<KeyCode>(). Build without the reflection flags and that
# count is 0 instead of 72, which shifts every following member.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
