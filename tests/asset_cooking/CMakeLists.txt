# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Host-side RGBE/JPEG/ZRD1/ZRD2 decoding is exercised separately from the core-only
# CPU_Assets group. zcook and this suite exist even with ZHLN_BUILD_EXTRAS=OFF.
set(TARGET_NAME TestRadianceCooking)
add_executable(${TARGET_NAME} RunRadianceTests.cpp TestRadiance.cpp)
set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(${TARGET_NAME} PRIVATE zahlen_asset_cooking zahlen_engine)
# Only public core/extras headers; no private src/ includes.
target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/plugins
)
zahlen_enable_reflection(${TARGET_NAME})
if(USE_SANITIZERS)
    target_compile_options(${TARGET_NAME} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${TARGET_NAME} PRIVATE -fsanitize=address,undefined)
    target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)
endif()

# The round-trip launches zcook as a subprocess, including in core-only builds.
add_dependencies(${TARGET_NAME} zcook)
add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
set_tests_properties(${TARGET_NAME} PROPERTIES LABELS "CPU;Fast;Assets;AssetCooking")
set(ASSET_COOKING_TEST_ENVIRONMENT ${ZHLN_TEST_ENVIRONMENT})
list(APPEND ASSET_COOKING_TEST_ENVIRONMENT ${ZHLN_VULKAN_SANDBOX_ENVIRONMENT})
if(ASSET_COOKING_TEST_ENVIRONMENT)
    set_tests_properties(${TARGET_NAME} PROPERTIES ENVIRONMENT "${ASSET_COOKING_TEST_ENVIRONMENT}")
endif()
# The binary, and the zcook it launches, both need the loader path on Windows.
zahlen_add_runtime_dll_path(${TARGET_NAME})
zahlen_add_runtime_dlls(${TARGET_NAME})
