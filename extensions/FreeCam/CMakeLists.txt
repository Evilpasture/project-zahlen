# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/FreeCam -- first-person fly camera (zahlen_freecam).
#
# Core's FreeCamTagComponent is a marker. This target owns the speed knobs
# and the Camera-phase step that writes Camera pose from WASD/RMB.
#
# Public headers are reached as <FreeCam/FreeCam.hpp>.

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
