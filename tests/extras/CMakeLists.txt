# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT ZHLN_BUILD_EXTRAS OR NOT TARGET zahlen_extras)
    return()
endif()

# Extras suites are registered ad hoc (one add_test per target), unlike the CPU
# groups in tests/CMakeLists.txt. Route every extras test through this helper so
# it receives the shared sanitizer environment, plus the opt-in Vulkan sandbox
# pin — these suites are CPU-labelled, so they follow the same rule as the CPU
# groups: pinned only when ZHLN_TEST_VULKAN_SANDBOX is ON.
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

    # The suites here shell out to zcook, whose own DLLs are in the build tree.
    zahlen_add_runtime_dll_path(${TARGET_NAME})
    zahlen_add_runtime_dlls(${TARGET_NAME})
endfunction()

# Suites below link the layer target they exercise, not the zahlen_extras
# aggregate: TestALife needs no JSON reader, TestNetworkWire needs no glTF
# importer, and neither should be waiting on the other's module interfaces to
# build. zahlen_extras remains only as a downstream compatibility target; these
# in-tree suites and samples link their selected domains directly.
#
# No plugins/glTF/cgltf_impl.c here: the importer lives in plugins/glTF, so zahlen_gltf
# compiles the one copy of it and this target gets the cgltf symbols (and the
# header path) through that link. Compiling it again would be a duplicate
# definition at link time. The suite needs two domains: zahlen_animation for the
# ZHLN.Locomotion and ZHLN.ProceduralAnimation interfaces it imports, and
# zahlen_gltf for the independent cgltf parse it checks the rig against.
set(TARGET_NAME TestProceduralAnimation)
add_executable(${TARGET_NAME}
    TestProceduralAnimation.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_animation
    zahlen_gltf
)

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
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

# --- UI Schema Suite ---
# The data-driven UI schema (UINode / ActionRegistry / PropertyStore /
# RenderUITree) left core for extensions/UI; its suite followed. Walks a
# CPU-constructed tree through the registry-only GUI::Context constructor,
# so it needs no GPU -- just the engine and the schema domain.
set(TARGET_NAME TestUITree)
add_executable(${TARGET_NAME} TestUITree.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_ui_schema
)

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
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

# --- Two-Bone IK Solver Suite ---
# The analytic IK toolkit (solver, chain component, pose post-processor) left
# core for extensions/Animation, and its solver test followed: the suite only
# compiles when that domain exists. Same shape as the other extras suites --
# one executable, public headers only.
set(TARGET_NAME TestTwoBoneIK)
add_executable(${TARGET_NAME} TestTwoBoneIK.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_animation
)

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
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

# Terrain's heightmap slots are external resources even without GPU meshes.
# Exercise explicit release for standalone ECS scenes on a CPU-only runner.
set(TARGET_NAME TestTerrainLifecycle)
add_executable(${TARGET_NAME} TestTerrainLifecycle.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_terrain
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

# --- ALife Test Suite ---
set(TARGET_NAME TestALife)
add_executable(${TARGET_NAME} TestALife.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_alife
)

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
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

# --- JSON Test Suite ---
# plugins/json: JSON.hpp is the opaque simdjson document reader; JSONSchema.hpp
# is the reflection-driven serialisation layer (typed read and write) plus the
# compile-time schema parser. Moved out of tests/core when JSON became
# an extra; it is the reason the CPU test groups cannot all link the core engine
# alone (see tests/CMakeLists.txt).
set(TARGET_NAME TestJSON)
add_executable(${TARGET_NAME} TestJSON.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_serialization
)

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
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

# --- TOML Test Suite ---
# plugins/toml: the reflection-driven document layer, plus SceneTOML.hpp, which
# binds Jolt vectors so a core ZHLN::Scene::Scene reads/writes as a document.
set(TARGET_NAME TestTOML)
add_executable(${TARGET_NAME} TestTOML.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_serialization
)

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
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

# --- DataModel Instance graph Suite ---
# Exercises stable instance IDs, typed class factories, parent/descendant events,
# reference validation and the optional ECS entity binding without requiring a
# Lua runtime or a second scripting object graph.
set(TARGET_NAME TestDataModel)
add_executable(${TARGET_NAME} TestDataModel.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_datamodel
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

# --- LuaJIT / DataModel Integration Suite ---
# Exercises the DataModel adapter through the existing scripting runtime and
# proves that Lua userdata refer to the same C++ Instance graph.
if(TARGET zahlen_scripting_lua)
    set(TARGET_NAME TestLuaDataModel)
    add_executable(${TARGET_NAME} TestLuaDataModel.cpp)

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_scripting_lua
        zahlen_datamodel
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

# --- Network Wire / Serialization Test Suite ---
# Exercises ZHLN.Wire (dependency-free reflection serialization) and the
# ZHLN.Network protocol codecs: annotated round-trips, hostile input sweeps,
# quantization, framing, CRC32 and block compression.
#
# zahlen_network and nothing else: `import ZHLN.Network;` is the suite's only
# extras dependency, and the isolation zahlen_network exists for is what makes
# this target build without the renderer, simdjson or cgltf.
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

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
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

# --- Performance Baseline Group (tests/extras/profile) ---
# Moved out of tests/perf when the baseline store was recognized for what it
# is: a JSON document. extras/profile/PerfBaseline.hpp reads and writes
# perf-baseline.json through plugins/json and defines VerifyBaseline(), the
# assertion every baseline check goes through. Core-only groups include
# TestsFramework.hpp, which no longer pulls any of that in.
# Sources are listed, not globbed, the same way the CPU groups in
# ../CMakeLists.txt now list theirs: a new Test*.cpp under profile/ has to be
# added here or it is silently unbuilt. PerfBaseline.hpp is the group's shared
# header and is deliberately not a source.
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

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
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

# --- Cooker glTF Container Suite ---
# The one cooker check that needs a JSON reader: it runs `zcook glb`, then reads
# the emitted chunk back with plugins/json and asserts the document declares the
# glTF 2.0 members. The framing checks and everything else the cooker does stay
# in CPU_Assets, which links the core engine alone.
set(TARGET_NAME TestCookerGLB)
add_executable(${TARGET_NAME} TestCookerGLB.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_serialization
)

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
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

# The suite shells out to the zcook binary, which is built at the root of the
# build tree and found by a working-directory-relative search (see
# tests/helpers/CookerFixture.hpp). This add_test lives two directories below
# that root, so the working directory is pinned to the same one CPU_Assets
# inherits by default; without it the lookup resolves to tests/zcook.
zahlen_register_extras_test(${TARGET_NAME}
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/.."
)

# --- Scripting Suite (tests/extras/scripting) ---
# ScriptBinder, ScriptECSBridge and ScriptValueTypes are the generic,
# Lua-independent half of the scripting layer (extensions/Scripting); the suite
# links the Lua runtime so both halves are built and exercised together. This
# is the only test target that links zahlen_scripting_lua, and it is the reason
# CPU_ECS no longer covers the bridge: a build without the scripting extra has
# nothing to bridge.
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
    zahlen_scripting_lua
)

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
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

# --- Console Debugger Suite ---
# extensions/Console: GameConsole + ConsoleDebugger. Built as zahlen_console so
# the compatibility aggregate does not pull the scripting binder into every consumer.
set(TARGET_NAME TestConsoleDebugger)
add_executable(${TARGET_NAME} TestConsoleDebugger.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_console
)

# Public headers only. src/ is banned — see configure/check_tests_public_api.py.
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

# --- SVG Test Suite ---
# plugins/SVG is optional twice over: the extra is only configured when
# ZHLN_BUILD_EXTRAS is on, and its target only exists when SVG/CMakeLists.txt
# found resvg. The suite follows the target, so a machine without resvg gets
# neither zahlen_svg nor TestSVG -- and no failure from either.
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

    # Public headers only. src/ is banned — see configure/check_tests_public_api.py.
    # ${ZHLN_OPTIONAL_INCLUDE_ROOTS} is what makes <SVG/SVG.hpp> resolve; resvg's
    # own include path stays PRIVATE to zahlen_svg and is not needed here.
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

# --- HTTP Test Suite ---
# extensions/net/HTTP is optional the same way plugins/SVG is: the extra is only
# configured when ZHLN_BUILD_EXTRAS is on, and its target only exists when
# HTTP/CMakeLists.txt found libcurl. The suite follows the target, so a machine
# without libcurl gets neither zahlen_http nor TestHTTP -- and no failure from
# either. The suite talks to a loopback server it starts itself
# (extensions/net/HTTP/HTTPServer.hpp), so it needs no network and no third-party host.
# --- Baked Font Loader Suite ---
# plugins/Fonts' fontbm path: the AngelCode BMFont descriptor scan and the
# bake assembly feeding core's baked-font hook. Pure CPU over fixture bytes --
# the production bakes are made offline (fontbm / `zcook font`), so no suite
# here invokes an outline-font parser.
set(TARGET_NAME TestBakedFontLoader)
add_executable(${TARGET_NAME} TestBakedFontLoader.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_fonts
)

# Public headers only. src/ is banned — see tools/check_tests_public_api.py.
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

if(TARGET zahlen_http)
    set(TARGET_NAME TestHTTP)
    add_executable(${TARGET_NAME} TestHTTP.cpp)

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_engine
        zahlen_http
    )

    # Public headers only. src/ is banned — see configure/check_tests_public_api.py.
    # ${ZHLN_OPTIONAL_INCLUDE_ROOTS} is what makes <HTTP/HTTP.hpp> resolve;
    # libcurl's own include path stays PRIVATE to zahlen_http and is not needed
    # here.
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

# --- Remote Asset Test Suite ---
# extensions/net/RemoteAsset follows extensions/net/HTTP's target, which follows libcurl: a
# machine without libcurl gets neither zahlen_remote_asset nor
# TestRemoteAsset -- and no failure from either. The pure half (URL rewrites,
# the GLB validator, the disk cache) runs against a temporary directory, and
# the async half talks to the loopback server of extensions/net/HTTP
# (HTTPServer.hpp), like TestHTTP: 127.0.0.1 on an ephemeral port, no
# network.
if(TARGET zahlen_remote_asset)
    set(TARGET_NAME TestRemoteAsset)
    add_executable(${TARGET_NAME} TestRemoteAsset.cpp)

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_engine
        zahlen_remote_asset
        zahlen_http
    )

    # Public headers only. src/ is banned — see configure/check_tests_public_api.py.
    # ${ZHLN_OPTIONAL_INCLUDE_ROOTS} is what makes <RemoteAsset/...> and
    # <HTTP/HTTPServer.hpp> resolve; libcurl's own include path stays PRIVATE
    # to zahlen_http and is not needed here.
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

# --- CDN Test Suite ---
# extensions/net/CDN is a base-URL layer over extensions/net/RemoteAsset. Built only
# when zahlen_cdn exists, which in turn needs libcurl. Every assertion runs
# against the loopback server of extensions/net/HTTP, so no test touches a real
# CDN host.
if(TARGET zahlen_cdn)
    set(TARGET_NAME TestCDN)
    add_executable(${TARGET_NAME} TestCDN.cpp)

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        zahlen_engine
        zahlen_cdn
        zahlen_remote_asset
        zahlen_http
    )

    # Public headers only, as TestRemoteAsset does: <CDN/...> and <RemoteAsset/...>
    # resolve through ${ZHLN_OPTIONAL_INCLUDE_ROOTS}.
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
