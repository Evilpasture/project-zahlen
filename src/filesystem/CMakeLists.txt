# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


# OBJECT library (like zahlen_ecs, zahlen_gui, and zahlen_threading): defines
# public ZHLN_API filesystem symbols (FileSystemWatcher, MappedFile, Paths,
# VirtualFileSystem) owned by libzahlen_engine, so its objects must enter the
# shared library directly rather than through a -Wl,--exclude-libs,ALL archive.
add_library(zahlen_filesystem OBJECT
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
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(zahlen_filesystem
    PRIVATE
        zahlen_warnings
)

if(DEFINED ZSTD_LIBRARY)
    target_link_libraries(zahlen_filesystem PRIVATE ${ZSTD_LIBRARY})
else()
    find_package(ZSTD QUIET)
    if(ZSTD_FOUND)
        target_link_libraries(zahlen_filesystem PRIVATE ZSTD::ZSTD)
    endif()
endif()

target_compile_options(zahlen_filesystem PRIVATE -fno-exceptions -fno-rtti)
