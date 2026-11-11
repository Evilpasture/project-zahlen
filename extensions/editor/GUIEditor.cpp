// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// extensions/editor/GUIEditor.cpp
//
// Native editor panels (Hierarchy + Inspector). See extensions/editor/GUIEditor.hpp
// for the design notes; this file is where the reflection iteration lives, so
// that the transpiler fallback (scripts/transpile_reflection.py, which rewrites
// reflection calls by translation-unit source offset) sees and flattens it.

#include <Zahlen/Engine/Internal/EngineAccess.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/GUI/GUI.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <editor/GUIEditor.hpp>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::Editor {

namespace {

namespace GUI = ZHLN::GUI;
using Comp    = ZHLN::Components;

// ============================================================================
// The component table
// ============================================================================
//
// One entry per component the editor can add, and the entry is built from the
// type rather than written out: the name comes from reflection, and the three
// operations are the registry calls instantiated for that type. So the table
// cannot name a component wrong, and it cannot offer one the registry could not
// store.
//
// What it deliberately is NOT is an enumeration of ZHLN::Components. See
// ComponentKinds() in the header for why: a default-constructed component that
// owns a GPU mesh, a physics body or a raw prefab pointer is a dangling handle
// the moment it exists. The set here is the set DrawInspectorPanel draws a
// section for, so adding one always produces something the editor can show.

template <typename C>
consteval auto MakeComponentKind() -> ComponentKind {
    // The two properties the registry relies on: Add() default-constructs, and
    // a trivially-copyable component takes the memcpy fast path in SparseSet.
    static_assert(std::is_default_constructible_v<C>, "an addable component must be default-constructible");
    static_assert(std::is_trivially_copyable_v<C>, "an addable component must be trivially copyable");

    return ComponentKind {// The same spelling the registry registers the component under, so the
                          // dropdown and Registry::DebugDumpEntity cannot disagree about a name.
                          .name   = ZHLN::ECS::BoxedName<C>(),
                          .has    = [](const ZHLN::ECS::Registry& reg, ZHLN::Entity entity) -> bool { return reg.Get<C>(entity).has_value(); },
                          .add    = [](ZHLN::ECS::Registry& reg, ZHLN::Entity entity) -> void { reg.Add<C>(entity, C {}); },
                          .remove = [](ZHLN::ECS::Registry& reg, ZHLN::Entity entity) -> void { reg.Remove<C>(entity); }
    };
}

template <typename... Cs>
consteval auto MakeComponentKinds() -> std::array<ComponentKind, sizeof...(Cs)> {
    return {MakeComponentKind<Cs>()...};
}

// The components the editor can add -- the same set, in the same order, as the
// sections DrawInspectorPanel draws below. The two lists cannot be collapsed
// into one: a section body calls Reflect::ForEachFieldWithName on a concrete
// static type, which the transpiler fallback requires (see the invariant note
// in DrawInspectorPanel), and a table-driven version would make that type
// dependent and flatten to zero rows.
constexpr auto kComponentKinds = MakeComponentKinds<Comp::NameComponent, Comp::TransformComponent, Comp::PBRComponent, Comp::LightComponent>();

// The editor and the edited scene share one registry, so the hierarchy
// has to know which subtree is chrome. Walk HierarchyComponent upward
// from `e`; anything that reaches `editorRoot` is the editor's own.
// Bounded so a corrupted parent cycle cannot hang the frame.
[[nodiscard]] auto IsEditorEntity(ZHLN::Entity e, const ZHLN::ECS::Registry& reg, ZHLN::Entity editorRoot) -> bool {
    if (editorRoot == ZHLN::Entity::Null()) {
        return false;
    }
    ZHLN::Entity cur = e;
    for (int guard = 0; guard < 128; ++guard) {
        if (cur == editorRoot) {
            return true;
        }
        const auto hierarchy = reg.Get<Comp::HierarchyComponent>(cur);
        if (!hierarchy || hierarchy->parent == ZHLN::Entity::Null()) {
            return false;
        }
        cur = hierarchy->parent;
    }
    return false;
}

// The generic row sink: one row per reflected (name, field) pair. The
// field name is the label; the row id is section-scoped so two components
// with a `width` field cannot collide in the child cache.
//
// Missing widget types (Reference) are rendered as read-only text until those
// widgets are implemented in the Clay API.
[[nodiscard]] auto MakeRowSink(GUI::Frame& gui, std::string_view sectionId) {
    return [&gui, sectionId](std::string_view name, auto& field) -> void {
        using FT = std::remove_cvref_t<decltype(field)>;

        // Padding/reserved members never get a row.
        if (name.starts_with('_')) {
            return;
        }

        std::array<char, 128>                   rowIdBuf {};
        [[maybe_unused]] const std::string_view rowId = ZHLN::FormatTo(rowIdBuf, "{}_{}", sectionId, name);

        if constexpr (std::is_same_v<FT, float>) {
            gui.Slider(rowId, field, -10000.0f, 10000.0f);
        } else if constexpr (std::is_same_v<FT, bool>) {
            bool copy = field;
            if (gui.Checkbox(name, copy)) {
                field = copy;
            }
        } else if constexpr (std::is_same_v<FT, int32_t>) {
            float v = static_cast<float>(field);
            gui.Slider(rowId, v, -100000.0f, 100000.0f);
            field = static_cast<int32_t>(v);
        } else if constexpr (std::is_same_v<FT, uint32_t>) {
            float v = static_cast<float>(field);
            gui.Slider(rowId, v, 0.0f, 1000000.0f);
            field = static_cast<uint32_t>(v);
        } else if constexpr (std::is_enum_v<FT>) {
            constexpr auto names = ZHLN::Reflect::EnumNames<FT>();
            if constexpr (names.size() > 0) {
                // Dropdown clamps the index itself, so a value the enum no
                // longer has (a scene authored against an older revision) lands
                // on a real enumerator instead of indexing past the names.
                int idx = static_cast<int>(field);
                if (gui.Dropdown(rowId, std::span<const std::string_view>(names), idx)) {
                    field = static_cast<FT>(idx);
                }
            }
        } else if constexpr (std::is_same_v<FT, ZHLN::Entity>) {
            // Stub: show packed handle as text
            std::array<char, 64> buf {};
            auto                 sv = ZHLN::FormatTo(buf, "{}: Entity({})", name, field.Pack());
            gui.Text(sv, 12.0f, {0.7f, 0.7f, 0.7f, 1.0f});
        } else if constexpr (std::is_same_v<FT, TextureID>) {
            std::array<char, 64> buf {};
            auto                 sv = ZHLN::FormatTo(buf, "{}: Texture({})", name, static_cast<uint64_t>(field));
            gui.Text(sv, 12.0f, {0.7f, 0.7f, 0.7f, 1.0f});
        } else if constexpr (std::is_same_v<FT, FVec4>) {
            float v[4] = {field.x, field.y, field.z, field.w};
            for (int axis = 0; axis < 4; ++axis) {
                std::array<char, 136> axisIdBuf {};
                auto                  axisLabel = ZHLN::FormatTo(axisIdBuf, "{} {}", name, "XYZW"[axis]);
                gui.Slider(axisLabel, v[axis], -10000.0f, 10000.0f);
            }
            field.x = v[0];
            field.y = v[1];
            field.z = v[2];
            field.w = v[3];
        } else if constexpr (std::is_same_v<FT, FVec3>) {
            float v[3] = {field.x, field.y, field.z};
            for (int axis = 0; axis < 3; ++axis) {
                std::array<char, 136> axisIdBuf {};
                auto                  axisLabel = ZHLN::FormatTo(axisIdBuf, "{} {}", name, "XYZ"[axis]);
                gui.Slider(axisLabel, v[axis], -10000.0f, 10000.0f);
            }
            field = FVec3(v[0], v[1], v[2]);
        } else if constexpr (std::is_same_v<FT, JPH::Quat>) {
            const auto euler = field.GetEulerAngles() * (180.0f / JPH::JPH_PI);
            float           deg[3]  = {euler.GetX(), euler.GetY(), euler.GetZ()};
            bool            changed = false;
            for (int axis = 0; axis < 3; ++axis) {
                std::array<char, 96> axisLabelBuf {};
                auto                 axisLabel = ZHLN::FormatTo(axisLabelBuf, "{} Rot {}", name, "XYZ"[axis]);
                const float          prev      = deg[axis];
                gui.Slider(axisLabel, deg[axis], -360.0f, 360.0f);
                if (deg[axis] != prev)
                    changed = true;
            }
            if (changed) {
                field = ZHLN::Math::EulerDegreesToQuat({deg[0], deg[1], deg[2]});
            }
        } else if constexpr (std::is_same_v<FT, ZHLN::String256> || std::is_same_v<FT, ZHLN::String64>) {
            // rowId rather than name: two components in the same section can
            // have a field of the same name, and the widget table is keyed by
            // the label's hash, so a collision would share one caret between
            // them. Same convention as Slider above.
            gui.TextInput(rowId, field);
        }
        // Everything else (char padding, nested structs) intentionally
        // gets no row in this version.
    };
}

} // namespace

auto ComponentKinds() noexcept -> std::span<const ComponentKind> {
    return kComponentKinds;
}

auto CreateEntity(ZHLN::ECS::Registry& reg, std::string_view name) -> ZHLN::Entity {
    const ZHLN::Entity entity = reg.Create();

    std::array<char, 64>   nameBuf {};
    const std::string_view label = name.empty() ? ZHLN::FormatTo(nameBuf, "Entity {}", entity.index) : name;

    // The world transform is written here rather than left for
    // TransformSystem::ResolveTransforms to add on first tick: an entity the
    // editor just made should be whole immediately, so inspecting or serialising
    // it on the same frame does not depend on a system having run.
    const auto local = Mat4x4::Identity();

    reg.Add(entity, Comp::NameComponent {.name = ZHLN::String64 {label}});
    reg.Add(entity, Comp::TransformComponent {});
    reg.Add(entity, Comp::WorldTransformComponent {.world = local, .previous = local});

    return entity;
}

void DestroySelected(ZHLN::Engine& engine, EditorState& state) noexcept {
    auto&              reg    = ZHLN::EngineAccess::RegistryOf(engine);
    const ZHLN::Entity victim = state.selectedEntity;

    // Cleared first: if the handle turns out to be stale the editor must not be
    // left holding it, and the inspector's own IsAlive check reads this.
    state.selectedEntity = ZHLN::Entity::Null();

    if (victim == ZHLN::Entity::Null() || !reg.IsAlive(victim)) {
        return;
    }
    ZHLN::DespawnEntity(engine, victim);
}

// ============================================================================
// Modal transform (Blender-style G/R/S)
// ============================================================================
//
// The mode is a small state machine over raw input LEVELS, edge-detected
// against EditorState::transformPrevInput: InputStateComponent carries key
// levels, not presses, exactly like the host's Ctrl+S handling.
//
// All manipulation math is camera-ray based (the same unproject
// CastPickingRay uses), so the whole mode is testable headless: a registry,
// a Camera and a mouse position are all it reads.

namespace {

auto ReadTransformInput(ZHLN::Optional<const Comp::InputStateComponent&> input) noexcept -> TransformInput {
    if (!input) {
        return TransformInput::None;
    }
    TransformInput bits = TransformInput::None;
    auto           key  = [&](ZHLN::KeyCode k) -> bool { return input->IsKeyDownRaw(static_cast<uint8_t>(k)); };
    if (key(ZHLN::KeyCode::G))
        bits |= TransformInput::G;
    if (key(ZHLN::KeyCode::R))
        bits |= TransformInput::R;
    if (key(ZHLN::KeyCode::S))
        bits |= TransformInput::S;
    if (key(ZHLN::KeyCode::X))
        bits |= TransformInput::X;
    if (key(ZHLN::KeyCode::Y))
        bits |= TransformInput::Y;
    if (key(ZHLN::KeyCode::Z))
        bits |= TransformInput::Z;
    if (key(ZHLN::KeyCode::Enter))
        bits |= TransformInput::Enter;
    if (key(ZHLN::KeyCode::Escape))
        bits |= TransformInput::Esc;
    if (key(ZHLN::KeyCode::LControl) || key(ZHLN::KeyCode::RControl))
        bits |= TransformInput::Ctrl;
    if (input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton)))
        bits |= TransformInput::LMB;
    if (input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton)))
        bits |= TransformInput::RMB;
    return bits;
}

auto CameraForward(const Camera& camera) noexcept -> FVec3 {
    const float yaw   = JPH::DegreesToRadians(camera.yaw);
    const float pitch = JPH::DegreesToRadians(camera.pitch);
    return FVec3(JPH::Cos(yaw) * JPH::Cos(pitch), JPH::Sin(pitch), JPH::Sin(yaw) * JPH::Cos(pitch)).Normalized();
}

// Unprojects the mouse exactly like the host's picking ray: same NDC
// convention, same inverse view-projection.
auto MouseRay(const Camera& camera, float mx, float my, const SceneViewport& vp, FVec3& origin, FVec3& dir) noexcept -> bool {
    const float w = static_cast<float>(vp.width);
    const float h = static_cast<float>(vp.height);
    if (w <= 0.0f || h <= 0.0f) {
        return false;
    }
    // Vulkan Y-down clip space with the viewport's row 0 at the top: the top
    // pixel of the scene rectangle is NDC y == -1. The scene is rasterized by
    // a fixed-function viewport into [vp.x, vp.y, vp.width, vp.height], so the
    // window-space mouse must be made rectangle-relative first -- exactly the
    // transform the viewport does in reverse.
    const float ndcX   = (2.0f * (mx - static_cast<float>(vp.x))) / w - 1.0f;
    const float ndcY   = (2.0f * (my - static_cast<float>(vp.y))) / h - 1.0f;
    const float aspect = w / h;

    const auto invVP = Inverse(camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix());
    const FVec4  nearW = invVP * FVec4(ndcX, ndcY, 0.0f, 1.0f);
    const FVec4  farW  = invVP * FVec4(ndcX, ndcY, 1.0f, 1.0f);
    if (nearW.w <= 0.0f || farW.w <= 0.0f) {
        return false;
    }
    origin               = FVec3(nearW.x / nearW.w, nearW.y / nearW.w, nearW.z / nearW.w);
    const FVec3 farP = FVec3(farW.x / farW.w, farW.y / farW.w, farW.z / farW.w);
    dir                  = (farP - origin).Normalized();
    return true;
}

auto RayPlane(const FVec3& origin, const FVec3& dir, const FVec3& normal, const FVec3& planePoint, FVec3& hit) noexcept -> bool {
    const float d = Dot(dir, normal);
    if (std::fabs(d) < 1e-6f) {
        return false;
    }
    const float t = Dot(planePoint - origin, normal) / d;
    hit           = origin + dir * t;
    return true;
}

auto WorldToScreen(const Camera& camera, const SceneViewport& r, const FVec3& world, float& sx, float& sy) noexcept -> bool {
    const float w = static_cast<float>(r.width);
    const float h = static_cast<float>(r.height);
    if (w <= 0.0f || h <= 0.0f) {
        return false;
    }
    const float      aspect = w / h;
    const auto vp = camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix();
    const FVec4  clip   = vp * FVec4(world.x, world.y, world.z, 1.0f);
    if (clip.w <= 0.0f) {
        return false;
    }
    // NDC -> window pixels inside the scene rectangle (see MouseRay).
    sx = (clip.x / clip.w + 1.0f) * 0.5f * w + static_cast<float>(r.x);
    sy = (clip.y / clip.w + 1.0f) * 0.5f * h + static_cast<float>(r.y);
    return true;
}

auto AxisVector(EditorState::TransformAxis axis) noexcept -> FVec3 {
    switch (axis) {
        case EditorState::TransformAxis::X:
            return FVec3::UnitX();
        case EditorState::TransformAxis::Y:
            return FVec3::UnitY();
        case EditorState::TransformAxis::Z:
            return FVec3::UnitZ();
        case EditorState::TransformAxis::None:
            break;
    }
    return FVec3::Zero();
}

} // namespace

constexpr std::string_view kSpawnShapeNames[] = {"Cube", "Plane", "Sphere", "Cylinder", "Cone"};

auto SpawnShapeNames() noexcept -> std::span<const std::string_view> {
    return kSpawnShapeNames;
}

void UpdateTransformMode(ZHLN::ECS::Registry& reg, EditorState& state, const Camera& camera, const SceneViewport& viewport, bool uiOwnsInput) noexcept {
    const auto input = reg.GetSingleton<Comp::InputStateComponent>();
    if (uiOwnsInput) {
        // Keep the edge detector honest across the capture window, so a key
        // held when the field lost focus is not seen as a fresh press.
        state.transformPrevInput = ReadTransformInput(input);
        return;
    }
    const TransformInput level   = ReadTransformInput(input);
    const TransformInput pressed = level & ~state.transformPrevInput;
    const float          mx      = input ? input->mouseX : -1.0f;
    const float          my      = input ? input->mouseY : -1.0f;

    if (state.transformMode == EditorState::TransformMode::None) {
        // Enter a mode only on a live selection. Plain S starts Scale, but a
        // Ctrl+S is the save chord and must never grab the object.
        EditorState::TransformMode mode = EditorState::TransformMode::None;
        if (input && state.selectedEntity != ZHLN::Entity::Null() && reg.IsAlive(state.selectedEntity)) {
            if ((pressed & TransformInput::G) != TransformInput::None) {
                mode = EditorState::TransformMode::Move;
            } else if ((pressed & TransformInput::R) != TransformInput::None) {
                mode = EditorState::TransformMode::Rotate;
            } else if ((pressed & TransformInput::S) != TransformInput::None && (level & TransformInput::Ctrl) == TransformInput::None) {
                mode = EditorState::TransformMode::Scale;
            }
        }
        if (mode != EditorState::TransformMode::None) {
            const auto t = reg.Get<Comp::TransformComponent>(state.selectedEntity);
            if (t) {
                state.transformMode          = mode;
                state.transformAxis          = EditorState::TransformAxis::None;
                state.transformEntity        = state.selectedEntity;
                state.transformStartPosition = t->position;
                state.transformStartRotation = JPH::Quat(t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w);
                state.transformStartScale    = t->scale;

                state.transformPlaneNormal = CameraForward(camera);
                FVec3 origin {}, dir {};
                if (!MouseRay(camera, mx, my, viewport, origin, dir) ||
                    !RayPlane(origin, dir, state.transformPlaneNormal, t->position, state.transformAnchor)) {
                    state.transformAnchor = t->position;
                }

                float ox = 0.0f, oy = 0.0f;
                if (!WorldToScreen(camera, viewport, t->position, ox, oy)) {
                    ox = mx;
                    oy = my;
                }
                state.transformStartAngle = std::atan2(my - oy, mx - ox);
                state.transformStartDist  = std::max(std::hypot(mx - ox, my - oy), 1e-3f);
            }
        }
    } else {
        const ZHLN::Entity e = state.transformEntity;
        if (!reg.IsAlive(e)) {
            state.transformMode = EditorState::TransformMode::None;
        } else if ((pressed & (TransformInput::Esc | TransformInput::RMB)) != TransformInput::None) {
            // Cancel: write the captures back, drop the mode.
            reg.Patch<Comp::TransformComponent>(e, [&](Comp::TransformComponent& t) -> void {
                t.position = state.transformStartPosition;
                t.rotation = {state.transformStartRotation.GetX(), state.transformStartRotation.GetY(), state.transformStartRotation.GetZ(), state.transformStartRotation.GetW()};
                t.scale    = state.transformStartScale;
            });
            state.transformMode = EditorState::TransformMode::None;
        } else if ((pressed & (TransformInput::Enter | TransformInput::LMB)) != TransformInput::None) {
            // Confirm: the live transform stands.
            state.transformMode = EditorState::TransformMode::None;
        } else {
            if ((pressed & TransformInput::X) != TransformInput::None) {
                state.transformAxis = (state.transformAxis == EditorState::TransformAxis::X) ? EditorState::TransformAxis::None : EditorState::TransformAxis::X;
            } else if ((pressed & TransformInput::Y) != TransformInput::None) {
                state.transformAxis = (state.transformAxis == EditorState::TransformAxis::Y) ? EditorState::TransformAxis::None : EditorState::TransformAxis::Y;
            } else if ((pressed & TransformInput::Z) != TransformInput::None) {
                state.transformAxis = (state.transformAxis == EditorState::TransformAxis::Z) ? EditorState::TransformAxis::None : EditorState::TransformAxis::Z;
            }

            switch (state.transformMode) {
                case EditorState::TransformMode::Move: {
                    FVec3 origin {}, dir {}, hit {};
                    if (MouseRay(camera, mx, my, viewport, origin, dir) &&
                        RayPlane(origin, dir, state.transformPlaneNormal, state.transformStartPosition, hit)) {
                        FVec3 delta = hit - state.transformAnchor;
                        if (state.transformAxis != EditorState::TransformAxis::None) {
                            const FVec3 axis = AxisVector(state.transformAxis);
                            delta                = axis * Dot(delta, axis);
                        }
                        const FVec3 newPos = state.transformStartPosition + delta;
                        reg.Patch<Comp::TransformComponent>(e, [&newPos](Comp::TransformComponent& t) -> void { t.position = newPos; });
                    }
                    break;
                }
                case EditorState::TransformMode::Rotate: {
                    float ox = 0.0f, oy = 0.0f;
                    if (WorldToScreen(camera, viewport, state.transformStartPosition, ox, oy)) {
                        // Screen pixels are y-down; the viewer thinks y-up, so
                        // the seen angle negates the pixel dy. Dragging counter-
                        // clockwise as seen must turn counter-clockwise: the
                        // free axis therefore points AT the viewer (the negated
                        // camera forward), making a positive seen delta a
                        // positive right-handed turn about it.
                        const float     angle  = std::atan2(oy - my, mx - ox);
                        const FVec3 axis   = (state.transformAxis != EditorState::TransformAxis::None) ? AxisVector(state.transformAxis) :
                                                                                                             state.transformPlaneNormal * -1.0f;
                        const JPH::Quat turn   = JPH::Quat::sRotation(JPH::Vec3(axis.x, axis.y, axis.z), angle - state.transformStartAngle);
                        const JPH::Quat newRot = turn * state.transformStartRotation;
                        reg.Patch<Comp::TransformComponent>(e, [&newRot](Comp::TransformComponent& t) -> void { t.rotation = {newRot.GetX(), newRot.GetY(), newRot.GetZ(), newRot.GetW()}; });
                    }
                    break;
                }
                case EditorState::TransformMode::Scale: {
                    float ox = 0.0f, oy = 0.0f;
                    if (WorldToScreen(camera, viewport, state.transformStartPosition, ox, oy)) {
                        // Screen-space distance ratio, like Blender's uniform
                        // scale: pull away from the object and it grows.
                        const float     dist   = std::max(std::hypot(mx - ox, my - oy), 1e-3f);
                        const float     factor = std::max(dist / state.transformStartDist, 1e-3f);
                        const FVec3 start  = state.transformStartScale;
                        FVec3       next   = start * factor;
                        if (state.transformAxis != EditorState::TransformAxis::None) {
                            next               = start;
                            const float scaled = (state.transformAxis == EditorState::TransformAxis::X) ? start.x * factor :
                                                 (state.transformAxis == EditorState::TransformAxis::Y) ? start.y * factor :
                                                                                                          start.z * factor;
                            if (state.transformAxis == EditorState::TransformAxis::X) {
                                next = FVec3(scaled, start.y, start.z);
                            } else if (state.transformAxis == EditorState::TransformAxis::Y) {
                                next = FVec3(start.x, scaled, start.z);
                            } else {
                                next = FVec3(start.x, start.y, scaled);
                            }
                        }
                        // A flipped or zero scale would turn the mesh inside
                        // out or collapse it; the mouse can get arbitrarily
                        // close to the projected center.
                        next = FVec3(std::max(next.x, 1e-4f), std::max(next.y, 1e-4f), std::max(next.z, 1e-4f));
                        reg.Patch<Comp::TransformComponent>(e, [&next](Comp::TransformComponent& t) -> void { t.scale = next; });
                    }
                    break;
                }
                case EditorState::TransformMode::None:
                    break;
            }
        }
    }

    state.transformPrevInput = level;
}

void DrawHierarchyPanel(GUI::Frame& gui, ZHLN::Engine& engine, EditorState& state, std::string_view id) {
    auto& reg = ZHLN::EngineAccess::RegistryOf(engine);
    struct Row {
        ZHLN::Entity entity;
        uint32_t     depth;
        uint32_t     order;
    };

    // Snapshot + sort: GetEntitiesWith returns dense-array order, which
    // reshuffles on every swap-remove destroy. Indent from HierarchyComponent
    // parent walks; sibling order is the entity index so the list is
    // deterministic without a private UI layout stamp.
    std::vector<Row> rows;
    for (const ZHLN::Entity e: reg.GetEntitiesWith<Comp::NameComponent>()) {
        if (IsEditorEntity(e, reg, state.editorRoot)) {
            continue;
        }
        uint32_t     depth = 0;
        ZHLN::Entity cur   = e;
        for (int guard = 0; guard < 128; ++guard) {
            const auto hierarchy = reg.Get<Comp::HierarchyComponent>(cur);
            if (!hierarchy || hierarchy->parent == ZHLN::Entity::Null()) {
                break;
            }
            ++depth;
            cur = hierarchy->parent;
        }
        rows.push_back(Row {e, depth, e.index});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) -> bool {
        if (a.depth != b.depth)
            return a.depth < b.depth;
        return a.order < b.order;
    });

    // Render the hierarchy as a scrollable column
    gui.BeginColumn(4.0f);
    gui.Text(id, 14.0f, {0.6f, 0.7f, 0.8f, 1.0f});

    // Entity-level operations. New Entity also selects its result, so the
    // inspector opens on it immediately instead of on whatever was picked last.
    gui.BeginRow(4.0f, 0.0f);
    if (gui.Button("New Entity", {0.16f, 0.30f, 0.20f, 0.95f})) {
        state.selectedEntity = CreateEntity(reg);
    }
    if (state.selectedEntity != ZHLN::Entity::Null() && reg.IsAlive(state.selectedEntity) && gui.Button("Delete", {0.42f, 0.16f, 0.16f, 0.95f})) {
        DestroySelected(engine, state);
    }
    gui.EndRow();

    // Add Shape requests a spawn rather than spawning here: the factory needs
    // an Engine (GPU mesh, material), which the panel deliberately does not
    // see. The host consumes requestedSpawn after drawing and resets it.
    std::array<std::string_view, 8> spawnOptions {};
    spawnOptions[0]   = "Add Shape...";
    size_t spawnCount = 1;
    for (const std::string_view name: kSpawnShapeNames) {
        spawnOptions[spawnCount++] = name;
    }
    int spawnPick = 0;
    if (gui.Dropdown("Add Shape", std::span<const std::string_view>(spawnOptions.data(), spawnCount), spawnPick) && spawnPick > 0) {
        state.requestedSpawn = spawnPick - 1;
    }

    // The running modal transform gets a visible affordance, so the keybinds
    // are not invisible state.
    if (state.transformMode != EditorState::TransformMode::None) {
        std::array<char, 96>   modeBuf {};
        const std::string_view modeName = state.transformMode == EditorState::TransformMode::Move   ? "Move" :
                                          state.transformMode == EditorState::TransformMode::Rotate ? "Rotate" :
                                                                                                      "Scale";
        const std::string_view axisName = state.transformAxis == EditorState::TransformAxis::X ? "X" :
                                          state.transformAxis == EditorState::TransformAxis::Y ? "Y" :
                                          state.transformAxis == EditorState::TransformAxis::Z ? "Z" :
                                                                                                 "free";
        gui.Text(ZHLN::FormatTo(modeBuf, "{} [{}] - LMB/Enter confirm, Esc/RMB cancel", modeName, axisName), 12.0f, {0.9f, 0.8f, 0.4f, 1.0f});
    }

    for (const Row& row: rows) {
        std::array<char, 96> fallbackBuf {};
        std::string_view     label;
        if (const auto name = reg.Get<Comp::NameComponent>(row.entity)) {
            label = std::string_view(name->name);
        } else {
            label = ZHLN::FormatTo(fallbackBuf, "Entity {}", row.entity.index);
        }

        bool      isSelected = (state.selectedEntity == row.entity);
        FVec4 color      = isSelected ? FVec4(0.20f, 0.35f, 0.55f, 0.9f) : FVec4(0.10f, 0.12f, 0.16f, 0.7f);

        // Indent by depth
        if (row.depth > 0) {
            gui.BeginRow(0.0f, static_cast<float>(row.depth) * 8.0f);
        }

        if (gui.Button(label, color)) {
            state.selectedEntity = row.entity;
        }

        if (row.depth > 0) {
            gui.EndRow();
        }
    }

    gui.EndColumn();
}

void DrawInspectorPanel(GUI::Frame& gui, ZHLN::ECS::Registry& reg, EditorState& state, std::string_view id) {
    gui.BeginColumn(4.0f);
    gui.Text(id, 14.0f, {0.6f, 0.7f, 0.8f, 1.0f});

    const ZHLN::Entity sel = state.selectedEntity;
    if (sel == ZHLN::Entity::Null() || !reg.IsAlive(sel)) {
        gui.Text("No selection", 12.0f, {0.5f, 0.5f, 0.5f, 1.0f});
        gui.EndColumn();
        return;
    }

    std::array<char, 64> headerBuf {};
    gui.Text(std::string_view(ZHLN::FormatTo(headerBuf, "Entity {}", sel.index)), 13.0f);

    // One collapsing section per editable component.
    //
    // Invariant: The call's object expression must have a CONCRETE static
    // type. The transpiler fallback extracts the field list from the object's
    // type, so a call inside a template with a dependent T would flatten to
    // zero rows.
    const auto section = [&](std::string_view sectionId, std::string_view title, auto comp, auto&& reflect) -> void {
        if (!comp)
            return;
        using CompT = typename decltype(comp)::value_type;
        if (gui.BeginCollapsingHeader(title, true)) {
            CompT local = *comp;
            reflect(local, MakeRowSink(gui, sectionId));

            // Deleting the component from the section that is showing it, so the
            // thing you remove is the thing you can see. `comp` points into the
            // sparse set's dense array and Remove is a swap-remove, which leaves
            // it holding some other entity's data -- hence the return before the
            // patch below, which would otherwise write the stale copy back.
            std::array<char, 64> removeBuf {};
            if (gui.Button(ZHLN::FormatTo(removeBuf, "Remove {}", title), {0.40f, 0.16f, 0.16f, 0.90f})) {
                reg.Remove<CompT>(sel);
                gui.EndCollapsingHeader();
                return;
            }

            reg.Patch<CompT>(sel, [&local](CompT& dst) -> void { dst = local; });
            gui.EndCollapsingHeader();
        }
    };

    section("name", "Name", reg.Get<Comp::NameComponent>(sel), [](Comp::NameComponent& c, auto&& sink) -> void {
        ZHLN::Reflect::ForEachFieldWithName(c, sink);
    });
    section("transform", "Transform", reg.Get<Comp::TransformComponent>(sel), [](Comp::TransformComponent& c, auto&& sink) -> void {
        ZHLN::Reflect::ForEachFieldWithName(c, sink);
    });
    section("pbr", "PBR Material", reg.Get<Comp::PBRComponent>(sel), [](Comp::PBRComponent& c, auto&& sink) -> void {
        ZHLN::Reflect::ForEachFieldWithName(c, sink);
    });
    section("light", "Light", reg.Get<Comp::LightComponent>(sel), [](Comp::LightComponent& c, auto&& sink) -> void {
        ZHLN::Reflect::ForEachFieldWithName(c, sink);
    });
    // No post-process section: those values are the graphics device's, not a
    // property of the selected entity. They belong in a settings panel.

    // --- Add Component ------------------------------------------------------
    //
    // Offered after the sections, so the dropdown's option list is built once
    // every Remove above has already run and cannot be contradicted by it.
    std::array<std::string_view, 32>     options {};
    std::array<const ComponentKind*, 32> kinds {};
    size_t                               count = 0;

    // Index 0 is a prompt rather than a choice. Dropdown reports a change only
    // when `selected` moves, and this panel rebuilds `picked` at 0 every frame,
    // so a real component sitting at index 0 would swallow its own click.
    options[0] = "Add Component...";
    count      = 1;

    for (const ComponentKind& kind: kComponentKinds) {
        if (count >= options.size() || kind.has(reg, sel)) {
            continue;
        }
        options[count]   = kind.name;
        kinds[count - 1] = &kind;
        ++count;
    }

    if (count > 1) {
        int picked = 0;
        if (gui.Dropdown("Add Component", std::span<const std::string_view>(options.data(), count), picked) && picked > 0) {
            kinds[static_cast<size_t>(picked) - 1]->add(reg, sel);
        }
    } else {
        gui.Text("No components to add", 12.0f, {0.5f, 0.5f, 0.5f, 1.0f});
    }

    gui.EndColumn();
}

} // namespace ZHLN::Editor
