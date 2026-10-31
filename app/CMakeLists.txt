# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Process/pace policy and the Kernel-only host are shared by the game and
# standalone 2D tools. This target has no optional gameplay dependencies.
add_library(zahlen_host_runner STATIC HostRunner.cpp KernelApp.cpp)
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
# Only app-owned targets publish the host-facing headers under app/include/.
target_include_directories(zahlen_host_runner
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_compile_definitions(zahlen_host_runner PRIVATE JPH_DOUBLE_PRECISION)
zahlen_enable_reflection(zahlen_host_runner)

# Application composition lives outside Core: ordinary games link just this
# target, while bespoke hosts can still use zahlen_engine directly. CMake
# leaves optional module composition to the consuming application.
add_library(zahlen_application STATIC Application.cpp)
set_target_properties(zahlen_application PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
# App.hpp exposes ConfigCli/CliFlag. Publish the generic CLI include root
# through the application target; no engine-specific CLI target exists.
target_link_libraries(zahlen_application
    PUBLIC zahlen_engine zahlen_command_line
    PRIVATE zahlen_host_runner zahlen_warnings
)
target_include_directories(zahlen_application
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/plugins
        ${PROJECT_SOURCE_DIR}/extensions
        ${PROJECT_SOURCE_DIR}/gameplay
)
target_compile_definitions(zahlen_application PRIVATE JPH_DOUBLE_PRECISION)
zahlen_enable_reflection(zahlen_application)

# Application is deliberately independent of which extras this checkout built.
# Compose gameplay explicitly with Application::Install in the consuming app.
set(AUTOLOAD_INCLUDES "")
set(AUTOLOAD_CALLS "")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/DefaultModules.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/DefaultModules.cpp"
    @ONLY
)
target_sources(zahlen_application PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/DefaultModules.cpp")

# The UI-tree editor uses the Kernel-only runner, not application gameplay.
if(TARGET zahlen_ui_editor)
    target_link_libraries(zahlen_ui_editor PRIVATE zahlen_host_runner)
endif()
