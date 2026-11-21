# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# First-person fly camera policy, explicitly driven by a COM host.

set(TARGET_NAME zahlen_freecam)

add_library(${TARGET_NAME} STATIC
    FreeCam.cpp
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
    ${PROJECT_SOURCE_DIR}/extensions
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

if(COMMAND zahlen_add_plugin)
    zahlen_add_plugin(zahlen_plugin_freecam freecam FreeCamPlugin.cpp)
    target_link_libraries(zahlen_plugin_freecam PRIVATE zahlen_freecam)
endif()
