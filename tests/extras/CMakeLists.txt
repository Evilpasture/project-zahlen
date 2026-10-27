# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT ZHLN_BUILD_EXTRAS OR NOT TARGET zahlen_extras)
    return()
endif()

# Register a standalone extras test executable with ztest.
function(zahlen_register_extras_test TARGET_NAME)
    set(EXTRAS_TEST_ENVIRONMENT ${ZHLN_TEST_ENVIRONMENT})
    list(APPEND EXTRAS_TEST_ENVIRONMENT ${ZHLN_VULKAN_SANDBOX_ENVIRONMENT})

    zahlen_register_ztest_target(${TARGET_NAME}
        POOL cpu
        TIMEOUT 120
        LABELS "CPU;Fast;Extras"
        ENV ${EXTRAS_TEST_ENVIRONMENT}
        ${ARGN}
    )

    zahlen_add_runtime_dlls(${TARGET_NAME})
endfunction()

# Shared setup for a grouped non-module extras binary (Extras_*).
function(zahlen_add_extras_group GROUP_TARGET)
    zahlen_wrap_ztest_sources(_wrapped_sources _rel_files ${ARGN})
    add_executable(${GROUP_TARGET} ${_wrapped_sources})

    set_target_properties(${GROUP_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        ZHLN_ZTEST_SOURCE_FILES "${_rel_files}"
    )

    target_include_directories(${GROUP_TARGET} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${ZHLN_OPTIONAL_INCLUDE_ROOTS}
    )

    zahlen_enable_reflection(${GROUP_TARGET})

    if(USE_SANITIZERS)
        target_compile_options(${GROUP_TARGET} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${GROUP_TARGET} PRIVATE
            -fsanitize=address,undefined
        )
        target_compile_definitions(${GROUP_TARGET} PRIVATE __ASAN_ENABLED__)
    endif()

    zahlen_add_runtime_dlls(${GROUP_TARGET})
endfunction()

# Preserve historical suite build target names (`cmake --build build --target TestCDN`).
function(zahlen_add_extras_build_alias GROUP_TARGET ALIAS_NAME)
    if(NOT TARGET ${ALIAS_NAME})
        add_custom_target(${ALIAS_NAME} DEPENDS ${GROUP_TARGET})
    endif()
endfunction()

# --- Standalone C++20 Module Suites (isolated BMI scan scope) ---

set(TARGET_NAME TestProceduralAnimation)
zahlen_wrap_ztest_sources(_proc_anim_sources _proc_anim_files TestProceduralAnimation.cpp)
add_executable(${TARGET_NAME}
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    ${_proc_anim_sources}
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON
    ZHLN_ZTEST_SOURCE_FILES "${_proc_anim_files}"
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_animation
    zahlen_gltf
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/extern/cgltf
)

target_compile_definitions(${TARGET_NAME} PRIVATE
    ZHLN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

zahlen_enable_reflection(${TARGET_NAME})

if(USE_SANITIZERS)
    target_compile_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)
endif()

zahlen_register_extras_test(${TARGET_NAME})

set(TARGET_NAME TestNetworkWire)
zahlen_wrap_ztest_sources(_net_wire_sources _net_wire_files TestNetworkWire.cpp)
add_executable(${TARGET_NAME}
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    ${_net_wire_sources}
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON
    ZHLN_ZTEST_SOURCE_FILES "${_net_wire_files}"
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_network
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${ZHLN_OPTIONAL_INCLUDE_ROOTS}
)

zahlen_enable_reflection(${TARGET_NAME})

if(USE_SANITIZERS)
    target_compile_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)
endif()

zahlen_register_extras_test(${TARGET_NAME})

# --- Grouped Non-Module Extras Binaries (5 links instead of 19) ---

# 1. Extras_UI: UI schema, widgets, console debugger, and baked font loader.
zahlen_add_extras_group(Extras_UI
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    TestUITree.cpp
    TestWidgets.cpp
    TestConsoleDebugger.cpp
    TestBakedFontLoader.cpp
)
target_link_libraries(Extras_UI PRIVATE
    zahlen_engine
    zahlen_ui_schema
    zahlen_widgets
    zahlen_console
    zahlen_fonts
)
zahlen_register_extras_test(Extras_UI)
zahlen_add_extras_build_alias(Extras_UI TestUITree)
zahlen_add_extras_build_alias(Extras_UI TestWidgets)
zahlen_add_extras_build_alias(Extras_UI TestConsoleDebugger)
zahlen_add_extras_build_alias(Extras_UI TestBakedFontLoader)

# 2. Extras_Simulation: TwoBoneIK (skeletal domain, module-free), terrain, ALife, DataModel.
zahlen_add_extras_group(Extras_Simulation
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    TestTwoBoneIK.cpp
    TestTerrainLifecycle.cpp
    TestALife.cpp
    TestDataModel.cpp
)
target_link_libraries(Extras_Simulation PRIVATE
    zahlen_engine
    zahlen_skeletal_animation
    zahlen_terrain
    zahlen_alife
    zahlen_datamodel
)
zahlen_register_extras_test(Extras_Simulation)
zahlen_add_extras_build_alias(Extras_Simulation TestTwoBoneIK)
zahlen_add_extras_build_alias(Extras_Simulation TestTerrainLifecycle)
zahlen_add_extras_build_alias(Extras_Simulation TestALife)
zahlen_add_extras_build_alias(Extras_Simulation TestDataModel)

# 3. Extras_Serialization: JSON, TOML, ReflectLua, and zcook GLB container inspection.
zahlen_add_extras_group(Extras_Serialization
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    TestJSON.cpp
    TestTOML.cpp
    TestReflectLua.cpp
    TestCookerGLB.cpp
)
target_link_libraries(Extras_Serialization PRIVATE
    zahlen_engine
    zahlen_serialization
    zahlen_lua
    zahlen_ragdoll_authoring
)
add_dependencies(Extras_Serialization zcook)
zahlen_register_extras_test(Extras_Serialization
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.."
)
zahlen_add_extras_build_alias(Extras_Serialization TestJSON)
zahlen_add_extras_build_alias(Extras_Serialization TestTOML)
zahlen_add_extras_build_alias(Extras_Serialization TestReflectLua)
zahlen_add_extras_build_alias(Extras_Serialization TestCookerGLB)

# 4. Extras_HTTP: loopback HTTP, RemoteAsset cache/fetcher, and CDN layer.
if(TARGET zahlen_http)
    set(ZHLN_EXTRAS_HTTP_SOURCES
        RunExtrasHTTPTests.cpp
        TestHTTP.cpp
    )
    set(ZHLN_EXTRAS_HTTP_LIBS
        zahlen_engine
        zahlen_http
    )
    if(TARGET zahlen_remote_asset)
        list(APPEND ZHLN_EXTRAS_HTTP_SOURCES TestRemoteAsset.cpp)
        list(APPEND ZHLN_EXTRAS_HTTP_LIBS zahlen_remote_asset)
    endif()
    if(TARGET zahlen_cdn)
        list(APPEND ZHLN_EXTRAS_HTTP_SOURCES TestCDN.cpp)
        list(APPEND ZHLN_EXTRAS_HTTP_LIBS zahlen_cdn)
    endif()

    zahlen_add_extras_group(Extras_HTTP ${ZHLN_EXTRAS_HTTP_SOURCES})
    target_link_libraries(Extras_HTTP PRIVATE ${ZHLN_EXTRAS_HTTP_LIBS})
    if(TARGET zahlen_remote_asset)
        target_compile_definitions(Extras_HTTP PRIVATE ZHLN_HAS_REMOTE_ASSET_SUITE=1)
    endif()
    if(TARGET zahlen_cdn)
        target_compile_definitions(Extras_HTTP PRIVATE ZHLN_HAS_CDN_SUITE=1)
    endif()

    zahlen_register_extras_test(Extras_HTTP)
    zahlen_add_extras_build_alias(Extras_HTTP TestHTTP)
    if(TARGET zahlen_remote_asset)
        zahlen_add_extras_build_alias(Extras_HTTP TestRemoteAsset)
    endif()
    if(TARGET zahlen_cdn)
        zahlen_add_extras_build_alias(Extras_HTTP TestCDN)
    endif()
endif()

# 5. Extras_BSP: BSP interactive entities, VMT/VTF materials, static props, StudioModel, VRAD/lighting.
zahlen_add_extras_group(Extras_BSP
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    TestBSPInteraction.cpp
    TestBSPMaterials.cpp
    TestBSPStaticProps.cpp
    TestBSPStudioModel.cpp
    TestBSPLighting.cpp
)
target_link_libraries(Extras_BSP PRIVATE
    zahlen_engine
    zahlen_bsp
    zahlen_interaction
    zahlen_character_controller
)
zahlen_register_extras_test(Extras_BSP)
zahlen_add_extras_build_alias(Extras_BSP TestBSPInteraction)
zahlen_add_extras_build_alias(Extras_BSP TestBSPMaterials)
zahlen_add_extras_build_alias(Extras_BSP TestBSPStaticProps)
zahlen_add_extras_build_alias(Extras_BSP TestBSPStudioModel)
zahlen_add_extras_build_alias(Extras_BSP TestBSPLighting)

# --- Remaining Domain-Specific Suites ---

if(TARGET zahlen_project_light_lua)
    set(TARGET_NAME TestLuaDataModel)
    zahlen_wrap_ztest_sources(_lua_dm_sources _lua_dm_files TestLuaDataModel.cpp)
    add_executable(${TARGET_NAME}
        ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
        ${_lua_dm_sources}
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        ZHLN_ZTEST_SOURCE_FILES "${_lua_dm_files}"
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_project_light_lua
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${ZHLN_OPTIONAL_INCLUDE_ROOTS}
    )

    zahlen_enable_reflection(${TARGET_NAME})

    if(USE_SANITIZERS)
        target_compile_options(${TARGET_NAME} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${TARGET_NAME} PRIVATE
            -fsanitize=address,undefined
        )
        target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)
    endif()

    zahlen_register_extras_test(${TARGET_NAME})
endif()

set(TARGET_NAME TestPerformance)
zahlen_wrap_ztest_sources(_perf_sources _perf_files profile/TestPerformance.cpp)
add_executable(${TARGET_NAME}
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    ${_perf_sources}
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    ZHLN_ZTEST_SOURCE_FILES "${_perf_files}"
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_serialization
    zahlen_character_controller
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${ZHLN_OPTIONAL_INCLUDE_ROOTS}
)

zahlen_enable_reflection(${TARGET_NAME})

if(USE_SANITIZERS)
    target_compile_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)
endif()

zahlen_register_extras_test(${TARGET_NAME})

set(TARGET_NAME TestScripting)
zahlen_wrap_ztest_sources(_scripting_sources _scripting_files
    scripting/TestScriptBinder.cpp
    scripting/TestScriptECSBridge.cpp
    scripting/TestScriptValueTypes.cpp
)
add_executable(${TARGET_NAME}
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    ${_scripting_sources}
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    ZHLN_ZTEST_SOURCE_FILES "${_scripting_files}"
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_scripting
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${ZHLN_OPTIONAL_INCLUDE_ROOTS}
)

zahlen_enable_reflection(${TARGET_NAME})

if(USE_SANITIZERS)
    target_compile_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)
endif()

zahlen_register_extras_test(${TARGET_NAME})

if(TARGET zahlen_svg)
    set(TARGET_NAME TestSVG)
    zahlen_wrap_ztest_sources(_svg_sources _svg_files TestSVG.cpp)
    add_executable(${TARGET_NAME}
        ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
        ${_svg_sources}
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        ZHLN_ZTEST_SOURCE_FILES "${_svg_files}"
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_engine
        zahlen_svg
    )

    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${ZHLN_OPTIONAL_INCLUDE_ROOTS}
    )

    zahlen_enable_reflection(${TARGET_NAME})

    if(USE_SANITIZERS)
        target_compile_options(${TARGET_NAME} PRIVATE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(${TARGET_NAME} PRIVATE
            -fsanitize=address,undefined
        )
        target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)
    endif()

    zahlen_register_extras_test(${TARGET_NAME})
endif()

include(${CMAKE_CURRENT_LIST_DIR}/ProjectLightClientTests.cmake)
