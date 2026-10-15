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
# SystemTelemetry is the machine under the frames -- CPU usage and clocks, per
# core as well as machine-wide, and the GPU's utilization and clock. Its header
# names no platform and contains no syscall: the counters arrive through a probe
# whose platform code lives entirely in translation units listed below, so the
# header is what a portable consumer sees and never grows a #if. On Linux the
# probe reads procfs, cpu frequency and the DRM counters, and drives NVML
# through the engine's shared-library wrapper without ever including <nvml.h>;
# elsewhere it reports that it has nothing to read.
#
# Public headers are reached as <Benchmark/Viewer/...>.

set(TARGET_NAME zahlen_benchmark_viewer)

add_library(${TARGET_NAME} STATIC
    BenchmarkViewer.cpp
    FrameTelemetry.cpp
    SystemTelemetry.cpp
    SystemProbeLinux.cpp
    SystemProbeUnavailable.cpp
)

# The two probe files are alternatives chosen by the platform, not by the build
# system: exactly one of them is non-empty after preprocessing and the other
# compiles to nothing. SystemProbeLinux.cpp defines the factory only under
# __linux__; SystemProbeUnavailable.cpp defines the same factory only outside
# it, and its probe states why there is nothing to read. Listing both is what
# keeps a platform #if out of this file and out of the headers.

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
#
# zahlen_widgets is where the panel's drawing patterns live: the rolling window
# the frame-time chart is a projection of, the bar strips the chart, the stage
# sparklines and the core cells are drawn as, the card an element opens on
# hover, and the text measurement that keeps a line inside its column. PRIVATE
# because none of it reaches this target's public header -- a consumer of the
# viewer gets a viewer, not a widget library.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_benchmark
    zahlen_engine
)

target_link_libraries(${TARGET_NAME} PRIVATE
    zahlen_widgets
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
