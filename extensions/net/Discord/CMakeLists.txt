# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
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

set(ENGINE_TARGET zahlen_discord_engine)
add_library(${ENGINE_TARGET} STATIC
    EngineIntegration.cpp
)

set_target_properties(${ENGINE_TARGET} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(${ENGINE_TARGET} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions/net
)

target_link_libraries(${ENGINE_TARGET} PUBLIC
    zahlen_discord_rpc
    zahlen_engine
    PRIVATE zahlen_warnings
)
