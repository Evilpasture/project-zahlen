# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Encoding and file I/O live outside the renderer and application. stb's
# implementation and include path are private to this reusable extension.
add_library(zahlen_screenshot STATIC Screenshot.cpp)
set_target_properties(zahlen_screenshot PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(zahlen_screenshot
    PUBLIC ${PROJECT_SOURCE_DIR}/extensions
    PRIVATE ${PROJECT_SOURCE_DIR}/extern/stb
)
target_link_libraries(zahlen_screenshot PUBLIC zahlen_engine PRIVATE zahlen_warnings)
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_screenshot)
endif()
