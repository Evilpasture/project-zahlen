# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


# OBJECT library (like zahlen_ecs and zahlen_gui): its translation units define
# public ZHLN_API runtime symbols (Mutex, SharedMutex, ConditionalVariable,
# Fiber, TaskSystem) and must enter libzahlen_engine directly so neither archive
# dead-stripping nor -Wl,--exclude-libs,ALL can drop or hide them. Do not name
# zahlen_threading in target_link_libraries of sibling STATIC archives -- CMake
# would copy its objects into those archives.
add_library(zahlen_threading OBJECT
    Thread.cpp
    Thread.S
    Mutex.cpp
    TaskSystem.cpp
)
set_target_properties(zahlen_threading PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(zahlen_threading
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(zahlen_threading PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(zahlen_threading PRIVATE zahlen_warnings)
