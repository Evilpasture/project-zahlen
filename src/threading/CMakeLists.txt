# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


add_library(zahlen_threading STATIC
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

# Publish only supported threading headers; fiber implementation files are
# local to this target.
target_include_directories(zahlen_threading
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Enable ASM support if not already inherited
set_target_properties(zahlen_threading PROPERTIES LINKER_LANGUAGE CXX)

if(NOT MSVC)
    target_compile_options(zahlen_threading PRIVATE -fno-exceptions -fno-rtti)
endif()
target_link_libraries(zahlen_threading PRIVATE zahlen_warnings)
