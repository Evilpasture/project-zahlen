# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Application composition lives outside Core: ordinary games link just this
# target, while bespoke hosts can still use zahlen_engine directly. The optional
# links/defines match what this build actually provides (including extras OFF).
add_library(zahlen_application STATIC Application.cpp)
set_target_properties(zahlen_application PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
# App.hpp exposes CommandHandler/CommandLineOptions, so its CLI include root
# must reach ordinary games through the application target's public interface.
target_link_libraries(zahlen_application
    PUBLIC zahlen_engine zahlen_engine_cli
    PRIVATE zahlen_signal zahlen_warnings
)
target_include_directories(zahlen_application PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/plugins
    ${PROJECT_SOURCE_DIR}/extensions
    ${PROJECT_SOURCE_DIR}/gameplay
)
target_compile_definitions(zahlen_application PRIVATE JPH_DOUBLE_PRECISION)
zahlen_enable_reflection(zahlen_application)

if(TARGET zahlen_fonts)
    target_link_libraries(zahlen_application PRIVATE zahlen_fonts)
    target_compile_definitions(zahlen_application PRIVATE ZHLN_APP_HAS_FONTS=1)
endif()
if(TARGET zahlen_character_controller)
    target_link_libraries(zahlen_application PRIVATE zahlen_character_controller)
    target_compile_definitions(zahlen_application PRIVATE ZHLN_APP_HAS_CHARACTER_CONTROLLER=1)
endif()
if(TARGET zahlen_camera)
    target_link_libraries(zahlen_application PRIVATE zahlen_camera)
    target_compile_definitions(zahlen_application PRIVATE ZHLN_APP_HAS_CAMERA=1)
endif()
if(TARGET zahlen_interaction)
    target_link_libraries(zahlen_application PRIVATE zahlen_interaction)
    target_compile_definitions(zahlen_application PRIVATE ZHLN_APP_HAS_INTERACTION=1)
endif()
if(TARGET zahlen_terrain)
    target_link_libraries(zahlen_application PRIVATE zahlen_terrain)
    target_compile_definitions(zahlen_application PRIVATE ZHLN_APP_HAS_TERRAIN=1)
endif()
if(TARGET zahlen_animation)
    target_link_libraries(zahlen_application PRIVATE zahlen_animation)
    target_compile_definitions(zahlen_application PRIVATE ZHLN_APP_HAS_ANIMATION=1)
endif()
if(TARGET zahlen_freecam)
    target_link_libraries(zahlen_application PRIVATE zahlen_freecam)
    target_compile_definitions(zahlen_application PRIVATE ZHLN_APP_HAS_FREECAM=1)
endif()
if(TARGET zahlen_ragdoll_authoring)
    target_link_libraries(zahlen_application PRIVATE zahlen_ragdoll_authoring)
    target_compile_definitions(zahlen_application PRIVATE ZHLN_APP_HAS_RAGDOLL_AUTHORING=1)
endif()
