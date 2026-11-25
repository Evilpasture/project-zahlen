# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

set(TARGET_NAME zahlen_discord_rpc)

add_library(${TARGET_NAME} STATIC
    RichPresence.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions/net
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_serialization
    zahlen_socket
    zahlen_warnings
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

if(COMMAND zahlen_add_plugin)
    zahlen_add_plugin(zahlen_plugin_discord_rpc discord_rpc DiscordPlugin.cpp)
    target_link_libraries(zahlen_plugin_discord_rpc PRIVATE ${TARGET_NAME})
endif()

# Copy the manifest beside the compiled plugin in the build tree so
# PluginManifestRegistry discovers it without an install step.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/zahlen_plugin_discord_rpc.manifest.toml
    ${CMAKE_BINARY_DIR}/lib/zahlen_plugin_discord_rpc.manifest.toml
    COPYONLY
)

