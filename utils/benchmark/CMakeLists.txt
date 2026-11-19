# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# The zahlen_benchmark target is the headless workload library. This standalone
# executable has no game, editor, scene-document or viewer dependency.
add_executable(zahlen_benchmark_app main.cpp)
set_target_properties(zahlen_benchmark_app PROPERTIES CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF OUTPUT_NAME "zahlen-benchmark")
target_link_libraries(zahlen_benchmark_app PRIVATE zahlen_benchmark
    zahlen_command_line zahlen_warnings)
zahlen_set_bin_output(zahlen_benchmark_app)
zahlen_add_runtime_dlls(zahlen_benchmark_app)
zahlen_enable_reflection(zahlen_benchmark_app)
if(USE_SANITIZERS)
    zahlen_install_sanitizer_runtime(zahlen_benchmark_app)
endif()

# Real COM viewer host: GUI/context/atlas submission is owned here.
add_executable(zahlen_benchmark_viewer_app viewer.cpp)
set_target_properties(zahlen_benchmark_viewer_app PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF OUTPUT_NAME "zahlen-benchmark-viewer")
target_link_libraries(zahlen_benchmark_viewer_app PRIVATE zahlen_benchmark_viewer zahlen_ui zahlen_host_runner zahlen_warnings)
zahlen_enable_reflection(zahlen_benchmark_viewer_app)
zahlen_set_bin_output(zahlen_benchmark_viewer_app)
zahlen_add_runtime_dlls(zahlen_benchmark_viewer_app)
add_dependencies(zahlen_benchmark_viewer_app link_resources)
if(USE_SANITIZERS)
    zahlen_install_sanitizer_runtime(zahlen_benchmark_viewer_app)
endif()
