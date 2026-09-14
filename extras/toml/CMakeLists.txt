# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/toml -- the TOML half of zahlen_serialization.
#
# The target is created one directory up, in ../CMakeLists.txt; see
# ../json/CMakeLists.txt for why the two directories are one target. What lives
# here is this directory's translation unit.
#
#   TOML.hpp       reflection-driven TOML read and write
#   SceneTOML.hpp  Jolt vector bindings; what makes a core ZHLN::Scene::Scene a
#                  document. This is the header the composition root's Ctrl+S
#                  path serialises through (app/main.cpp, ZHLN_HAS_SCENE_TOML).
#   UITOML.hpp     the same for GUI::UINode (app/UIEditor.cpp, ZHLN_HAS_UI_TOML).
#
# No third-party dependency: the parser is charconv and a hand-rolled reader.
# UITOML.hpp reaches <Zahlen/gui/UITree.hpp>, which is a public core header under
# include/, so the engine link the parent sets up already covers it -- the GUI
# implementation (zahlen_gui) is not needed to compile or link this.

file(GLOB ZHLN_TOML_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

if(ZHLN_TOML_SOURCES)
    target_sources(zahlen_serialization PRIVATE ${ZHLN_TOML_SOURCES})
endif()
