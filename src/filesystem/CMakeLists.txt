# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# zahlen_filesystem — low-level VFS, pak archive, file watching, paths
# No Vulkan, no Jolt, no ECS, no audio. Only threading + zstd + core headers.

add_library(zahlen_filesystem STATIC
    FileSystemWatcher.cpp
    RuntimePaths.cpp
    VirtualFileSystem.cpp
    MappedFile.cpp
)

set_target_properties(zahlen_filesystem PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(zahlen_filesystem
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(zahlen_filesystem
    PUBLIC
        zahlen_threading
    PRIVATE
        zahlen_warnings
)

# zstd is used for pak decompression — link via the existing variable or imported target
if(DEFINED ZSTD_LIBRARY)
    target_link_libraries(zahlen_filesystem PRIVATE ${ZSTD_LIBRARY})
else()
    find_package(ZSTD QUIET)
    if(ZSTD_FOUND)
        target_link_libraries(zahlen_filesystem PRIVATE ZSTD::ZSTD)
    endif()
endif()

if(NOT MSVC)
    target_compile_options(zahlen_filesystem PRIVATE -fno-exceptions -fno-rtti)
endif()
