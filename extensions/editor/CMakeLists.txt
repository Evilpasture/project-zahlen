# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/editor -- COM world-editor panels (Hierarchy + Inspector).
#
# A separate STATIC target -- not folded into a shared aggregate: the composition
# root links it only when present, and a host that does not want the editor
# is not forced to compile it. Public headers are included as
# <editor/GUIEditor.hpp>.

set(TARGET_NAME zahlen_editor)

add_library(${TARGET_NAME} STATIC
    GUIEditor.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
    zahlen_ui
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

# The editor is UI-dependent: only build its loadable plugin when the UI
# extension is present, mirroring how app/CMakeLists.txt guards zahlen_ui_editor.
if(COMMAND zahlen_add_plugin AND TARGET zahlen_ui)
    zahlen_add_plugin(zahlen_plugin_editor editor EditorPlugin.cpp)
    target_link_libraries(zahlen_plugin_editor PRIVATE zahlen_editor)
    target_include_directories(zahlen_plugin_editor PUBLIC
        ${PROJECT_SOURCE_DIR}/extensions)
endif()

configure_file(zahlen_plugin_editor.manifest.toml
    ${CMAKE_BINARY_DIR}/lib/zahlen_plugin_editor.manifest.toml COPYONLY)
