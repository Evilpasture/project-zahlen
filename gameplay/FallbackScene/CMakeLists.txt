# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Host-owned diagnostic scene content and immediate UI. No native Module.

set(TARGET_NAME zahlen_fallback_scene)

set(ZHLN_FALLBACK_SOURCES
    FallbackScene.cpp
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

target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine zahlen_scene zahlen_ui
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/gameplay
)
target_compile_definitions(${TARGET_NAME} PRIVATE
    ZHLN_TARGET_TRIPLE="${ZHLN_TARGET_TRIPLE}"
)

# Reflection is a whole-program ABI contract across targets that share ECS
# component registration and reflected type names with zahlen_engine.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
