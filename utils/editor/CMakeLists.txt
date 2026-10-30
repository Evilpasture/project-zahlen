# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# The low-level zahlen_editor target is an editor library; this is its dedicated
# executable with a paused simulation loop, picking and TOML scene import/save.
add_executable(zahlen_world_editor main.cpp)
set_target_properties(zahlen_world_editor PROPERTIES CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF OUTPUT_NAME "zahlen-editor")
target_link_libraries(zahlen_world_editor PRIVATE zahlen_application
    zahlen_editor zahlen_freecam zahlen_scene_documents zahlen_warnings)
target_include_directories(zahlen_world_editor PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(zahlen_world_editor PRIVATE JPH_DOUBLE_PRECISION)
zahlen_set_bin_output(zahlen_world_editor)
zahlen_add_runtime_dlls(zahlen_world_editor)
add_dependencies(zahlen_world_editor link_resources)
zahlen_enable_reflection(zahlen_world_editor)
if(USE_SANITIZERS)
    zahlen_install_sanitizer_runtime(zahlen_world_editor)
endif()
