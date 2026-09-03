// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// include/Zahlen/GUIEditor.hpp
//
// The self-hosting editor's first native panels: a scene Hierarchy and a
// reflection-driven Inspector, built entirely from ZHLN::GUI primitives.
// No ImGui, no editor-private widget vocabulary — the editor is an ordinary
// UI scene that edits another UI scene.
//
// Design notes:
//
//   * The editor draws into the SAME GUI::Context and Registry as the content
//     it edits. To keep the editor out of its own hierarchy, EditorState
//     carries `editorRoot`: every entity at or below that node is filtered
//     out of the hierarchy and never inspectable through it.
//
//   * The inspector is generic. Component fields are enumerated through
//     ZHLN::Reflect::ForEachFieldWithName, so a new component (or a new field
//     on an existing one) shows up in the inspector without touching this
//     file. Field dispatch: float -> Slider, bool -> Checkbox, int -> Slider
//     with step 1, enum -> Dropdown (via ZHLN::Reflect::EnumNames), String64/
//     String256 -> TextInput, JPH::Vec4 -> four sliders. Handles, textures
//     and padding fields (leading '_') get no row in this version.
//
//   * The reflection iteration lives in src/gui/GUIEditor.cpp, not here.
//     Compilers without P2996 build the engine through
//     tools/transpile_reflection.py, which rewrites reflection calls by
//     source offset within a translation unit; calls inside headers would be
//     rewritten against the wrong buffer. Keeping the call in the .cpp makes
//     the native-reflection and transpiled toolchain paths behave identically.
//
//   * Both panels are plain frame functions: call them once per frame inside
//     your editor layout (a Columns split, a dock, ...) and they rebuild or
//     reclaim their widget subtree through the Context's child cache, exactly
//     like every compound widget in GUI.hpp. They return the panel root
//     entity, so the caller can measure or hide it like any other widget.

#pragma once

#include <Zahlen/Entity.hpp>
#include <string_view>

namespace ZHLN::GUI {
class Context;
}

namespace ZHLN::Editor {

/// Persistent editor state, owned by the host application (one instance per
/// editor window). The panels read and update it every frame; nothing else in
/// the engine sees it.
struct EditorState {
    /// Entity shown in the inspector. Entity::Null() = nothing selected.
    ZHLN::Entity selectedEntity = ZHLN::Entity::Null();

    /// Root of the editor's own widget subtree. Entities at or below this
    /// node are hidden from the hierarchy so the editor never lists (or
    /// lets you select) its own chrome. Null = no filtering.
    ZHLN::Entity editorRoot = ZHLN::Entity::Null();
};

/// Draws the scene hierarchy (against the registry the context already owns):
/// one selectable row per UI entity (anything with a UIRectComponent that is
/// not part of the editor's own subtree), ordered by
/// (hierarchyDepth, layoutOrder) — the same key layout, render and hit-testing
/// sort by, so the list matches what is on screen. Clicking a row writes
/// `state.selectedEntity`.
///
/// Returns the panel root entity (a scroll box), for callers that want to
/// measure or hide the panel; like every closure-form widget in GUI.hpp,
/// discarding it is normal.
auto DrawHierarchyPanel(
    ZHLN::GUI::Context& gui,
    EditorState&        state,
    std::string_view    id = "Hierarchy"
) -> ZHLN::Entity;

/// Draws the inspector for `state.selectedEntity`: a labelled header plus one
/// collapsing section per editable component present on the entity
/// (Name, Rect, Flex, Panel, Text). With no live selection the panel shows a
/// "No selection" placeholder. Field rows are generated from reflection; see
/// the dispatch table in the file comment above.
///
/// Returns the panel root entity (a scroll box); discarding it is normal,
/// matching the closure-form widget convention.
auto DrawInspectorPanel(
    ZHLN::GUI::Context& gui,
    EditorState&        state,
    std::string_view    id = "Inspector"
) -> ZHLN::Entity;

} // namespace ZHLN::Editor
