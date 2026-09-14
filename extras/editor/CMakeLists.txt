# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/editor -- the native world editor (Hierarchy + Inspector).
#
# A separate STATIC target, not part of the zahlen_extras aggregate: the composition
# root links it only when present, and a host that does not want the editor
# is not forced to compile it. Public headers are included as
# <editor/GUIEditor.hpp>.

set(TARGET_NAME zahlen_editor)

add_library(${TARGET_NAME} STATIC
    GUIEditor.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
