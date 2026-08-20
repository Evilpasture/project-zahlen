# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# 1. Enable CTest testing framework
enable_testing()

# 2. Automatically discover all test source files in this directory
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "Test*.cpp")
if(NOT TARGET zahlen_extras)
    list(FILTER TEST_SOURCES EXCLUDE REGEX "/TestProceduralAnimation\\.cpp$")
endif()

# 3. Iterate and dynamically register each test executable with original casing preserved
foreach(TEST_SRC IN LISTS TEST_SOURCES)
    # Extract file name without path or extension (e.g., "TestTaskSystem", "TestJSON", "TestECS")
    get_filename_component(TARGET_NAME ${TEST_SRC} NAME_WLE)

    add_executable(${TARGET_NAME} ${TEST_SRC})

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_engine
    )
    if("${TARGET_NAME}" STREQUAL "TestProceduralAnimation" AND TARGET zahlen_extras)
        target_link_libraries(${TARGET_NAME} PRIVATE zahlen_extras)
        target_sources(${TARGET_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/extern/cgltf_impl.c)
        target_include_directories(${TARGET_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/extern/cgltf)
        target_compile_definitions(${TARGET_NAME} PRIVATE ZHLN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    endif()

    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}/tests
    )

    if(NOT MSVC)
        # A C++ module consumer must use the same exception/RTTI ABI mode as the
        # PCM producer. zahlen_extras currently builds with the compiler default
        # (exceptions + RTTI), so keep its module integration test compatible.
        if("${TARGET_NAME}" STREQUAL "TestProceduralAnimation" AND TARGET zahlen_extras)
            target_compile_options(${TARGET_NAME} PRIVATE -fexceptions -frtti)
        else()
            target_compile_options(${TARGET_NAME} PRIVATE -fno-exceptions -fno-rtti)
        endif()
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

    # 4. Register the executable as a test with CTest (using original casing)
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    set_tests_properties(${TARGET_NAME} PROPERTIES LABELS "CPU;Fast")

    # Pass Sanitizer options to CTest runner so reports halt and print stack traces
    if(USE_SANITIZERS)
        set_tests_properties(${TARGET_NAME} PROPERTIES
            ENVIRONMENT "${ZHLN_TEST_ENVIRONMENT}"
        )
    endif()
endforeach()

# 5. Local GPU Render Test Suite (Off by default for headless CI/CD)
option(ZHLN_BUILD_GPU_TESTS "Build GPU/Vulkan rendering tests (local hardware required)" OFF)

if(ZHLN_BUILD_GPU_TESTS)
    message(STATUS "Configuring local GPU render tests in tests/render...")
    add_subdirectory(render)
endif()


