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
endfunction()

# No extern/cgltf_impl.c here: the importer moved into extras, so zahlen_extras
# compiles the one copy of it and this target gets the cgltf symbols (and the
# header path) through that link. Compiling it again would be a duplicate
# definition at link time.
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
    zahlen_extras
)

# Public headers only. src/ is banned — see tools/check_tests_public_api.py.
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
    zahlen_extras
)

# Public headers only. src/ is banned — see tools/check_tests_public_api.py.
target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/extras
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
# extras/json: JSON.hpp is the opaque simdjson document reader; JSONSchema.hpp
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
    zahlen_extras
)

# Public headers only. src/ is banned — see tools/check_tests_public_api.py.
target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/extras
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
# extras/toml: the reflection-driven document layer, plus SceneTOML.hpp, which
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
    zahlen_extras
)

# Public headers only. src/ is banned — see tools/check_tests_public_api.py.
target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/extras
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

# --- Network Wire / Serialization Test Suite ---
# Exercises ZHLN.Wire (dependency-free reflection serialization) and the
# ZHLN.Network protocol codecs: annotated round-trips, hostile input sweeps,
# quantization, framing, CRC32 and block compression.
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
    zahlen_extras
)

# Public headers only. src/ is banned — see tools/check_tests_public_api.py.
target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/extras
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
# perf-baseline.json through extras/json and defines VerifyBaseline(), the
# assertion every baseline check goes through. Core-only groups include
# TestsFramework.hpp, which no longer pulls any of that in.
set(TARGET_NAME TestPerformance)
file(GLOB PROFILE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/profile/*.cpp")

add_executable(${TARGET_NAME} ${PROFILE_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_extras
)

# Public headers only. src/ is banned — see tools/check_tests_public_api.py.
target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/extras
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
# the emitted chunk back with extras/json and asserts the document declares the
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
    zahlen_extras
)

# Public headers only. src/ is banned — see tools/check_tests_public_api.py.
target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/extras
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
# Lua-independent half of the scripting layer (extras/Scripting); the suite
# links the Lua runtime so both halves are built and exercised together. This
# is the only test target that links zahlen_scripting_lua, and it is the reason
# CPU_ECS no longer covers the bridge: a build without the scripting extra has
# nothing to bridge.
set(TARGET_NAME TestScripting)
file(GLOB SCRIPTING_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripting/*.cpp")

add_executable(${TARGET_NAME} ${SCRIPTING_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_engine
    zahlen_scripting_lua
)

# Public headers only. src/ is banned — see tools/check_tests_public_api.py.
target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/extras
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
# extras/Console: GameConsole + ConsoleDebugger. Built as zahlen_console so
# the extras archive does not pull the scripting binder into every consumer.
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

# Public headers only. src/ is banned — see tools/check_tests_public_api.py.
target_include_directories(${TARGET_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/tests
    ${PROJECT_SOURCE_DIR}/extras
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
# extras/SVG is optional twice over: the extra is only configured when
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

    # Public headers only. src/ is banned — see tools/check_tests_public_api.py.
    # ${PROJECT_SOURCE_DIR}/extras is what makes <SVG/SVG.hpp> resolve; resvg's
    # own include path stays PRIVATE to zahlen_svg and is not needed here.
    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/extras
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
# extras/HTTP is optional the same way extras/SVG is: the extra is only
# configured when ZHLN_BUILD_EXTRAS is on, and its target only exists when
# HTTP/CMakeLists.txt found libcurl. The suite follows the target, so a machine
# without libcurl gets neither zahlen_http nor TestHTTP -- and no failure from
# either. The suite talks to a loopback server it starts itself (see the header
# of TestHTTP.cpp), so it needs no network and no third-party host.
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

    # Public headers only. src/ is banned — see tools/check_tests_public_api.py.
    # ${PROJECT_SOURCE_DIR}/extras is what makes <HTTP/HTTP.hpp> resolve;
    # libcurl's own include path stays PRIVATE to zahlen_http and is not needed
    # here.
    target_include_directories(${TARGET_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/tests
        ${PROJECT_SOURCE_DIR}/extras
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
