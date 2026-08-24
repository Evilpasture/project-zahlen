# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT ZHLN_BUILD_EXTRAS OR NOT TARGET zahlen_extras)
    return()
endif()

file(GLOB EXTRAS_RENDER_TEST_SOURCES CONFIGURE_DEPENDS "Test*.cpp")

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
        zahlen_extras
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

    if(USE_SANITIZERS)
        set_tests_properties(GPU_EXTRAS_${TARGET_NAME} PROPERTIES
            ENVIRONMENT "${ZHLN_TEST_ENVIRONMENT}"
        )
    endif()
endforeach()


