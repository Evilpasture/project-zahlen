# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.28)

# 1. Automatically discover all test source files in this directory
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "Test*.cpp")

# 2. Iterate and dynamically register each test executable
foreach(TEST_SRC IN LISTS TEST_SOURCES)
    # Extract file name without path or extension (e.g., "TestTaskSystem")
    get_filename_component(TEST_FILE_WE ${TEST_SRC} NAME_WLE)

    # Convert PascalCase (TestTaskSystem) to snake_case (test_task_system)
    string(REGEX REPLACE "([A-Z])" "_\\1" TARGET_NAME "${TEST_FILE_WE}")
    string(TOLOWER "${TARGET_NAME}" TARGET_NAME)
    string(REGEX REPLACE "^_" "" TARGET_NAME "${TARGET_NAME}")

    add_executable(${TARGET_NAME} ${TEST_SRC})

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_engine
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
    )

    if(NOT MSVC)
        target_compile_options(${TARGET_NAME} PRIVATE -fno-exceptions -fno-rtti)
    endif()

    # Applies the required reflection flags (-freflection / custom libc++) configured in the root
    zahlen_enable_reflection(${TARGET_NAME})
endforeach()
