# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/CharacterController -- COM host character locomotion (zahlen_character_controller).
# Public headers are reached as <CharacterController/...>.

add_library(zahlen_character_controller STATIC CharacterController.cpp)

set_target_properties(zahlen_character_controller PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(zahlen_character_controller
    PUBLIC zahlen_engine
    PRIVATE zahlen_warnings
)

target_include_directories(zahlen_character_controller PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_character_controller)
endif()

if(COMMAND zahlen_add_plugin)
    zahlen_add_plugin(zahlen_plugin_character_controller character_controller
        CharacterControllerPlugin.cpp)
    target_link_libraries(zahlen_plugin_character_controller PRIVATE zahlen_character_controller)
    target_include_directories(zahlen_plugin_character_controller PUBLIC
        ${PROJECT_SOURCE_DIR}/extensions)
endif()

configure_file(zahlen_plugin_character_controller.manifest.toml
    ${CMAKE_BINARY_DIR}/lib/zahlen_plugin_character_controller.manifest.toml COPYONLY)
