# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/DataModel -- stable Instance identities and the project-light object
# graph. This is the C++ OOP layer only; Lua bindings and ECS synchronization
# are deliberately separate consumers.

set(TARGET_NAME zahlen_datamodel)

add_library(${TARGET_NAME} STATIC
    DataModel.cpp
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

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

# Typed runtime class names are derived from their reflected C++ types.
zahlen_enable_reflection(${TARGET_NAME})
