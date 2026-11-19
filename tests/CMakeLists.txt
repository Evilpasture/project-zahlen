# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

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
    core/TestCOM.cpp
    core/TestCOMApp.cpp
    core/TestCOMBoundary.cpp
    core/TestCOMPhase2.cpp
    core/TestClock.cpp
    core/TestCommandLine.cpp
    core/TestContainers.cpp
    core/TestDefer.cpp
    core/TestEnumFlags.cpp
    core/TestError.cpp
    core/TestFileSystem.cpp
    core/TestFileSystemWatcher.cpp
    core/TestUIData.cpp
    core/TestGraphicsSettings.cpp
    core/TestInputLive.cpp
    core/TestMangle.cpp
    core/TestMath.cpp
    core/TestNamespaceFixture.cpp
    core/TestOptional.cpp
    core/TestPlatform.cpp
    core/TestRayTracedNoiseMetrics.cpp
    core/TestReflection.cpp
    core/TestSignal.cpp
    core/TestSoA.cpp
    core/TestTestFramework.cpp
    core/TestWaitIdle.cpp
    physics/TestPhysics.cpp
    threading/TestChannel.cpp
    threading/TestSyncPrim.cpp
    threading/TestTaskSystem.cpp
)

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
    zahlen_asset_cooking
    zahlen_image
    zahlen_application
)

target_include_directories(ztest_cpu PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/app/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/plugins
    ${PROJECT_SOURCE_DIR}/extensions
)

add_library(ztest_mangle_fixture SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/core/MangleFixture.cpp
)
set_target_properties(ztest_mangle_fixture PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
target_include_directories(ztest_mangle_fixture PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)
target_link_libraries(ztest_mangle_fixture PRIVATE zahlen_engine)
zahlen_enable_reflection(ztest_mangle_fixture)
if(USE_SANITIZERS)
    target_compile_options(ztest_mangle_fixture PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize=vptr
        -fno-omit-frame-pointer
    )
    target_link_options(ztest_mangle_fixture PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize=vptr
    )
    target_compile_definitions(ztest_mangle_fixture PRIVATE __ASAN_ENABLED__)
endif()

zahlen_enable_reflection(ztest_cpu)
add_dependencies(ztest_cpu zcook ztest_mangle_fixture)

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

if(ZHLN_BUILD_EXTRAS)
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

add_executable(TestImage image/TestImage.cpp)
target_link_libraries(TestImage PRIVATE zahlen_image)
set_target_properties(TestImage PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON)
add_test(NAME ImageCodec COMMAND TestImage)
