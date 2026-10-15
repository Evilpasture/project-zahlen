# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/Benchmark/Viewer -- the interactive benchmark overlay
# (zahlen_benchmark_viewer).
#
# The same LogicBenchmark the parent target measures, drawn as a Clay panel in
# an Engine-hosted window and advanced one fixed 1/60 s tick per 1/60 s of wall
# time. This is the only half that names a GUI header or the Engine: a host
# linking it names the numbers and the panel together, one linking only
# zahlen_benchmark names the numbers alone.
#
# FrameTelemetry is the second panel this half owns -- the host's own frames
# (average FPS, 1% / 0.1% frame-time lows) plus every GPU stage and CPU scope
# the engine's profiler holds, and a CSV capture of the same numbers. It is a
# source file here rather than a target of its own because it is only ever
# drawn by these panels: the logic benchmark is what a non-GUI consumer links.
#
# Public headers are reached as <Benchmark/Viewer/...>.

set(TARGET_NAME zahlen_benchmark_viewer)

add_library(${TARGET_NAME} STATIC
    BenchmarkViewer.cpp
    FrameTelemetry.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# zahlen_benchmark is PUBLIC because the viewer's public header hands out a
# LogicBenchmark* (GetBenchmark), so a consumer of this target is a consumer of
# that type's definition. Core comes with it -- the panel is drawn through
# GUI::Context and the window through Engine -- and is named for clarity.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_benchmark
    zahlen_engine
)

# Public headers are included as <Benchmark/Viewer/...> by consumers, so the
# extensions root is the include path this target publishes, like its parent.
target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

# BenchmarkViewer.cpp reaches Components.hpp (see the parent's note): the same
# reflection flags every component-reading domain carries.
if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
