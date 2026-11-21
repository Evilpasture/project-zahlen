# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# Shared process, CLI, signal, and frame-pacing utilities used by ZHLN::App
# and standalone tools, independent of optional UI and gameplay targets.
add_library(zahlen_host_runner STATIC HostRunner.cpp)
set_target_properties(zahlen_host_runner PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(zahlen_host_runner
    PUBLIC zahlen_engine zahlen_command_line
    PRIVATE zahlen_signal zahlen_warnings
)
target_include_directories(zahlen_host_runner
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
zahlen_enable_reflection(zahlen_host_runner)

# Unified COM application host: ships in both full and core-only builds with
# built-in --frames, --capture, and dynamic COM::IAppLayer --plugin loading,
# without pulling in immediate-mode UI or optional gameplay archives.
add_library(zahlen_application STATIC
    Application.cpp
    PluginManifest.cpp
    PluginManifestGraph.cpp
    PluginManifestRegistry.cpp
)
set_target_properties(zahlen_application PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_link_libraries(zahlen_application
    PUBLIC zahlen_engine zahlen_command_line
    PRIVATE zahlen_host_runner zahlen_screenshot zahlen_warnings
)
target_include_directories(zahlen_application
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
zahlen_enable_reflection(zahlen_application)

if(TARGET zahlen_ui_editor)
    target_link_libraries(zahlen_ui_editor PRIVATE zahlen_application zahlen_host_runner)
endif()
