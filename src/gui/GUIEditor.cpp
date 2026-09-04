// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// src/gui/GUIEditor.cpp
//
// Native editor panels (Hierarchy + Inspector). See include/Zahlen/GUIEditor.hpp
// for the design notes; this file is where the reflection iteration lives, so
// that the transpiler fallback (tools/transpile_reflection.py, which rewrites
// reflection calls by translation-unit source offset) sees and flattens it.

#include <Zahlen/GUIEditor.hpp>

#include <Zahlen/Components.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <Zahlen/ecs/ECS.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::Editor {

namespace {

    namespace GUI = ZHLN::GUI;
    using Comp    = ZHLN::Components;
    using UIComp  = ZHLN::GUI::UIComponents;

    // The editor and the edited scene share one registry, so the hierarchy
    // has to know which subtree is chrome. Walk the UI parent chain upward
    // from `e`; anything that reaches `editorRoot` is the editor's own.
    // Bounded so a corrupted parent cycle cannot hang the frame.
    [[nodiscard]] auto IsEditorEntity(
        ZHLN::Entity             e,
        const ZHLN::ECS::Registry& reg,
        ZHLN::Entity             editorRoot
    ) -> bool {
        if (editorRoot == ZHLN::Entity::Null()) {
            return false;
        }
        ZHLN::Entity cur = e;
        for (int guard = 0; guard < 128; ++guard) {
            if (cur == editorRoot) {
                return true;
            }
            const auto* rect = reg.Get<UIComp::UIRectComponent>(cur);
            if (rect == nullptr || rect->parentEntity == ZHLN::Entity::Null()) {
                return false;
            }
            cur = rect->parentEntity;
        }
        return false;
    }

    // The candidate list for a reference field that points at an ENTITY.
    //
    // Only the registry knows what exists, but only the editor knows which of
    // those are scene content: everything under editorRoot is the editor's own
    // chrome, and offering the inspector's own rows as the target of a scene
    // component's `parent` handle is how a scene ends up parented to a
    // scrollbar. `Entity::Null()` is not offered — a field that means "no
    // target" is expressed by the picker's None entry, not by a second empty
    // row nobody can tell apart from the first.
    // Reachable only through an `if constexpr` branch of MakeRowSink's generic
    // lambda, which is instantiated per reflected field type. Builds without
    // P2996 reflection compile ForEachFieldWithName down to a no-op, so the
    // lambda is never instantiated and nothing references this — hence the
    // attribute. It is live in any build where reflection actually runs.
    [[maybe_unused]] [[nodiscard]] auto BuildEntityOptions(ZHLN::ECS::Registry& reg, ZHLN::Entity editorRoot)
        -> std::vector<GUI::ReferenceOption> {
        std::vector<GUI::ReferenceOption> out;
        out.reserve(64);
        for (const ZHLN::Entity e: reg.GetEntitiesWith<Comp::NameComponent>()) {
            if (e == ZHLN::Entity::Null() || IsEditorEntity(e, reg, editorRoot)) {
                continue;
            }
            const auto* name = reg.Get<Comp::NameComponent>(e);
            if (name == nullptr) {
                continue;
            }
            // The label borrows the component's own storage. It only has to
            // outlive this frame's Reference() call, which copies the strings
            // it keeps into the widget's own option array.
            out.push_back(GUI::ReferenceOption {.id = e.Pack(), .label = std::string_view(name->name)});
        }
        return out;
    }

    // The generic row sink: one row per reflected (name, field) pair. The
    // field name is the label; the row id is section-scoped so two components
    // with a `width` field cannot collide in the child cache.
    //
    // The ForEachFieldWithName call sites live in DrawInspectorPanel, one per
    // concrete component type — see the comment there for why the iteration
    // is not driven from this generic lambda.
    [[nodiscard]] auto MakeRowSink(GUI::Context& gui, ZHLN::Entity editorRoot, std::string_view sectionId) {
        return [&gui, editorRoot, sectionId](std::string_view name, auto& field) -> void {
            using FT = std::remove_cvref_t<decltype(field)>;

            // Padding/reserved members never get a row.
            if (name.starts_with('_')) {
                return;
            }

            std::array<char, 128> rowIdBuf {};
            const std::string_view rowId = ZHLN::FormatTo(rowIdBuf, "{}_{}", sectionId, name);

            if constexpr (std::is_same_v<FT, float>) {
                gui.Slider(rowId, name, field, -10000.0f, 10000.0f);
            } else if constexpr (std::is_same_v<FT, bool>) {
                gui.Checkbox(rowId, name, field);
            } else if constexpr (std::is_same_v<FT, int32_t>) {
                float v = static_cast<float>(field);
                gui.Slider(rowId, name, v, -100000.0f, 100000.0f, 1.0f);
                field = static_cast<int32_t>(v);
            } else if constexpr (std::is_same_v<FT, uint32_t>) {
                float v = static_cast<float>(field);
                gui.Slider(rowId, name, v, 0.0f, 1000000.0f, 1.0f);
                field = static_cast<uint32_t>(v);
            } else if constexpr (std::is_enum_v<FT>) {
                // Assumes contiguous enumerators starting at 0, which every UI
                // enum in Components.hpp satisfies. Flag-style enums degrade
                // to "pick one bit", which is still better than no row.
                constexpr auto names = ZHLN::Reflect::EnumNames<FT>();
                if constexpr (names.size() > 0) {
                    int idx = static_cast<int>(field);
                    if (idx < 0 || static_cast<size_t>(idx) >= names.size()) {
                        idx = 0;
                    }
                    gui.Dropdown(rowId, name, idx, std::span<const std::string_view> {names});
                    field = static_cast<FT>(idx);
                }
            } else if constexpr (std::is_same_v<FT, ZHLN::Entity>) {
                // Entity handles: a picker over the scene's named entities.
                // Entity::Null() packs to all-ones, which is not a useful
                // dropdown value, so it is mapped onto the picker's None entry
                // (0) in both directions.
                const ZHLN::Entity current = field;
                uint64_t           packed  = (current == ZHLN::Entity::Null()) ? 0 : current.Pack();
                gui.Reference(rowId, name, packed, BuildEntityOptions(gui.GetRegistry(), editorRoot));
                field = (packed == 0) ? ZHLN::Entity::Null() : ZHLN::Entity::Unpack(packed);
            } else if constexpr (std::is_same_v<FT, TextureHandle>) {
                // Deliberately ahead of the generic enum branch below.
                // TextureHandle IS an enum, so it used to fall through to
                // "render a dropdown of Reflect::EnumNames" — whose only
                // enumerator is Invalid. Under real reflection that produced a
                // one-option menu and rewrote every texture on the entity to
                // Invalid the moment the inspector touched it.
                //
                // There is no enumerable asset catalogue to list yet, so this
                // passes an empty option list: the row shows the handle (or
                // "None"), flags it as dangling when it names nothing known,
                // and lets it be cleared. It becomes a real asset browser the
                // moment something can enumerate mounted assets.
                uint64_t raw = static_cast<uint64_t>(field);
                gui.Reference(rowId, name, raw, std::span<const GUI::ReferenceOption> {});
                field = static_cast<TextureHandle>(raw);
            } else if constexpr (std::is_same_v<FT, JPH::Vec4>) {
                float v[4] = {field.GetX(), field.GetY(), field.GetZ(), field.GetW()};
                for (int axis = 0; axis < 4; ++axis) {
                    std::array<char, 136> axisIdBuf {};
                    const std::string_view axisId = ZHLN::FormatTo(axisIdBuf, "{}_{}", rowId, "xyzw"[axis]);
                    std::array<char, 96>  axisLabelBuf {};
                    const std::string_view axisLabel = ZHLN::FormatTo(axisLabelBuf, "{} {}", name, "XYZW"[axis]);
                    gui.Slider(axisId, axisLabel, v[axis], -10000.0f, 10000.0f);
                }
                field.SetX(v[0]);
                field.SetY(v[1]);
                field.SetZ(v[2]);
                field.SetW(v[3]);
            } else if constexpr (std::is_same_v<FT, JPH::Vec3>) {
                float v[3] = {field.GetX(), field.GetY(), field.GetZ()};
                for (int axis = 0; axis < 3; ++axis) {
                    std::array<char, 136> axisIdBuf {};
                    const std::string_view axisId = ZHLN::FormatTo(axisIdBuf, "{}_{}", rowId, "xyz"[axis]);
                    std::array<char, 96>  axisLabelBuf {};
                    const std::string_view axisLabel = ZHLN::FormatTo(axisLabelBuf, "{} {}", name, "XYZ"[axis]);
                    gui.Slider(axisId, axisLabel, v[axis], -10000.0f, 10000.0f);
                }
                field = JPH::Vec3(v[0], v[1], v[2]);
            } else if constexpr (std::is_same_v<FT, JPH::Quat>) {
                // Designers edit Euler degrees; the component stores a
                // quaternion. Only write back when a row actually changed —
                // an unconditional Euler->Quat->Euler round trip every frame
                // would slowly drift (and sign-flip) untouched rotations.
                const JPH::Vec3 euler = ZHLN::Math::QuatToEulerDegrees(field);
                float           deg[3] = {euler.GetX(), euler.GetY(), euler.GetZ()};
                bool            changed = false;
                for (int axis = 0; axis < 3; ++axis) {
                    std::array<char, 136> axisIdBuf {};
                    const std::string_view axisId = ZHLN::FormatTo(axisIdBuf, "{}_rot_{}", rowId, "xyz"[axis]);
                    std::array<char, 96>  axisLabelBuf {};
                    const std::string_view axisLabel = ZHLN::FormatTo(axisLabelBuf, "{} Rot {}", name, "XYZ"[axis]);
                    const float          prev = deg[axis];
                    gui.Slider(axisId, axisLabel, deg[axis], -360.0f, 360.0f, 1.0f);
                    if (deg[axis] != prev) {
                        changed = true;
                    }
                }
                if (changed) {
                    field = ZHLN::Math::EulerDegreesToQuat(JPH::Vec3(deg[0], deg[1], deg[2]));
                }
            } else if constexpr (std::is_same_v<FT, ZHLN::String256> || std::is_same_v<FT, ZHLN::String64>) {
                gui.TextInput(rowId, name, field);
            }
            // Everything else (Entity handles, TextureHandle, char padding,
            // nested structs) intentionally gets no row in this version.
        };
    }

} // namespace

auto DrawHierarchyPanel(
    GUI::Context&     gui,
    EditorState&      state,
    std::string_view  id
) -> ZHLN::Entity {
    ZHLN::ECS::Registry& reg = gui.GetRegistry();
    struct Row {
        ZHLN::Entity entity;
        uint32_t     depth;
        uint32_t     order;
    };

    // Snapshot + sort: GetEntitiesWith returns dense-array order, which
    // reshuffles on every swap-remove destroy. UI entities sort by
    // (hierarchyDepth, layoutOrder) — the stable key layout/render/
    // hit-testing already use. Pure 3D entities (no UIRectComponent) have
    // no layout stamps; they fall back to (0, entity index) so the list is
    // still deterministic and they group ahead of deep UI nesting.
    std::vector<Row> rows;
    for (const ZHLN::Entity e: reg.GetEntitiesWith<Comp::NameComponent>()) {
        if (IsEditorEntity(e, reg, state.editorRoot)) {
            continue;
        }
        const auto* rect = reg.Get<UIComp::UIRectComponent>(e);
        rows.push_back(Row {e,
                            rect != nullptr ? rect->hierarchyDepth : 0u,
                            rect != nullptr ? rect->layoutOrder : e.index});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) -> bool {
        if (a.depth != b.depth) {
            return a.depth < b.depth;
        }
        return a.order < b.order;
    });

    return gui.ScrollBox(
        id,
        GUI::ScrollBoxConfig {
            .flexGrow = 1.0f // The panel fills whatever the host layout gives it.
        },
        [&]() -> void {
            for (const Row& row: rows) {
                std::array<char, 96> rowIdBuf {};
                const std::string_view rowId = ZHLN::FormatTo(rowIdBuf, "{}_row{}", id, row.entity.index);

                std::array<char, 96> fallbackBuf {};
                std::string_view     label;
                if (const auto* name = reg.Get<Comp::NameComponent>(row.entity)) {
                    label = std::string_view(name->name);
                } else {
                    label = ZHLN::FormatTo(fallbackBuf, "Entity {}", row.entity.index);
                }

                bool selected = (state.selectedEntity == row.entity);
                gui.Selectable(
                    rowId,
                    label,
                    selected,
                    GUI::SelectableConfig {},
                    [&state, e = row.entity](bool nowSelected) -> void {
                        if (nowSelected) {
                            state.selectedEntity = e;
                        }
                    }
                );
            }
        }
    );
}

auto DrawInspectorPanel(
    GUI::Context&     gui,
    EditorState&      state,
    std::string_view  id
) -> ZHLN::Entity {
    ZHLN::ECS::Registry& reg = gui.GetRegistry();
    return gui.ScrollBox(
        id,
        GUI::ScrollBoxConfig {
            .flexGrow = 1.0f
        },
        [&]() -> void {
            const ZHLN::Entity sel = state.selectedEntity;
            if (sel == ZHLN::Entity::Null() || !reg.IsAlive(sel)) {
                gui.Label("No selection");
                return;
            }

            std::array<char, 64> headerBuf {};
            gui.Label(std::string_view(ZHLN::FormatTo(headerBuf, "Entity {}", sel.index)));

            // One collapsing section per editable component, each with its
            // OWN ForEachFieldWithName call site. Two invariants make these
            // blocks load-bearing:
            //
            //   1. The call's object expression must have a CONCRETE static
            //      type. The transpiler fallback extracts the field list from
            //      the object's type, so a call inside a template with a
            //      dependent T would flatten to zero rows.
            //
            //   2. The rows are drawn against a LOCAL COPY of the component,
            //      which is patched back afterwards. Every widget built for a
            //      row can create entities (labels, inner text children, ...),
            //      and entity creation can reallocate the component pool —
            //      which would leave a reference bound to the pool dangling
            //      mid-iteration, with every later row writing through freed
            //      memory. (Found by the flattened-path verifier: flaky
            //      SIGSEGV inside glibc's malloc bin traversal.)
            const auto section = [&](std::string_view sectionId, std::string_view title, auto* comp, auto&& reflect) -> void {
                if (comp == nullptr) {
                    return;
                }
                using CompT = std::remove_pointer_t<decltype(comp)>;
                gui.CollapsingHeader(sectionId, title, true, [&]() -> void {
                    CompT local = *comp;
                    reflect(local, MakeRowSink(gui, state.editorRoot, sectionId));
                    reg.Patch<CompT>(sel, [&local](CompT& dst) -> void { dst = local; });
                });
            };

            section("name", "Name", reg.Get<Comp::NameComponent>(sel),
                    [](Comp::NameComponent& c, auto&& sink) -> void { ZHLN::Reflect::ForEachFieldWithName(c, sink); });
            section("transform", "Transform", reg.Get<Comp::TransformComponent>(sel),
                    [](Comp::TransformComponent& c, auto&& sink) -> void { ZHLN::Reflect::ForEachFieldWithName(c, sink); });
            section("pbr", "PBR Material", reg.Get<Comp::PBRComponent>(sel),
                    [](Comp::PBRComponent& c, auto&& sink) -> void { ZHLN::Reflect::ForEachFieldWithName(c, sink); });
            section("light", "Light", reg.Get<Comp::LightComponent>(sel),
                    [](Comp::LightComponent& c, auto&& sink) -> void { ZHLN::Reflect::ForEachFieldWithName(c, sink); });
            section("rect", "Rect", reg.Get<UIComp::UIRectComponent>(sel),
                    [](UIComp::UIRectComponent& c, auto&& sink) -> void { ZHLN::Reflect::ForEachFieldWithName(c, sink); });
            section("flex", "Flex", reg.Get<UIComp::UIFlexComponent>(sel),
                    [](UIComp::UIFlexComponent& c, auto&& sink) -> void { ZHLN::Reflect::ForEachFieldWithName(c, sink); });
            section("panel", "Panel", reg.Get<UIComp::UIPanelComponent>(sel),
                    [](UIComp::UIPanelComponent& c, auto&& sink) -> void { ZHLN::Reflect::ForEachFieldWithName(c, sink); });
            section("text", "Text", reg.Get<UIComp::TextComponent>(sel),
                    [](UIComp::TextComponent& c, auto&& sink) -> void { ZHLN::Reflect::ForEachFieldWithName(c, sink); });
        }
    );
}

} // namespace ZHLN::Editor
