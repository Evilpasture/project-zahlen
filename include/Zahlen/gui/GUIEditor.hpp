// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// include/Zahlen/gui/GUIEditor.hpp
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
#include <Zahlen/Math3D.hpp>
#include <span>
#include <string_view>

namespace ZHLN::ECS {
class Registry;
}
namespace ZHLN {
struct Camera;
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

    // --- Blender-style modal transform (UpdateTransformMode) -----------------
    // G/R/S enter Move/Rotate/Scale on the selection; X/Y/Z constrain the axis;
    // LMB or Enter confirms; Esc or RMB cancels and restores the start transform.

    /// Which modal transform, if any, is running.
    enum class TransformMode : uint8_t { None = 0, Move, Rotate, Scale };
    TransformMode transformMode = TransformMode::None;

    /// Axis constraint for the running mode. None = free (view plane / uniform).
    enum class TransformAxis : uint8_t { None = 0, X, Y, Z };
    TransformAxis transformAxis = TransformAxis::None;

    /// Entity the running mode acts on. Captured at mode entry, so a selection
    /// change mid-mode does not redirect the manipulation.
    ZHLN::Entity transformEntity = ZHLN::Entity::Null();

    // Captures taken at mode entry; cancel writes them back.
    JPH::Vec3 transformStartPosition = JPH::Vec3::sZero();
    JPH::Quat transformStartRotation = JPH::Quat::sIdentity();
    JPH::Vec3 transformStartScale    = JPH::Vec3::sReplicate(1.0f);

    // Anchors for the running mode: Move intersects the mouse ray with a plane
    // through the start position (normal = camera forward at entry); Rotate and
    // Scale measure screen-space angle / distance around the projected object.
    JPH::Vec3 transformPlaneNormal = JPH::Vec3::sAxisY();
    JPH::Vec3 transformAnchor      = JPH::Vec3::sZero();
    float     transformStartAngle  = 0.0f;
    float     transformStartDist   = 1.0f;

    /// Previous-frame raw input levels, for press-edge detection. Owned by
    /// UpdateTransformMode; hosts must not read or write it.
    uint16_t transformPrevInput = 0;

    /// Spawn requested from the hierarchy's Add Shape dropdown, as an index
    /// into SpawnShapeNames(); -1 means nothing requested. The host owns the
    /// Engine a spawn needs, so it consumes this after drawing the panels and
    /// resets it to -1.
    int requestedSpawn = -1;
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

/// The basic shapes the hierarchy's Add Shape dropdown offers, in the order
/// the dropdown lists them. The host maps a `requestedSpawn` index to the
/// matching CreativeWorksFactory spawner.
[[nodiscard]] ZHLN_API auto SpawnShapeNames() noexcept -> std::span<const std::string_view>;

/// Runs the Blender-style modal transform for this frame: enters a mode on
/// G/R/S press edges (plain S only -- a Ctrl+S save chord never starts Scale),
/// manipulates the captured entity from the mouse ray, and confirms or cancels
/// it. Reads raw input levels from the registry's InputStateComponent
/// singleton and writes the entity's TransformComponent live, so the world
/// preview follows the pointer before confirmation.
///
/// Call once per frame BEFORE camera control and viewport picking: cancel and
/// confirm then win over the global Escape / click-to-select bindings on the
/// same frame, and a mode never leaks keys into the fly camera.
/// @p uiOwnsInput is true while a text field or dropdown owns the keyboard
/// (the host's `uiCapturesKeyboard`); the mode then neither starts nor acts,
/// so typing "g" into a name box never grabs the object.
/// Sub-rectangle of the window/framebuffer the 3D scene occupies, in pixels
/// with a top-left origin -- the same rectangle RenderContext::SetViewport is
/// given. Mouse unprojection and world-to-screen here are rectangle-relative:
/// pass {0, 0, w, h} for a full-frame viewport and the behaviour is identical
/// to a plain window-sized viewport.
struct SceneViewport {
    uint32_t x      = 0;
    uint32_t y      = 0;
    uint32_t width  = 0;
    uint32_t height = 0;
};

ZHLN_API void UpdateTransformMode(
    ZHLN::ECS::Registry& reg,
    EditorState&         state,
    const Camera&        camera,
    const SceneViewport& viewport,
    bool                 uiOwnsInput
) noexcept;

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
