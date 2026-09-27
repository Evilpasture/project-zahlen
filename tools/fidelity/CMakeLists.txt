# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# tools/fidelity/CMakeLists.txt
#
# The fidelity runner's host tool: `fidelity list|compare|report`. It is a
# zcook-style host tool -- no renderer, no window. Link set mirrors what
# samples/FidelityHarness proves works for this exact API surface:
#
#   * zahlen_serialization (extras/json) for ReflectJSON::Document/ValueReader
#     and <json/JSONSchema.hpp>; it carries the extras include root.
#   * zahlen_engine for the stb_image DECODE implementation
#     (src/engine/stbi_impl.c) and the Zahlen/<...> headers.
#   * Jolt for JPH_DOUBLE_PRECISION's Vec types (FidelityCore.hpp uses none,
#     but the shared compile flags stay uniform with the harness).
#
#   * FidelityCore.hpp is dependency-free: the metric, PPM reader and downscale.
#   * ImageIO.cpp owns the one stb_image_write implementation in this binary
#     (same TU boundary as tools/zcook/GLB.cpp); the decode half belongs to the
#     engine, so STB_IMAGE_IMPLEMENTATION is NOT redefined here.
#   * main.cpp is the CLI surface.
#
# The render half stays in samples/FidelityHarness; this tool never initialises
# the GPU. A full run is samples/FidelityHarness (render) driven by
# tools/fidelity (list/compare/report) under scripts/run_fidelity.sh + Ninja.

add_executable(fidelity
    main.cpp
    ImageIO.cpp
)

set_target_properties(fidelity PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CXX_SCAN_FOR_MODULES ON
)

target_link_libraries(fidelity PRIVATE
    zahlen_engine
    zahlen_serialization
    Jolt
)

target_include_directories(fidelity SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/stb
    ${CMAKE_SOURCE_DIR}/extras
    ${CMAKE_SOURCE_DIR}/tools/fidelity
)
target_include_directories(fidelity PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(fidelity PRIVATE JPH_DOUBLE_PRECISION)

# Reflection gate, identical to every target that reaches a reflected header
# through zahlen_serialization (JSONSchema.hpp walks aggregates reflectively).
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(fidelity)
endif()

target_link_libraries(fidelity PRIVATE zahlen_warnings)
