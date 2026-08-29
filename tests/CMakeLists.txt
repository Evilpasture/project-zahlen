# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Tests exercise public behaviour only. They must not include engine internals
# from src/ (systems, render/Vulkan guts, cooker headers, ...). Configure-time
# enforcement lives in tools/check_tests_public_api.py.

# 1. Enable CTest testing framework
enable_testing()

# 2. Automatically discover all test source files in this directory
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "Test*.cpp")

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
    # Public headers only. src/ is banned — see tools/check_tests_public_api.py.
    #
    # extern/stb is here for the same reason it is in tests/render: the noise
    # metric suite decodes the shipped LDR_RGBA_0 tile with stb_image so the
    # discriminator is measured against the real asset rather than a synthetic
    # stand-in.
    #
    # Only the header is wanted. STB_IMAGE_IMPLEMENTATION already lives in
    # extern/stbi_impl.c, which is part of ENGINE_SOURCES, so the decode
    # functions arrive through the zahlen_engine link below: no visibility
    # preset is set on that target and no -fvisibility=hidden appears anywhere
    # in the CMake tree, so its extern symbols are exported by the ELF default.
    # Do not define STB_IMAGE_IMPLEMENTATION in a test TU.
    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/extern/stb
    )

    # include/Zahlen/Meshlet.hpp is a public header, but it includes
    # <meshoptimizer.h> and its inline BuildMeshlets() calls into the library.
    # zahlen_engine links meshoptimizer PRIVATE (root CMakeLists.txt:502), so
    # neither the include path nor the symbols propagate to test targets through
    # the engine link. Any test exercising BuildMeshlets must link it directly.
    if(TARGET_NAME STREQUAL "TestMeshlets")
        target_link_libraries(${TARGET_NAME} PRIVATE meshoptimizer)
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

# Optional CPU tests live with the extras toolkit and disappear completely when
# ZHLN_BUILD_EXTRAS is disabled.
if(ZHLN_BUILD_EXTRAS AND TARGET zahlen_extras)
    add_subdirectory(extras)
endif()

# 5. Local GPU Render Test Suite (Off by default for headless CI/CD)
option(ZHLN_BUILD_GPU_TESTS "Build GPU/Vulkan rendering tests (local hardware required)" OFF)

if(ZHLN_BUILD_GPU_TESTS)
    message(STATUS "Configuring local GPU render tests in tests/render...")
    add_subdirectory(render)
endif()


