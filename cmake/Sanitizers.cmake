# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/Sanitizers.cmake
#
# ASan + UBSan for the whole build, the test environment they need, and the
# Windows runtime DLL copy. Must be included before any add_subdirectory: the
# compile/link options only reach targets that are created afterwards.

option(USE_SANITIZERS "Enable ASan and UBSan" OFF)

# Shared CTest environment. Keep this list independent of the sanitizer option:
# the Vulkan sandbox must reach every test in a normal build as well.
set(ZHLN_TEST_ENVIRONMENT "")

if(USE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    # Marks the whole sanitizer profile. Tests whose consteval code builds
    # std::strings are broken by GCC bug 71962 (-fsanitize=undefined rejects
    # pointer null-checks during constant evaluation); see
    # tests/core/TestReflection.cpp and tests/extras/TestJSON.cpp.
    #
    # Engine-side readers avoid the pattern instead of being guarded away from
    # it: the GPU ABI reader answers "no such type" with an index rather than a
    # null pointer, because the inventory walk is exactly what CI has to run
    # (src/vulkan/pipeline/SpirvLayout.hpp, TypeIndexAt). A new constexpr reader
    # that null-checks a pointer into a static object will break this build and
    # not the others.
    add_definitions(
        -D__ASAN_ENABLED__
        -DZHLN_SANITIZER_BUILD=1
        -DZHLN_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
    )

    list(APPEND ZHLN_TEST_ENVIRONMENT
        "ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=1:symbolize=1:halt_on_error=1"
        "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/lsan.supp:print_suppressions=0"
        "UBSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/ubsan.supp:print_stacktrace=1:halt_on_error=1"
        "TSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tsan.supp:halt_on_error=1"
    )
endif()

# The Clang ASan dynamic runtime is a DLL next to the executable on Windows.
# The copy is a POST_BUILD step on a real target, so it is applied from the
# root CMakeLists after the target exists; this module only owns the recipe.
function(zahlen_install_sanitizer_runtime TARGET_NAME)
    get_filename_component(CLANG_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    find_file(ASAN_DLL
        NAMES "clang_rt.asan_dynamic-x86_64.dll"
        PATHS "${CLANG_BIN_DIR}/../lib/clang/23/lib/windows" "${CLANG_BIN_DIR}/../lib/clang/22/lib/windows"
    )
    if(ASAN_DLL)
        add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy "${ASAN_DLL}" "$<TARGET_FILE_DIR:${TARGET_NAME}>"
            COMMENT "Copying ASan Runtime for ${TARGET_NAME}..."
        )
    endif()
endfunction()
