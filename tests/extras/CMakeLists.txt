# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# The option is the whole test. This used to also require `TARGET zahlen_extras`,
# an aggregate nothing linked (see the root CMakeLists note); keeping a target
# check for a target that no longer exists would make this whole directory
# silently unreachable.
if(NOT ZHLN_BUILD_EXTRAS)
    return()
endif()

# Register a standalone extras test executable with ztest.
function(zahlen_register_extras_test TARGET_NAME)
    cmake_parse_arguments(EXTRAS "" "POOL;TIMEOUT" "LABELS" ${ARGN})
    if(NOT EXTRAS_POOL)
        set(EXTRAS_POOL cpu)
    endif()
    if(NOT EXTRAS_TIMEOUT)
        set(EXTRAS_TIMEOUT 120)
    endif()
    if(NOT EXTRAS_LABELS)
        set(EXTRAS_LABELS "CPU;Fast;Extras")
    endif()

    set(EXTRAS_TEST_ENVIRONMENT ${ZHLN_TEST_ENVIRONMENT})
    list(APPEND EXTRAS_TEST_ENVIRONMENT ${ZHLN_VULKAN_SANDBOX_ENVIRONMENT})

    zahlen_register_ztest_target(${TARGET_NAME}
        POOL ${EXTRAS_POOL}
        TIMEOUT ${EXTRAS_TIMEOUT}
        LABELS ${EXTRAS_LABELS}
        ENV ${EXTRAS_TEST_ENVIRONMENT}
        ${EXTRAS_UNPARSED_ARGUMENTS}
    )

    zahlen_add_runtime_dlls(${TARGET_NAME})
endfunction()

# Shared setup for a grouped non-module extras binary (Extras_*).
function(zahlen_add_extras_group GROUP_TARGET)
    zahlen_wrap_ztest_sources(_wrapped_sources _rel_files ${ARGN})
    add_executable(${GROUP_TARGET} ${_wrapped_sources})

    # Binaries belong in ${CMAKE_BINARY_DIR}/bin, the same place ztest_cpu and
    # the host tools go. FindDataFile() probes the executable's parent directory
    # only when the executable itself sits in a directory named "bin" or
    # "samples" (src/filesystem/RuntimePaths.cpp), so an extras binary left in
    # tests/extras/ can never reach the data staged in ${CMAKE_BINARY_DIR} --
    # every FindDataFile("config/...") and ("scripts/...") returns nullopt no
    # matter what the build stages.
    set_target_properties(${GROUP_TARGET} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ZHLN_ZTEST_SOURCE_FILES "${_rel_files}"
    )

    target_include_directories(${GROUP_TARGET} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${ZHLN_OPTIONAL_INCLUDE_ROOTS}
    )

    zahlen_enable_reflection(${GROUP_TARGET})

    # A targeted `--target ${GROUP_TARGET}` build must still stage the data
    # directories the suite reads through FindDataFile(); the ALL rules only
    # cover a full build. Mirrors samples/CMakeLists.txt.
    foreach(_data_target IN ITEMS link_resources link_config link_scripts)
        if(TARGET ${_data_target})
            add_dependencies(${GROUP_TARGET} ${_data_target})
        endif()
    endforeach()

    if(USE_SANITIZERS)
        target_compile_options(${GROUP_TARGET} PRIVATE
            -fsanitize=address,undefined
            -fno-sanitize=vptr
            -fno-omit-frame-pointer
        )
        target_link_options(${GROUP_TARGET} PRIVATE
            -fsanitize=address,undefined
            -fno-sanitize=vptr
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
    zahlen_animation zahlen_locomotion zahlen_procedural_animation
    zahlen_gltf_assets
)

target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${ZHLN_OPTIONAL_INCLUDE_ROOTS}
)

target_compile_definitions(${TARGET_NAME} PRIVATE
    ZHLN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

zahlen_enable_reflection(${TARGET_NAME})

if(USE_SANITIZERS)
    target_compile_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize=vptr
        -fno-omit-frame-pointer
    )
    target_link_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize=vptr
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
        -fno-sanitize=vptr
        -fno-omit-frame-pointer
    )
    target_link_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize=vptr
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)
endif()

zahlen_register_extras_test(${TARGET_NAME})

# The reusable application's CLI early exits are CPU-only: registered
# options, typed help/version/graph outcomes, invalid C arguments and removed
# host-mode flags must not create a GPU device.
zahlen_add_extras_group(Extras_Application
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    TestApplication.cpp
    TestScreenshot.cpp
    TestPlugin.cpp
)
# Deliberately does NOT link zahlen_ui, zahlen_freecam, zahlen_animation,
# zahlen_character_controller, zahlen_terrain, zahlen_vfx, zahlen_gltf_assets,
# zahlen_bsp_assets, zahlen_svg, zahlen_http, zahlen_discord_rpc,
# zahlen_interaction, or any plugin .so: TestPlugin loads every plugin .so
# dynamically at runtime through PluginManifestRegistry / SharedLibrary / App.
target_link_libraries(Extras_Application PRIVATE zahlen_application zahlen_screenshot zahlen_image)
set_property(TARGET Extras_Application APPEND PROPERTY BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
foreach(_zhln_app_plugin IN ITEMS
    zahlen_plugin_freecam
    zahlen_plugin_screenshot
    zahlen_plugin_character_controller
    zahlen_plugin_animation
    zahlen_plugin_terrain
    zahlen_plugin_vfx
    zahlen_plugin_gltf
    zahlen_plugin_bsp
    zahlen_plugin_discord_rpc
    zahlen_plugin_interaction
    zahlen_plugin_svg
    zahlen_plugin_http)
    if(TARGET ${_zhln_app_plugin})
        add_dependencies(Extras_Application ${_zhln_app_plugin})
    endif()
endforeach()
unset(_zhln_app_plugin)
if(TARGET zahlen_plugin_svg)
    target_compile_definitions(Extras_Application PRIVATE ZHLN_HAS_SVG_PLUGIN=1)
endif()
if(TARGET zahlen_plugin_http)
    target_compile_definitions(Extras_Application PRIVATE ZHLN_HAS_HTTP_PLUGIN=1)
endif()
zahlen_register_extras_test(Extras_Application POOL gpu TIMEOUT 180 LABELS "GPU;Extras;COM")
zahlen_add_extras_build_alias(Extras_Application TestApplication)
zahlen_add_extras_build_alias(Extras_Application TestPlugin)

# --- Grouped Non-Module Extras Binaries (6 links instead of 20) ---

# 1. Extras_Socket: TCP/UDP, local IPC, and Discord RPC/service integration.
zahlen_add_extras_group(Extras_Socket
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    TestSocket.cpp
    TestLocalStream.cpp
    TestDiscordRPC.cpp
)
# Discord::Encode/DecodeRpcFrame, which the rpc target owns.
target_link_libraries(Extras_Socket PRIVATE
    zahlen_engine
    zahlen_socket
    zahlen_discord_rpc
    zahlen_serialization
)
zahlen_register_extras_test(Extras_Socket)
zahlen_add_extras_build_alias(Extras_Socket TestSocket)
zahlen_add_extras_build_alias(Extras_Socket TestLocalStream)
zahlen_add_extras_build_alias(Extras_Socket TestDiscordRPC)

# 2. Extras_UI: UI schema, widgets, console debugger, and baked font loader.
zahlen_add_extras_group(Extras_UI
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    TestFontAsset.cpp
    TestGUIContext.cpp
    TestTextEdit.cpp
    TestHostUI.cpp
    TestUITree.cpp
    TestWidgets.cpp
    TestConsoleDebugger.cpp
    TestBakedFontLoader.cpp
)
target_link_libraries(Extras_UI PRIVATE
    zahlen_engine
    zahlen_application
    zahlen_ui
    zahlen_ui_schema
    zahlen_widgets
    zahlen_console
    zahlen_fonts
)
zahlen_register_extras_test(Extras_UI TIMEOUT 180)
zahlen_add_extras_build_alias(Extras_UI TestUITree)
zahlen_add_extras_build_alias(Extras_UI TestWidgets)
zahlen_add_extras_build_alias(Extras_UI TestConsoleDebugger)
zahlen_add_extras_build_alias(Extras_UI TestBakedFontLoader)

# 3. Extras_Simulation: TwoBoneIK (skeletal domain, module-free), terrain, ALife, DataModel.
zahlen_add_extras_group(Extras_Simulation
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    TestTwoBoneIK.cpp
    TestALife.cpp
    TestDataModel.cpp
)
target_link_libraries(Extras_Simulation PRIVATE
    zahlen_engine
    zahlen_animation
    zahlen_alife
    zahlen_datamodel
)
zahlen_register_extras_test(Extras_Simulation)
zahlen_add_extras_build_alias(Extras_Simulation TestTwoBoneIK)
zahlen_add_extras_build_alias(Extras_Simulation TestALife)
zahlen_add_extras_build_alias(Extras_Simulation TestDataModel)

# 4. Extras_Serialization: JSON, TOML, Lua, and zcook GLB container inspection.
zahlen_add_extras_group(Extras_Serialization
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    TestJSON.cpp
    TestTOML.cpp
    TestLua.cpp
    TestCookerGLB.cpp
)
target_link_libraries(Extras_Serialization PRIVATE
    zahlen_engine
    zahlen_serialization
    zahlen_lua
)
if(TARGET zcook)
    add_dependencies(Extras_Serialization zcook)
endif()
zahlen_register_extras_test(Extras_Serialization
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.."
)
zahlen_add_extras_build_alias(Extras_Serialization TestJSON)
zahlen_add_extras_build_alias(Extras_Serialization TestTOML)
zahlen_add_extras_build_alias(Extras_Serialization TestLua)
zahlen_add_extras_build_alias(Extras_Serialization TestCookerGLB)

# 5. Extras_HTTP: loopback HTTP, RemoteAsset cache/fetcher, and CDN layer.
if(TARGET zahlen_http)
    set(ZHLN_EXTRAS_HTTP_SOURCES
        RunExtrasHTTPTests.cpp
        TestHTTP.cpp
    )
    set(ZHLN_EXTRAS_HTTP_LIBS
        zahlen_engine
        zahlen_http
    )
    # Only the CDN entry is listed: zahlen_cdn is PUBLIC zahlen_remote_asset,
    # which is PUBLIC zahlen_http, so the chain arrives in order.
    if(TARGET zahlen_remote_asset)
        list(APPEND ZHLN_EXTRAS_HTTP_SOURCES TestRemoteAsset.cpp)
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

# Capability-only authoring must link without the legacy BSP/Interaction archive.
zahlen_add_extras_group(Extras_BSPAssets
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestBSPAssets.cpp TestBSPWorldAssets.cpp)
target_link_libraries(Extras_BSPAssets PRIVATE zahlen_bsp_assets zahlen_signal zahlen_command_line)
zahlen_register_extras_test(Extras_BSPAssets)

# 6. Extras_BSP: BSP interactive entities, VMT/VTF materials, static props, StudioModel, VRAD/lighting.
zahlen_add_extras_group(Extras_BSP
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp
    TestBSPInteraction.cpp
    TestBSPMaterials.cpp
    TestBSPStaticProps.cpp
    TestBSPStudioModel.cpp
    TestBSPLighting.cpp
)
# zahlen_interaction is PUBLIC zahlen_engine and PUBLIC zahlen_bsp_assets, and the
# character components these suites touch are PODs reached through
# ZHLN_OPTIONAL_INCLUDE_ROOTS. No BSP or Character:: symbol is called directly.
target_link_libraries(Extras_BSP PRIVATE
    zahlen_interaction
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

    # zahlen_lua directly: TestLuaDataModel.cpp includes <lua/Lua.hpp> and
    # calls Lua::VirtualMachine::Create / ExecuteString alongside DataModelLuaSession.
    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_lua
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
            -fno-sanitize=vptr
            -fno-omit-frame-pointer
        )
        target_link_options(${TARGET_NAME} PRIVATE
            -fsanitize=address,undefined
            -fno-sanitize=vptr
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

# No zahlen_character_controller: the profiling suites include
# <CharacterController/CharacterComponents.hpp> for PODs only and call no
# Character:: function. zahlen_serialization stays -- PerfBaseline.hpp parses
# and serializes baselines through ReflectJSON (the header, not the .cpp).
# Jolt is direct for the same reason zahlen_serialization is: the profiling
# suite references JPH symbols from its own object file rather than reaching
# them through the engine's DT_NEEDED entry.
target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine zahlen_ui
    zahlen_serialization
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
        -fno-sanitize=vptr
        -fno-omit-frame-pointer
    )
    target_link_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize=vptr
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)
endif()

zahlen_register_extras_test(${TARGET_NAME}
    POOL serial
)

set(TARGET_NAME TestScripting)
zahlen_wrap_ztest_sources(_scripting_sources _scripting_files
    scripting/TestScriptBinder.cpp
    scripting/TestCOMScriptLifetime.cpp
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
        -fno-sanitize=vptr
        -fno-omit-frame-pointer
    )
    target_link_options(${TARGET_NAME} PRIVATE
        -fsanitize=address,undefined
        -fno-sanitize=vptr
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
            -fno-sanitize=vptr
            -fno-omit-frame-pointer
        )
        target_link_options(${TARGET_NAME} PRIVATE
            -fsanitize=address,undefined
            -fno-sanitize=vptr
        )
        target_compile_definitions(${TARGET_NAME} PRIVATE __ASAN_ENABLED__)
    endif()

    zahlen_register_extras_test(${TARGET_NAME})
endif()

include(${CMAKE_CURRENT_LIST_DIR}/ProjectLightClientTests.cmake)

zahlen_add_extras_group(Extras_GLTFAssets
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestGLTFAssets.cpp)
target_link_libraries(Extras_GLTFAssets PRIVATE zahlen_gltf_assets zahlen_signal zahlen_command_line)
# ztest workers may run from a build/output directory, not the checkout root.
target_compile_definitions(Extras_GLTFAssets PRIVATE ZHLN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
zahlen_register_extras_test(Extras_GLTFAssets POOL gpu TIMEOUT 180 LABELS GPU Extras)


# Scene orchestration is a consumer library; this executable links no native adapter.
zahlen_add_extras_group(Extras_SceneAssets
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestSceneAssets.cpp)
target_link_libraries(Extras_SceneAssets PRIVATE zahlen_scene zahlen_serialization zahlen_signal zahlen_command_line)
zahlen_register_extras_test(Extras_SceneAssets POOL gpu LABELS GPU Extras)
if(TARGET zahlen_scene_documents)
    target_link_libraries(Extras_SceneAssets PRIVATE zahlen_scene_documents)
    target_compile_definitions(Extras_SceneAssets PRIVATE ZHLN_TEST_SCENE_DOCUMENTS=1)
endif()

if(TARGET zahlen_freecam AND TARGET zahlen_camera)
    zahlen_add_extras_group(Extras_CameraHosts
        ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestCameraHosts.cpp TestFramePipeline.cpp)
    target_link_libraries(Extras_CameraHosts PRIVATE zahlen_freecam zahlen_camera zahlen_application)
    zahlen_register_extras_test(Extras_CameraHosts POOL gpu TIMEOUT 180 LABELS GPU Extras)
endif()

if(TARGET zahlen_fallback_scene)
    zahlen_add_extras_group(Extras_FallbackHost
        ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestFallbackHost.cpp)
    target_link_libraries(Extras_FallbackHost PRIVATE zahlen_fallback_scene zahlen_application)
    zahlen_register_extras_test(Extras_FallbackHost POOL gpu LABELS GPU Extras)
endif()

# Real COM combat policy scenarios, also available without the optional GPU
# test catalog. No native VFX modules or registry fixtures are linked.
zahlen_add_extras_group(Extras_CombatHost
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp ../render/extras/TestCombatFX.cpp)
set_target_properties(Extras_CombatHost PROPERTIES CXX_SCAN_FOR_MODULES ON)
target_link_libraries(Extras_CombatHost PRIVATE zahlen_vfx)
zahlen_register_extras_test(Extras_CombatHost POOL gpu LABELS "GPU;Extras;COM")

zahlen_add_extras_group(Extras_TerrainHost
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestTerrain.cpp)
target_link_libraries(Extras_TerrainHost PRIVATE zahlen_terrain zahlen_vfx zahlen_scene zahlen_fallback_scene zahlen_application zahlen_screenshot)
target_include_directories(Extras_TerrainHost PRIVATE ${PROJECT_SOURCE_DIR}/utils/scene)
zahlen_register_extras_test(Extras_TerrainHost POOL gpu TIMEOUT 180 LABELS "GPU;Extras;COM")

zahlen_add_extras_group(Extras_LightningHost
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestLightning.cpp)
target_link_libraries(Extras_LightningHost PRIVATE zahlen_vfx)
zahlen_register_extras_test(Extras_LightningHost POOL gpu LABELS "GPU;Extras;COM")

zahlen_add_extras_group(Extras_ExplosionHost
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestExplosion.cpp)
target_link_libraries(Extras_ExplosionHost PRIVATE zahlen_vfx)
zahlen_register_extras_test(Extras_ExplosionHost POOL gpu LABELS "GPU;Extras;COM")

if(TARGET zahlen_editor_session)
    zahlen_add_extras_group(Extras_EditorHost
        ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestEditorHost.cpp)
    target_link_libraries(Extras_EditorHost PRIVATE zahlen_editor_session zahlen_screenshot)
    zahlen_register_extras_test(Extras_EditorHost POOL gpu LABELS "GPU;Extras;COM")
endif()

if(TARGET zahlen_benchmark)
    zahlen_add_extras_group(Extras_LogicWorld
        ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestLogicWorld.cpp)
    target_link_libraries(Extras_LogicWorld PRIVATE zahlen_benchmark)
    zahlen_register_extras_test(Extras_LogicWorld LABELS "Extras;COM;CPU")
endif()
if(TARGET zahlen_benchmark_viewer)
    zahlen_add_extras_group(Extras_BenchmarkHost
        ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestBenchmarkHost.cpp)
    target_link_libraries(Extras_BenchmarkHost PRIVATE zahlen_benchmark_viewer zahlen_ui zahlen_screenshot)
    zahlen_register_extras_test(Extras_BenchmarkHost POOL gpu LABELS "GPU;Extras;COM")
endif()
if(TARGET zahlen_console)
    zahlen_add_extras_group(Extras_ConsoleHost
        ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestConsoleHost.cpp)
    target_link_libraries(Extras_ConsoleHost PRIVATE zahlen_console zahlen_scene zahlen_screenshot zahlen_ui)
    zahlen_register_extras_test(Extras_ConsoleHost POOL gpu LABELS "GPU;Extras;COM")
endif()

# Engine-owned motor/playback controls: no native animation/controller dependency.
zahlen_add_extras_group(Extras_RuntimeControls
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestRuntimeControls.cpp)
target_link_libraries(Extras_RuntimeControls PRIVATE zahlen_gltf_assets zahlen_animation zahlen_application)
zahlen_register_extras_test(Extras_RuntimeControls POOL gpu TIMEOUT 240 LABELS GPU Extras)

# Capability-only character host: no native controller, Jolt or graph adapter.
zahlen_add_extras_group(Extras_CharacterHost
    ${PROJECT_SOURCE_DIR}/tests/TestsMain.cpp TestCharacterController.cpp)
target_link_libraries(Extras_CharacterHost PRIVATE zahlen_character_controller zahlen_application)
zahlen_register_extras_test(Extras_CharacterHost POOL gpu LABELS GPU Extras)
