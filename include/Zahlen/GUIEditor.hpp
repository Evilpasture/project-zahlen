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
//   * The editor draws into the same GUI::Context as the scene it edits.
//     The registry is passed separately: the new Clay-based GUI::Context is
//     pure layout and does not own or expose ECS state.
//
//   * The inspector is generic. Component fields are enumerated through
//     ZHLN::Reflect::ForEachFieldWithName, so a new component (or a new field
//     on an existing one) shows up in the inspector without touching this
//     file. Field dispatch: float -> Slider, bool -> Checkbox, int -> Slider
//     with step 1, enum -> Dropdown (via ZHLN::Reflect::EnumNames), String64/
//     String256 -> TextInput stub, JPH::Vec4 -> four sliders. Handles,
//     textures and padding fields (leading '_') get no row in this version.
//
//   * The reflection iteration lives in src/gui/GUIEditor.cpp, not here.
//
//   * Both panels are plain frame functions: call them once per frame inside
//     your editor layout (a Columns split, a dock, ...).

#pragma once

#include <Zahlen/Entity.hpp>
#include <string_view>

namespace ZHLN::ECS {
class Registry;
}

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

/// Draws the scene hierarchy: one selectable row per named entity that is not
/// part of the editor's own subtree. Clicking a row writes
/// `state.selectedEntity`.
///
/// NOTE: `reg` is separate from `gui` because the new Clay-based GUI::Context
/// is stateless w.r.t. the ECS — the editor reads the registry directly.
void DrawHierarchyPanel(
    ZHLN::GUI::Context&    gui,
    ZHLN::ECS::Registry&   reg,
    EditorState&           state,
    std::string_view       id = "Hierarchy"
);

/// Draws the inspector for `state.selectedEntity`: a labelled header plus one
/// collapsing section per editable component present on the entity.
/// With no live selection the panel shows a "No selection" placeholder.
///
/// NOTE: `reg` is separate from `gui` for the same reason as above.
void DrawInspectorPanel(
    ZHLN::GUI::Context&    gui,
    ZHLN::ECS::Registry&   reg,
    EditorState&           state,
    std::string_view       id = "Inspector"
);

} // namespace ZHLN::Editor
