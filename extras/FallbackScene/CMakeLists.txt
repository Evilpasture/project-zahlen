# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/FallbackScene -- the compiled-in fail-safe scene (zahlen_fallback_scene).
#
# Boot-failure detection plus the demo scene and popup UI it engages: the
# engine's own visual representation, not substrate. Core keeps what this
# composes -- Scene::Instantiate, the frame-scheduler extension seam, the
# enableFallbackScene config flag and the teardown-hook list -- and a host
# that wants no fallback simply never calls Install.
#
# Public headers are reached as <FallbackScene/...>.

set(TARGET_NAME zahlen_fallback_scene)

file(GLOB ZHLN_FALLBACK_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(${TARGET_NAME} STATIC ${ZHLN_FALLBACK_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE.
    POSITION_INDEPENDENT_CODE ON
)

# Core is a public dependency: the preset instantiates through Scene, drives
# the immediate-mode GUI, reads InputStateComponent and subscribes engine
# teardown.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

# FallbackScene.cpp reaches Components.hpp, whose key bitset is sized by
# Reflect::EnumCount<KeyCode>(). Reflection is not a per-target feature --
# without the flags that count is 0 instead of 72 and any component after it
# moves, a silent wrong-offset read.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
