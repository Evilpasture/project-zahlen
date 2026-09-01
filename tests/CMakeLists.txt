# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Tests exercise public behaviour only. They must not include engine internals
# from src/ (systems, render/Vulkan guts, cooker headers, ...). Configure-time
# enforcement lives in tools/check_tests_public_api.py.
#
# Layout: one binary per directory group instead of one per suite. Every
# Test*.cpp in a group keeps its suite definition private to its own
# translation unit and exports a stats-returning function; the group's
# Run*Tests.cpp aggregates them through ZHLN::Test::Runner::RunDeferred.
# Grouping 24 suites into 8 links is what pays for itself under ASan/UBSan or
# LTO, where each extra link of zahlen_engine + Jolt + reflection meta-types is
# the expensive part. The cost is process isolation: a crash or timeout in one
# suite takes the rest of its group with it.

# The test tree is built on top of the extras layer even though the core engine
# is not: the perf baselines and the cooker's GLB checks read and write JSON
# through extras/json, and the JSON and TOML suites live in tests/extras.
# Turning extras off therefore turns the tests off with them, rather than
# leaving a test tree that cannot compile.
if(NOT TARGET zahlen_extras)
    message(FATAL_ERROR
        "ZHLN_BUILD_TESTS=ON requires ZHLN_BUILD_EXTRAS=ON: the test suites read JSON through extras/json.")
endif()

# 1. Enable CTest testing framework
enable_testing()

# Shared per-target setup for a CPU test group.
function(zahlen_add_cpu_test_group TARGET_NAME GROUP_DIR)
    # Picks up both the Test*.cpp suites and the group's Run*Tests.cpp entry
    # point. A file that defines main() must not be added to any other group.
    file(GLOB GROUP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${GROUP_DIR}/*.cpp")

    if(NOT GROUP_SOURCES)
        message(FATAL_ERROR "CPU test group ${TARGET_NAME}: no sources found in tests/${GROUP_DIR}")
    endif()

    add_executable(${TARGET_NAME} ${GROUP_SOURCES})

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

    # Applies the required reflection flags (-freflection / custom libc++) configured in the root
    zahlen_enable_reflection(${TARGET_NAME})

    # Extra per-group dependencies.
    #
    # assets: include/Zahlen/Meshlet.hpp is a public header, but it includes
    # <meshoptimizer.h> and its inline BuildMeshlets() calls into the library.
    # zahlen_engine links meshoptimizer PRIVATE (root CMakeLists.txt:502), so
    # neither the include path nor the symbols propagate through the engine
    # link. TestMeshlets exercises BuildMeshlets, so the group links it.
    if(TARGET_NAME STREQUAL "CPU_Assets")
        target_link_libraries(${TARGET_NAME} PRIVATE meshoptimizer)
    endif()

    # perf/assets: JSON is an extra, so the two groups that read it link the
    # extras archive -- TestCooker.cpp parses the cooker's GLB JSON chunk and
    # PerfBaseline.hpp reads and writes the perf baseline files. Only the
    # parser's symbols are needed (the <json/...> include path arrives with
    # zahlen_extras' PUBLIC include directories), and the linker drops the
    # VFX/animation objects nothing here references. The other groups link the
    # core engine only, which is the whole point of the boundary.
    if(TARGET_NAME STREQUAL "CPU_Assets" OR TARGET_NAME STREQUAL "CPU_Perf")
        target_link_libraries(${TARGET_NAME} PRIVATE zahlen_extras)
    endif()

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

    # Register the group as a single CTest entry. Per-suite results still print
    # individually; use `ctest -R CPU_Core` to run one group.
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    set_tests_properties(${TARGET_NAME} PROPERTIES LABELS "CPU;Fast")

    # Pass Sanitizer options to CTest runner so reports halt and print stack traces
    if(USE_SANITIZERS)
        set_tests_properties(${TARGET_NAME} PROPERTIES
            ENVIRONMENT "${ZHLN_TEST_ENVIRONMENT}"
        )
    endif()
endfunction()

# 2. The CPU test groups.
zahlen_add_cpu_test_group(CPU_Core      core)
zahlen_add_cpu_test_group(CPU_Threading threading)
zahlen_add_cpu_test_group(CPU_ECS       ecs)
zahlen_add_cpu_test_group(CPU_Scripting scripting)
zahlen_add_cpu_test_group(CPU_Physics   physics)
zahlen_add_cpu_test_group(CPU_Assets    assets)
zahlen_add_cpu_test_group(CPU_Audio     audio)
zahlen_add_cpu_test_group(CPU_Perf      perf)

# Optional CPU tests live with the extras toolkit and disappear completely when
# ZHLN_BUILD_EXTRAS is disabled.
if(ZHLN_BUILD_EXTRAS AND TARGET zahlen_extras)
    add_subdirectory(extras)
endif()

# 3. Local GPU Render Test Suite (Off by default for headless CI/CD)
option(ZHLN_BUILD_GPU_TESTS "Build GPU/Vulkan rendering tests (local hardware required)" OFF)

if(ZHLN_BUILD_GPU_TESTS)
    message(STATUS "Configuring local GPU render tests in tests/render...")
    add_subdirectory(render)
endif()
