# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Optional native client for project-light's existing Python server protocol.
# This is deliberately separate from ZHLN.Network: project-light uses
# MessagePack + zlib, not the Zahlen binary Wire v2 protocol.

find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    message(STATUS "Skipping zahlen_project_light_client: zlib development files were not found")
    return()
endif()

if(NOT TARGET zahlen_character_controller OR NOT TARGET zahlen_camera)
    message(STATUS "Skipping zahlen_project_light_client: CharacterController and Camera extras are required")
    return()
endif()

add_library(zahlen_project_light_client STATIC EXCLUDE_FROM_ALL
    ClientConfig.cpp
    ProjectLightClient.cpp
    LightProtocol.cpp
)

set_target_properties(zahlen_project_light_client PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(zahlen_project_light_client PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

target_link_libraries(zahlen_project_light_client
    PUBLIC zahlen_engine
    PRIVATE zahlen_character_controller zahlen_camera ZLIB::ZLIB
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_project_light_client)
endif()

target_link_libraries(zahlen_project_light_client PRIVATE zahlen_warnings)

if(ZHLN_HAVE_WINSOCK)
    target_link_libraries(zahlen_project_light_client PRIVATE ws2_32)
endif()
