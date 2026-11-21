# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# extensions/UI -- immediate-mode UI runtime (zahlen_ui) and declarative UI
# document schema (zahlen_ui_schema).
#
# Core keeps only the renderer submission PODs (<Zahlen/GUI/UIData.hpp>) and
# cooked font container layout (<Zahlen/GUI/Font.hpp>). The Clay layout
# engine, UTF-8 text measurement/editing, baked font decoder/default font, and
# COM HostUI bridge all live here so libzahlen_engine carries zero UI layout
# state, zero Clay code, and zero GUI symbol exports.
#
# Public headers are reached as <UI/...>.

add_library(zahlen_ui STATIC
    FontLoader.cpp
    GUIContext.cpp
    HostUI.cpp
    Text.cpp
)

set_target_properties(zahlen_ui PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

set_source_files_properties(FontLoader.cpp PROPERTIES
    OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/resources/fonts/DefaultFont.zfont"
)

target_link_libraries(zahlen_ui
    PUBLIC zahlen_engine
    PRIVATE zahlen_warnings
)

target_include_directories(zahlen_ui PUBLIC ${PROJECT_SOURCE_DIR}/extensions)
target_compile_options(zahlen_ui PRIVATE "--embed-dir=${PROJECT_SOURCE_DIR}/resources/fonts")

# Clay is strictly private to GUIContext.cpp; no consumer sees <clay.h>.
target_include_directories(zahlen_ui SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/clay
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_ui)
endif()

add_library(zahlen_ui_schema STATIC
    UITree.cpp
)

set_target_properties(zahlen_ui_schema PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(zahlen_ui_schema
    PUBLIC zahlen_ui
    PRIVATE zahlen_warnings
)

target_include_directories(zahlen_ui_schema PUBLIC
    ${PROJECT_SOURCE_DIR}/extensions
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(zahlen_ui_schema)
endif()
