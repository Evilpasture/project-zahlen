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
    # Shared sanitizer settings only. The opt-in Vulkan sandbox pin
    # (ZHLN_VULKAN_SANDBOX_ENVIRONMENT) is deliberately NOT inherited: these
    # groups are "local hardware required", and VK_DRIVER_FILES replaces the
    # loader's ICD search path, which would hide the very GPU they exist to
    # exercise and quietly run them on lavapipe instead. To force a render
    # suite onto the sandbox ICD, export VK_DRIVER_FILES yourself when invoking
    # the test binary directly.
    list(APPEND GPU_TEST_ENVIRONMENT ${ZHLN_TEST_ENVIRONMENT})
    if(GPU_TEST_ENVIRONMENT)
        set_tests_properties(${TARGET_NAME} PROPERTIES
            ENVIRONMENT "${GPU_TEST_ENVIRONMENT}"
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
    TestEmissiveShading.cpp
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
    TestGLTFImport.cpp
)

# TestGLTFImport.cpp checks what the shipping importer produced against the
# same GLB parsed independently by cgltf, and TestRenderAnimatedMesh.cpp plays
# one back. Both call ZHLN::GLTF::* in extras/glTF directly -- core's loader is
# only a lookup in the prefab cache the importer fills -- so this group links
# zahlen_extras. That one link also supplies:
#
#   * the cgltf symbols for the reference parse, and its header path below
#     (zahlen_extras compiles the one extern/cgltf_impl.c in the tree)
#   * extras/json, which TestGLTFImport.cpp uses to serialize the JSON chunk of
#     the GLBs it assembles
#
# ZHLN_TEST_SOURCE_DIR locates resources/assets/ at runtime instead of
# embedding a 100 KB rig.
target_link_libraries(GPU_Pipeline PRIVATE zahlen_extras)
target_include_directories(GPU_Pipeline PRIVATE ${PROJECT_SOURCE_DIR}/extras)
target_include_directories(GPU_Pipeline SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/extern/cgltf)
target_compile_definitions(GPU_Pipeline PRIVATE ZHLN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

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

# TestRenderPerformance.cpp checks its measurements against the recorded
# baselines, and those live in extras/profile/PerfBaseline.hpp on top of
# extras/json -- so this group links the extras archive like GPU_Pipeline does.
target_link_libraries(GPU_Performance PRIVATE zahlen_extras)
target_include_directories(GPU_Performance PRIVATE ${PROJECT_SOURCE_DIR}/extras)

# --- Extras VFX & Render Tests ---
if(ZHLN_BUILD_EXTRAS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extras")
    add_subdirectory(extras)
endif()
