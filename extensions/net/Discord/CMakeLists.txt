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

