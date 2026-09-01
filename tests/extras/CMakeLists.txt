# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT ZHLN_BUILD_EXTRAS OR NOT TARGET zahlen_extras)
    return()
endif()

# Extras suites are registered ad hoc (one add_test per target), unlike the CPU
# groups in tests/CMakeLists.txt, which is why they never received
# ZHLN_TEST_ENVIRONMENT. In sanitizer builds that meant LSAN_OPTIONS (and thus
# lsan.supp) was never set for them -- the JACK-related leak in TestPerformance
# is the visible symptom. Route every extras test through this helper so it
# carries the same ASan/LSan/UBSan options as the CPU groups.
function(zahlen_register_extras_test TARGET_NAME)
    add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
    if(USE_SANITIZERS)
        set_tests_properties(${TARGET_NAME} PROPERTIES
            LABELS "CPU;Fast;Extras"
            ENVIRONMENT "${ZHLN_TEST_ENVIRONMENT}"
            ${ARGN}
        )
    else()
        set_tests_properties(${TARGET_NAME} PROPERTIES
            LABELS "CPU;Fast;Extras"
            ${ARGN}
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
# extras/json: the reflection-driven reader (simdjson behind an opaque handle)
# and the compile-time schema parser. Moved out of tests/core when JSON became
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
# is what makes a core ZHLN::Scene::Scene readable and writable as a document.
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
# ScriptBinder and ScriptECSBridge moved out of core with the Lua layer, so
# their suites moved too. This is the only test target that links
# zahlen_scripting_lua, and it is the reason CPU_ECS no longer covers the
# bridge: a build without the scripting extra has nothing to bridge.
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
