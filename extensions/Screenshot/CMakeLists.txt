# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# Capture conversion lives outside Core. Encoding is delegated to Image;
# no third-party codec headers or symbols are exposed by this extension.
add_library(zahlen_screenshot STATIC Screenshot.cpp)
set_target_properties(zahlen_screenshot PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(zahlen_screenshot
    PUBLIC ${PROJECT_SOURCE_DIR}/extensions
)
target_link_libraries(zahlen_screenshot PUBLIC zahlen_engine PRIVATE zahlen_image zahlen_warnings)
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_screenshot)
endif()

if(COMMAND zahlen_add_plugin)
    zahlen_add_plugin(zahlen_plugin_screenshot screenshot ScreenshotPlugin.cpp)
    target_link_libraries(zahlen_plugin_screenshot PRIVATE zahlen_screenshot)
endif()
