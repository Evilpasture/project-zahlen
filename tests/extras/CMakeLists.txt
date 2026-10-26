# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT ZHLN_BUILD_EXTRAS OR NOT TARGET zahlen_extras)
    return()
endif()

# Register a standalone extras test executable with CTest.
function(zahlen_register_extras_test TARGET_NAME)
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    set_tests_properties(${TARGET_NAME} PROPERTIES
        LABELS "CPU;Fast;Extras"
        ${ARGN}
    )
    set(EXTRAS_TEST_ENVIRONMENT ${ZHLN_TEST_ENVIRONMENT})
    list(APPEND EXTRAS_TEST_ENVIRONMENT ${ZHLN_VULKAN_SANDBOX_ENVIRONMENT})
    if(EXTRAS_TEST_ENVIRONMENT)
        set_tests_properties(${TARGET_NAME} PROPERTIES
            ENVIRONMENT "${EXTRAS_TEST_ENVIRONMENT}"
        )
    endif()

    zahlen_add_runtime_dll_path(${TARGET_NAME})
    zahlen_add_runtime_dlls(${TARGET_NAME})
endfunction()

# Shared setup for a grouped non-module extras binary (Extras_*).
function(zahlen_add_extras_group GROUP_TARGET)
    add_executable(${GROUP_TARGET} ${ARGN})

    set_target_properties(${GROUP_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
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

# Register one suite inside a grouped Extras_* binary under its historical
# target/CTest name: `ninja <ALIAS_NAME>` builds the group binary, and
# `ctest -R <ALIAS_NAME>` runs just `<SUITE_FILTER>` in an isolated process.
function(zahlen_register_extras_group_member GROUP_TARGET ALIAS_NAME SUITE_FILTER)
    if(NOT TARGET ${ALIAS_NAME})
        add_custom_target(${ALIAS_NAME} DEPENDS ${GROUP_TARGET})
    endif()

    add_test(NAME ${ALIAS_NAME} COMMAND ${GROUP_TARGET})
    set_tests_properties(${ALIAS_NAME} PROPERTIES
        LABELS "CPU;Fast;Extras;${GROUP_TARGET}"
        ${ARGN}
    )

    set(EXTRAS_TEST_ENVIRONMENT ${ZHLN_TEST_ENVIRONMENT})
    list(APPEND EXTRAS_TEST_ENVIRONMENT ${ZHLN_VULKAN_SANDBOX_ENVIRONMENT})
    list(APPEND EXTRAS_TEST_ENVIRONMENT "ZHLN_TEST_FILTER=${SUITE_FILTER}")
    set_tests_properties(${ALIAS_NAME} PROPERTIES
        ENVIRONMENT "${EXTRAS_TEST_ENVIRONMENT}"
    )

    zahlen_add_runtime_dll_path(${ALIAS_NAME})
endfunction()

# --- Standalone C++20 Module Suites (isolated BMI scan scope) ---

set(TARGET_NAME TestProceduralAnimation)
add_executable(${TARGET_NAME}
    TestProceduralAnimation.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON
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
add_executable(${TARGET_NAME} TestNetworkWire.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON
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
    RunExtrasUITests.cpp
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
zahlen_register_extras_group_member(Extras_UI TestUITree "UITreeTestSuite::*")
zahlen_register_extras_group_member(Extras_UI TestWidgets "WidgetTestSuite::*")
zahlen_register_extras_group_member(Extras_UI TestConsoleDebugger "ConsoleDebuggerTestSuite::*")
zahlen_register_extras_group_member(Extras_UI TestBakedFontLoader "BakedFontLoaderTestSuite::*")

# 2. Extras_Simulation: TwoBoneIK (skeletal domain, module-free), terrain, ALife, DataModel.
zahlen_add_extras_group(Extras_Simulation
    RunExtrasSimulationTests.cpp
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
zahlen_register_extras_group_member(Extras_Simulation TestTwoBoneIK "TwoBoneIKTestSuite::*")
zahlen_register_extras_group_member(Extras_Simulation TestTerrainLifecycle "TerrainLifecycleTestSuite::*")
zahlen_register_extras_group_member(Extras_Simulation TestALife "ALifeTestSuite::*")
zahlen_register_extras_group_member(Extras_Simulation TestDataModel "TestDataModelSuite::*")

# 3. Extras_Serialization: JSON, TOML, ReflectLua, and zcook GLB container inspection.
zahlen_add_extras_group(Extras_Serialization
    RunExtrasSerializationTests.cpp
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
zahlen_register_extras_group_member(Extras_Serialization TestJSON "JSONTestSuite::*")
zahlen_register_extras_group_member(Extras_Serialization TestTOML "TOMLTestSuite::*")
zahlen_register_extras_group_member(Extras_Serialization TestReflectLua "ReflectLuaSuite::*")
zahlen_register_extras_group_member(Extras_Serialization TestCookerGLB "CookerGlbTestSuite::*"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.."
)

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

    zahlen_register_extras_group_member(Extras_HTTP TestHTTP "HTTPTestSuite::*")
    if(TARGET zahlen_remote_asset)
        zahlen_register_extras_group_member(Extras_HTTP TestRemoteAsset "RemoteAssetTestSuite::*")
    endif()
    if(TARGET zahlen_cdn)
        zahlen_register_extras_group_member(Extras_HTTP TestCDN "CDNTestSuite::*")
    endif()
endif()

# 5. Extras_BSP: BSP interactive entities, VMT/VTF materials, static props, StudioModel, VRAD/lighting.
zahlen_add_extras_group(Extras_BSP
    RunExtrasBSPTests.cpp
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
zahlen_register_extras_group_member(Extras_BSP TestBSPInteraction "BSPInteractionTestSuite::*")
zahlen_register_extras_group_member(Extras_BSP TestBSPMaterials "BSPMaterialsTestSuite::*")
zahlen_register_extras_group_member(Extras_BSP TestBSPStaticProps "BSPStaticPropsTestSuite::*")
zahlen_register_extras_group_member(Extras_BSP TestBSPStudioModel "BSPStudioModelTestSuite::*")
zahlen_register_extras_group_member(Extras_BSP TestBSPLighting "BSPLightingTestSuite::*")

# --- Remaining Domain-Specific Suites ---

if(TARGET zahlen_project_light_lua)
    set(TARGET_NAME TestLuaDataModel)
    add_executable(${TARGET_NAME} TestLuaDataModel.cpp)

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
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
add_executable(${TARGET_NAME}
    profile/RunPerfTests.cpp
    profile/TestPerformance.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
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
add_executable(${TARGET_NAME}
    scripting/RunScriptingTests.cpp
    scripting/TestScriptBinder.cpp
    scripting/TestScriptECSBridge.cpp
    scripting/TestScriptValueTypes.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
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
    add_executable(${TARGET_NAME} TestSVG.cpp)

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
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
