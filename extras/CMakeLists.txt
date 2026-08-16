# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

option(ZHLN_BUILD_EXTRAS "Build first-party engine feature modules and VFX toolkits" ON)

if(NOT ZHLN_BUILD_EXTRAS)
    return()
endif()

# 1. Automatically discover all C++26 module interface files (.cppm / .ixx)
file(GLOB_RECURSE ZHLN_EXTRAS_MODULES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.ixx"
)

# Gait and SecondaryPhysics are consumed by an engine system and are therefore
# registered on zahlen_engine to avoid a zahlen_engine <-> zahlen_extras cycle.
list(FILTER ZHLN_EXTRAS_MODULES EXCLUDE REGEX "/Animation/(Gait|SecondaryPhysics)\\.cppm$")

# 2. Automatically discover any accompanying C/C++ implementation files (.cpp / .c)
file(GLOB_RECURSE ZHLN_EXTRAS_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)

if(NOT ZHLN_EXTRAS_MODULES AND NOT ZHLN_EXTRAS_SOURCES)
    message(STATUS "[extras] No module files found in ${CMAKE_CURRENT_SOURCE_DIR}. Skipping target creation.")
    return()
endif()

add_library(zahlen_extras STATIC)

set_target_properties(zahlen_extras PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# 3. Prevent PCH injection on C++26 module interface files
if(ZHLN_EXTRAS_MODULES)
    set_source_files_properties(${ZHLN_EXTRAS_MODULES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    target_sources(zahlen_extras
        PUBLIC
            FILE_SET CXX_MODULES
            TYPE CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
            FILES ${ZHLN_EXTRAS_MODULES}
    )
endif()

# 4. Attach regular implementation source files if any exist
if(ZHLN_EXTRAS_SOURCES)
    target_sources(zahlen_extras PRIVATE ${ZHLN_EXTRAS_SOURCES})
endif()

# 5. Link against engine core, reflection, and warning configurations
target_link_libraries(zahlen_extras PUBLIC zahlen_engine)
target_link_libraries(zahlen_extras PRIVATE zahlen_warnings)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_extras)
endif()
