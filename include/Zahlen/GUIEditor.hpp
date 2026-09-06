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

#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <span>
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

// ============================================================================
// The CRUD operations the panels are built from
// ============================================================================
//
// These are separate from the panel functions on purpose. A panel needs a
// GUI::Context, a viewport and a frame to do anything; these need only a
// registry, so "create an entity", "destroy the selection" and "add/remove a
// component" are callable from a script, a test or a key binding without
// building a single widget -- and testable on a machine with no GPU.

/// Creates an entity with the components every scene object starts with: a
/// name, a local transform and the world transform cached from it. @p name
/// defaults to "Entity <index>", matching the label the hierarchy shows for an
/// unnamed entity.
///
/// The result is an empty game object: it renders nothing until it is given a
/// MeshComponent or a LightComponent. That is deliberate. Both of those own
/// something the editor cannot conjure -- a GPU mesh and material, a body in
/// the physics world -- so "new entity" cannot mean "new box" from in here.
/// Spawning geometry is CreativeWorksFactory's job and needs an Engine.
ZHLN_API auto CreateEntity(ZHLN::ECS::Registry& reg, std::string_view name = {}) -> ZHLN::Entity;

/// Destroys `state.selectedEntity` and clears the selection.
///
/// The selection is cleared before the destroy, so a stale handle is never left
/// in the editor state even if the entity turns out to be gone already. Does
/// not cascade: children pointing at the destroyed entity through
/// HierarchyComponent keep their handle and simply resolve as dead.
ZHLN_API void DestroySelected(ZHLN::ECS::Registry& reg, EditorState& state) noexcept;

/// One component the editor knows how to add to an entity, remove from it, and
/// display. Function pointers rather than a std::function, so the table is a
/// constant and a caller needs no allocation to walk it.
struct ComponentKind {
    std::string_view name;
    bool (*has)(const ZHLN::ECS::Registry& reg, ZHLN::Entity entity);
    void (*add)(ZHLN::ECS::Registry& reg, ZHLN::Entity entity);
    void (*remove)(ZHLN::ECS::Registry& reg, ZHLN::Entity entity);
};

/// The components the editor can add, in the order the inspector lists them.
///
/// This is not every component in ZHLN::Components, and the reason is safety
/// rather than effort: a default-constructed MeshComponent names a GPU mesh that
/// does not exist, a PhysicsComponent names a body the physics world never
/// created, and an AnimatorComponent holds a raw pointer to a ModelPrefab.
/// Conjuring those from "Add Component" would put a dangling handle in the
/// registry on the click.
///
/// The line drawn is therefore: the editor may add exactly what the inspector
/// can also show and edit, so "Add" never produces something the editor cannot
/// immediately display. DrawInspectorPanel draws one section per entry here --
/// the two lists are the same set, and a component added to one belongs in the
/// other.
[[nodiscard]] ZHLN_API auto ComponentKinds() noexcept -> std::span<const ComponentKind>;

/// Draws the scene hierarchy: one selectable row per named entity that is not
/// part of the editor's own subtree. Clicking a row writes
/// `state.selectedEntity`.
///
/// The header row carries the two entity-level operations: New Entity
/// (CreateEntity, which also selects the result) and Delete (DestroySelected).
///
/// NOTE: `reg` is separate from `gui` because the new Clay-based GUI::Context
/// is stateless w.r.t. the ECS — the editor reads the registry directly.
ZHLN_API void DrawHierarchyPanel(
    ZHLN::GUI::Context&    gui,
    ZHLN::ECS::Registry&   reg,
    EditorState&           state,
    std::string_view       id = "Hierarchy"
);

/// Draws the inspector for `state.selectedEntity`: a labelled header plus one
/// collapsing section per editable component present on the entity, and a
/// dropdown offering the components it does not have yet.
/// With no live selection the panel shows a "No selection" placeholder.
///
/// Each section ends with a Remove button, so a component is deleted from the
/// entity that is showing it rather than from a list kept elsewhere.
///
/// NOTE: `reg` is separate from `gui` for the same reason as above.
ZHLN_API void DrawInspectorPanel(
    ZHLN::GUI::Context&    gui,
    ZHLN::ECS::Registry&   reg,
    EditorState&           state,
    std::string_view       id = "Inspector"
);

} // namespace ZHLN::Editor
