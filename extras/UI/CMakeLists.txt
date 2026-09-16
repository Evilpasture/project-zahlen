# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/UI -- the data-driven UI schema (zahlen_ui_schema).
#
# UINode / NodeKind / TreeMode document types plus the ActionRegistry and
# PropertyStore tables a document names, and RenderUITree, the walk that turns
# a description into one frame of GUI::Context calls. This is the schema the
# UI-tree editor (zahlen_ui_editor) authors and extras/toml round-trips --
# presentation data, not substrate. Core keeps src/gui/: the immediate-mode
# Clay wrapper, TextEdit and the font rasterizer.
#
# The namespace stays ZHLN::GUI (the schema reads as part of the GUI family
# and consumers are untouched by the move); the ZHLN_API symbols are compiled
# here and exported through libzahlen_engine like any other engine entry.
#
# Public headers are reached as <UI/...>.

set(TARGET_NAME zahlen_ui_schema)

file(GLOB ZHLN_UI_SCHEMA_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_library(${TARGET_NAME} STATIC ${ZHLN_UI_SCHEMA_SOURCES})

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # Every static library in this tree that ends up inside a shared library or
    # a PIE executable sets this explicitly -- there is no global
    # CMAKE_POSITION_INDEPENDENT_CODE.
    POSITION_INDEPENDENT_CODE ON
)

# Core is a public dependency: the schema walks GUI::Context's public API,
# pushes through ECS::EventBus and uses Jolt storage vectors in NodeBox.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)
