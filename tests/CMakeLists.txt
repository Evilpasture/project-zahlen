# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# 1. Enable CTest testing framework
enable_testing()

# 2. Automatically discover all test source files in this directory
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "Test*.cpp")

# 3. Iterate and dynamically register each test executable
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

    # --- ASAN & UBSAN CONFIGURATION ---
    if(USE_SANITIZERS)
        target_compile_options(${TARGET_NAME} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${TARGET_NAME} PRIVATE
            -fsanitize=address,undefined
        )
        target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)

        if(WIN32)
            get_filename_component(CLANG_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
            find_file(ASAN_DLL
                NAMES "clang_rt.asan_dynamic-x86_64.dll"
                PATHS "${CLANG_BIN_DIR}/../lib/clang/23/lib/windows" "${CLANG_BIN_DIR}/../lib/clang/22/lib/windows"
            )
            if(ASAN_DLL)
                add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ASAN_DLL}" "$<TARGET_FILE_DIR:${TARGET_NAME}>"
                    COMMENT "Copying ASan Runtime DLL for ${TARGET_NAME}..."
                )
            endif()
        endif()
    endif()

    # 4. Register the executable as a test with CTest
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})

    # Pass Sanitizer options to CTest runner so reports halt and print stack traces
    if(USE_SANITIZERS)
        set_tests_properties(${TARGET_NAME} PROPERTIES
            ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:symbolize=1:halt_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
        )
    endif()
endforeach()
