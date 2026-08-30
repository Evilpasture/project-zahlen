# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# GPU test groups: one binary per domain instead of one per suite. Each
# Test*.cpp keeps its suite definition private to its own translation unit and
# exports a stats-returning function; the group's Run*Tests.cpp aggregates them
# through ZHLN::Test::Runner::RunDeferred.
#
# What grouping costs here specifically: these suites create and destroy a
# Vulkan device, and a group now shares one process and one CTest TIMEOUT. A
# device-lost that the engine cannot recover from, or a hang, takes every
# remaining suite in the group with it. The grouping below keeps the two
# long-running suites (render performance, distance stability) in their own
# group so a timeout there does not also lose the pipeline results.
#
# Every group compiles tests/helpers/ImageWriteImpl.cpp exactly once -- it owns
# the stb_image_write implementation the shared PNG capture path links against.
# Adding a second TU that defines ZHLN_TEST_IMAGE_WRITE_IMPL breaks the link.

set(GPU_HELPER_IMPL ${PROJECT_SOURCE_DIR}/tests/helpers/ImageWriteImpl.cpp)

function(zahlen_add_gpu_test_group TARGET_NAME TIMEOUT_SECONDS)
    set(GROUP_SOURCES ${ARGN})

    add_executable(${TARGET_NAME} ${GROUP_SOURCES} ${GPU_HELPER_IMPL})

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_engine
        zahlen_render
        imgui
        # TestMeshShaders.cpp exercises Zahlen/Meshlet.hpp (the shared cooker
        # partitioning) directly; zahlen_engine links meshoptimizer privately,
        # so the tests need their own INTERFACE include/link.
        meshoptimizer
    )

    # Public headers only. src/ is banned — see tools/check_tests_public_api.py.
    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/extern/stb
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

    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    set_tests_properties(${TARGET_NAME} PROPERTIES
        LABELS "GPU;LocalOnly"
        TIMEOUT ${TIMEOUT_SECONDS}
    )

    if(USE_SANITIZERS)
        set_tests_properties(${TARGET_NAME} PROPERTIES
            ENVIRONMENT "${ZHLN_TEST_ENVIRONMENT}"
        )
    endif()
endfunction()

# Lighting, shading and ray-traced image quality.
zahlen_add_gpu_test_group(GPU_Lighting 300
    RunLightingTests.cpp
    TestClusteredLighting.cpp
    TestRayTracedShadows.cpp
    TestReflections.cpp
    TestPBR.cpp
    TestRTRPBRReflection.cpp
    TestRayTracedNoiseStability.cpp
    TestRayTracedReflectionNoise.cpp
    TestDecal.cpp
)

# Pipeline, resource and culling mechanics.
zahlen_add_gpu_test_group(GPU_Pipeline 300
    RunPipelineTests.cpp
    TestRenderPipelines.cpp
    TestRenderCompute.cpp
    TestMeshShaders.cpp
    TestDescriptorHeaps.cpp
    TestDescriptorHeapsParallel.cpp
    TestRenderAnimatedMesh.cpp
    TestCameraLookAt.cpp
    TestRenderHIZ.cpp
)

# ImGui, UI layout and viewmodel presentation.
zahlen_add_gpu_test_group(GPU_Interface 180
    RunInterfaceTests.cpp
    TestImGuiRender.cpp
    TestUILayoutRender.cpp
    TestViewmodel.cpp
)

# Throughput and long-run distance stability. Kept apart from the others: these
# two are the slowest suites and the most likely to hit the timeout.
zahlen_add_gpu_test_group(GPU_Performance 900
    RunPerformanceTests.cpp
    TestRenderPerformance.cpp
    TestRenderDistanceStability.cpp
)

# --- Extras VFX & Render Tests ---
if(ZHLN_BUILD_EXTRAS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extras")
    add_subdirectory(extras)
endif()
