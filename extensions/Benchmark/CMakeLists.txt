# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/Benchmark -- the headless logic benchmark (zahlen_benchmark).
#
# A World stepped at a fixed timestep with the ECS pass a game loop would run
# over the bodies physics just wrote, and the wall time of each of those steps:
# measurement, not content. Core is the only dependency -- this target links
# zahlen_engine for the ECS and the PhysicsContext and compiles no GUI header,
# no render header and no Clay, which is what lets a CI job or a startup check
# time the engine without a device.
#
# The interactive overlay is the sub-target under Viewer/, which links this one
# and adds GUI::Context on top, the same way extensions/Scripting holds the
# generic binder and Lua/ holds the runtime that draws on it.
#
# Public headers are reached as <Benchmark/...>.

set(TARGET_NAME zahlen_benchmark)

add_library(${TARGET_NAME} STATIC
    Benchmark.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE.
    POSITION_INDEPENDENT_CODE ON
)

# Core is the whole dependency: the benchmark builds a standalone World, so it
# needs the ECS registry, the Jolt PhysicsContext behind it and nothing else.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

# Public headers are included as <Benchmark/...> by consumers, so the
# extensions root is the only include path this target needs to publish.
target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

# Benchmark.cpp reaches Components.hpp, whose key bitset is sized by
# Reflect::EnumCount<KeyCode>(): without the reflection flags that count is 0
# and every component after it moves, which is a silent wrong-offset read
# rather than a build error. Same reason Terrain and Camera carry this.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

# The GUI wrapper. Configured after this target because it links it.
add_subdirectory(Viewer)
