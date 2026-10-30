# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Tests exercise public behaviour only. They must not include engine internals
# from src/ (systems, render/Vulkan guts, cooker headers, ...). Configure-time
# enforcement lives in configure/check_tests_public_api.py.
#
# Layout: explicit file list (no file(GLOB), preserving exact Ninja incremental
# rebuilds) compiled into an internal CPU worker binary (`ztest_cpu`). Each TU
# is wrapped via `zahlen_wrap_ztest_sources(...)` so `test_*` functions inside
# `namespace ZHLN::Test` and primary-TU suite structs self-register with zero
# boilerplate, and `ztest` collects and shards execution per source file across
# `-j N` worker processes.

# Public include roots for the optional outer layers. Kept as a list because
# plugin formats, engine extensions, network I/O, and gameplay each have their
# own include root after the source-tree split.
set(ZHLN_OPTIONAL_INCLUDE_ROOTS
    ${PROJECT_SOURCE_DIR}/plugins
    ${PROJECT_SOURCE_DIR}/extensions
    ${PROJECT_SOURCE_DIR}/extensions/net
    ${PROJECT_SOURCE_DIR}/gameplay
    ${PROJECT_SOURCE_DIR}/gameplay/ProjectLight
)

set(ZHLN_CPU_TEST_SOURCES
    asset_cooking/TestRadiance.cpp
    assets/TestCooker.cpp
    assets/TestEnvironmentImage.cpp
    assets/TestMeshlets.cpp
    assets/TestPackaging.cpp
    audio/TestsAudio.cpp
    core/TestClock.cpp
    core/TestCommandLine.cpp
    core/TestContainers.cpp
    core/TestDefer.cpp
    core/TestEnumFlags.cpp
    core/TestError.cpp
    core/TestFileSystem.cpp
    core/TestFileSystemWatcher.cpp
    core/TestFontAsset.cpp
    core/TestGUIContext.cpp
    core/TestGraphicsSettings.cpp
    core/TestInput.cpp
    core/TestMath.cpp
    core/TestOptional.cpp
    core/TestPlatform.cpp
    core/TestRayTracedNoiseMetrics.cpp
    core/TestReflection.cpp
    core/TestSignal.cpp
    core/TestSoA.cpp
    core/TestTestFramework.cpp
    core/TestTextEdit.cpp
    core/TestWaitIdle.cpp
    ecs/TestECS.cpp
    ecs/TestEventBus.cpp
    ecs/TestSystemGraph.cpp
    physics/TestPhysics.cpp
    threading/TestChannel.cpp
    threading/TestSyncPrim.cpp
    threading/TestTaskSystem.cpp
)

if(TARGET zahlen_character_controller)
    list(APPEND ZHLN_CPU_TEST_SOURCES physics/TestCharacterMovement.cpp)
endif()

zahlen_wrap_ztest_sources(ZHLN_CPU_WRAPPED_SOURCES ZHLN_CPU_REL_FILES
    ${ZHLN_CPU_TEST_SOURCES}
)

add_executable(ztest_cpu
    ${CMAKE_CURRENT_SOURCE_DIR}/TestsMain.cpp
    ${ZHLN_CPU_WRAPPED_SOURCES}
)

set_target_properties(ztest_cpu PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    ZHLN_ZTEST_SOURCE_FILES "${ZHLN_CPU_REL_FILES}"
)

target_link_libraries(ztest_cpu PRIVATE
    zahlen_engine
    zahlen_command_line
    zahlen_signal
    zahlen_asset_cooking
)

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
target_include_directories(ztest_cpu PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/plugins
    ${PROJECT_SOURCE_DIR}/extensions
    ${PROJECT_SOURCE_DIR}/extern/stb
)

if(TARGET zahlen_character_controller)
    target_link_libraries(ztest_cpu PRIVATE zahlen_character_controller)
    target_include_directories(ztest_cpu PRIVATE ${PROJECT_SOURCE_DIR}/extensions)
    target_compile_definitions(ztest_cpu PRIVATE ZHLN_HAS_CHARACTER_CONTROLLER=1)
endif()

zahlen_enable_reflection(ztest_cpu)
add_dependencies(ztest_cpu zcook)

if(USE_SANITIZERS)
    target_compile_options(ztest_cpu PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize=vptr
        -fno-omit-frame-pointer
    )
    target_link_options(ztest_cpu PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize=vptr
    )
    target_compile_definitions(ztest_cpu PRIVATE __ASAN_ENABLED__)

    if(WIN32)
        get_filename_component(CLANG_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
        find_file(ASAN_DLL
            NAMES "clang_rt.asan_dynamic-x86_64.dll"
            PATHS "${CLANG_BIN_DIR}/../lib/clang/23/lib/windows" "${CLANG_BIN_DIR}/../lib/clang/22/lib/windows"
        )
        if(ASAN_DLL)
            add_custom_command(TARGET ztest_cpu POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ASAN_DLL}" "$<TARGET_FILE_DIR:ztest_cpu>"
                COMMENT "Copying ASan Runtime DLL for ztest_cpu..."
            )
        endif()
    endif()
endif()

set(CPU_TEST_ENVIRONMENT ${ZHLN_TEST_ENVIRONMENT})
list(APPEND CPU_TEST_ENVIRONMENT ${ZHLN_VULKAN_SANDBOX_ENVIRONMENT})

zahlen_register_ztest_target(ztest_cpu
    POOL cpu
    TIMEOUT 120
    LABELS "CPU;Fast"
    FILES ${ZHLN_CPU_REL_FILES}
    ENV ${CPU_TEST_ENVIRONMENT}
)

zahlen_add_runtime_dlls(ztest_cpu)

if(ZHLN_BUILD_EXTRAS AND TARGET zahlen_extras)
    add_subdirectory(extras)
endif()

option(ZHLN_BUILD_GPU_TESTS "Build GPU/Vulkan rendering tests (local hardware required)" OFF)

if(ZHLN_BUILD_GPU_TESTS)
    if(NOT ZHLN_BUILD_RENDER)
        message(STATUS "Skipping tests/render: ZHLN_BUILD_RENDER=OFF")
    else()
        message(STATUS "Configuring local GPU render tests in tests/render...")
        add_subdirectory(render)
    endif()
endif()
