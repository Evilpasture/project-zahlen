# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Platform socket ownership, setup, and I/O. The backend is selected once here;
# consumers depend on the platform-neutral API and never include OS socket headers.
set(TARGET_NAME zahlen_socket)

if(WIN32)
    set(ZHLN_SOCKET_BACKEND SocketWin32.cpp)
else()
    set(ZHLN_SOCKET_BACKEND SocketPosix.cpp)
endif()

add_library(${TARGET_NAME} STATIC
    ${ZHLN_SOCKET_BACKEND}
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

if(ZHLN_HAVE_WINSOCK)
    target_link_libraries(${TARGET_NAME} PRIVATE ws2_32)
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
