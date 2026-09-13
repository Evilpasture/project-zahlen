# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Every suite in this directory is a VFX suite -- TestCombatFX imports
# ZHLN.CombatFX, TestExplosion imports ZHLN.Explosions, TestLightning imports
# ZHLN.Lightning -- so the loop links zahlen_vfx and the guard is on that target.
# A future suite here that reaches into another domain needs its own link added
# explicitly; the list below deliberately does not hand every suite all of extras.
if(NOT ZHLN_BUILD_EXTRAS OR NOT TARGET zahlen_vfx)
    return()
endif()

# One executable per suite rather than one binary for the directory, so a
# device-lost or a timeout in one VFX suite does not take the others with it.
#
# Sources are listed, not globbed: a new Test*.cpp dropped in here is silently
# unbuilt until it is added to this list. The loop stays because the per-target
# setup below -- reflection flags, sanitizer wiring, the CTest entry and its
# GLIBC_TUNABLES pin -- is identical for every suite and is not worth repeating.
set(EXTRAS_RENDER_TEST_SOURCES
    TestCombatFX.cpp
    TestExplosion.cpp
    TestLightning.cpp
)

foreach(TEST_SRC IN LISTS EXTRAS_RENDER_TEST_SOURCES)
    get_filename_component(TARGET_NAME ${TEST_SRC} NAME_WLE)

    add_executable(${TARGET_NAME} ${TEST_SRC})

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_engine
        zahlen_render
        zahlen_vfx
    )

    # Public headers only. src/ is banned — see tools/check_tests_public_api.py.
    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/extras
    )

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

    add_test(NAME GPU_EXTRAS_${TARGET_NAME} COMMAND ${TARGET_NAME})
    set_tests_properties(GPU_EXTRAS_${TARGET_NAME} PROPERTIES
        LABELS "GPU;Extras;LocalOnly"
        TIMEOUT 60
    )

    # Each vkCreateInstance dlopen()s the Vulkan ICD, which on the NVIDIA driver
    # pulls in libnvidia-tls.so. glibc's static TLS surplus is finite and is not
    # fully reclaimed on dlclose, so a binary that builds many engines eventually
    # fails with "cannot allocate memory in static TLS block" and every remaining
    # test reports "vkCreateInstance: Found no drivers!". The suites pool their
    # engines (see tests/helpers/HeadlessEngineFixture.hpp); this raises the
    # surplus as well, so a suite that still churns engines degrades late instead
    # of falling over.
    set(GPU_TEST_ENVIRONMENT "")
    if(UNIX AND NOT APPLE)
        list(APPEND GPU_TEST_ENVIRONMENT "GLIBC_TUNABLES=glibc.rtld.optional_static_tls=4096")
    endif()
    # Include the shared sanitizer and Vulkan sandbox settings regardless of
    # whether sanitizers are enabled.
    list(APPEND GPU_TEST_ENVIRONMENT ${ZHLN_TEST_ENVIRONMENT})
    if(GPU_TEST_ENVIRONMENT)
        set_tests_properties(GPU_EXTRAS_${TARGET_NAME} PROPERTIES
            ENVIRONMENT "${GPU_TEST_ENVIRONMENT}"
        )
    endif()
endforeach()


