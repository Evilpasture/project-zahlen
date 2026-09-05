// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// src/gui/GUIEditor.cpp
//
// Native editor panels (Hierarchy + Inspector). See include/Zahlen/GUIEditor.hpp
// for the design notes; this file is where the reflection iteration lives, so
// that the transpiler fallback (tools/transpile_reflection.py, which rewrites
// reflection calls by translation-unit source offset) sees and flattens it.

#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/GUIEditor.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/UIComponents.hpp>
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
[[nodiscard]] auto IsEditorEntity(ZHLN::Entity e, const ZHLN::ECS::Registry& reg, ZHLN::Entity editorRoot) -> bool {
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

// The generic row sink: one row per reflected (name, field) pair. The
// field name is the label; the row id is section-scoped so two components
// with a `width` field cannot collide in the child cache.
//
// Missing widget types (Dropdown, TextInput, Reference) are rendered as
// read-only text until those widgets are implemented in the Clay API.
[[nodiscard]] auto MakeRowSink(GUI::Context& gui, std::string_view sectionId) {
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
            // Render enum index as a float slider (dropdown stub)
            constexpr auto names = ZHLN::Reflect::EnumNames<FT>();
            if constexpr (names.size() > 0) {
                int idx = static_cast<int>(field);
                if (idx < 0 || static_cast<size_t>(idx) >= names.size())
                    idx = 0;
                // Stub: show current enum name as text
                std::array<char, 128> buf {};
                auto                  sv = ZHLN::FormatTo(buf, "{}: {}", name, names[static_cast<size_t>(idx)]);
                gui.Text(sv, 12.0f, {0.8f, 0.8f, 0.8f, 1.0f});
            }
        } else if constexpr (std::is_same_v<FT, ZHLN::Entity>) {
            // Stub: show packed handle as text
            std::array<char, 64> buf {};
            auto                 sv = ZHLN::FormatTo(buf, "{}: Entity({})", name, field.Pack());
            gui.Text(sv, 12.0f, {0.7f, 0.7f, 0.7f, 1.0f});
        } else if constexpr (std::is_same_v<FT, TextureHandle>) {
            std::array<char, 64> buf {};
            auto                 sv = ZHLN::FormatTo(buf, "{}: Texture({})", name, static_cast<uint64_t>(field));
            gui.Text(sv, 12.0f, {0.7f, 0.7f, 0.7f, 1.0f});
        } else if constexpr (std::is_same_v<FT, JPH::Vec4>) {
            float v[4] = {field.GetX(), field.GetY(), field.GetZ(), field.GetW()};
            for (int axis = 0; axis < 4; ++axis) {
                std::array<char, 136> axisIdBuf {};
                auto                  axisLabel = ZHLN::FormatTo(axisIdBuf, "{} {}", name, "XYZW"[axis]);
                gui.Slider(axisLabel, v[axis], -10000.0f, 10000.0f);
            }
            field.SetX(v[0]);
            field.SetY(v[1]);
            field.SetZ(v[2]);
            field.SetW(v[3]);
        } else if constexpr (std::is_same_v<FT, JPH::Vec3>) {
            float v[3] = {field.GetX(), field.GetY(), field.GetZ()};
            for (int axis = 0; axis < 3; ++axis) {
                std::array<char, 136> axisIdBuf {};
                auto                  axisLabel = ZHLN::FormatTo(axisIdBuf, "{} {}", name, "XYZ"[axis]);
                gui.Slider(axisLabel, v[axis], -10000.0f, 10000.0f);
            }
            field = JPH::Vec3(v[0], v[1], v[2]);
        } else if constexpr (std::is_same_v<FT, JPH::Quat>) {
            const JPH::Vec3 euler   = ZHLN::Math::QuatToEulerDegrees(field);
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
                field = ZHLN::Math::EulerDegreesToQuat(JPH::Vec3(deg[0], deg[1], deg[2]));
            }
        } else if constexpr (std::is_same_v<FT, ZHLN::String256> || std::is_same_v<FT, ZHLN::String64>) {
            // Stub: show as read-only text until TextInput is implemented
            std::array<char, 32> buf {};
            auto                 label = ZHLN::FormatTo(buf, "{}: ", name);
            gui.Text(label, 12.0f, {0.7f, 0.7f, 0.7f, 1.0f});
            gui.Text(std::string_view(field), 12.0f, {0.9f, 0.9f, 0.9f, 1.0f});
        }
        // Everything else (char padding, nested structs) intentionally
        // gets no row in this version.
    };
}

} // namespace

void DrawHierarchyPanel(GUI::Context& gui, ZHLN::ECS::Registry& reg, EditorState& state, std::string_view id) {
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
        rows.push_back(Row {e, rect != nullptr ? rect->hierarchyDepth : 0u, rect != nullptr ? rect->layoutOrder : e.index});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) -> bool {
        if (a.depth != b.depth)
            return a.depth < b.depth;
        return a.order < b.order;
    });

    // Render the hierarchy as a scrollable column
    gui.BeginColumn(4.0f);
    gui.Text(id, 14.0f, {0.6f, 0.7f, 0.8f, 1.0f});

    for (const Row& row: rows) {
        std::array<char, 96> fallbackBuf {};
        std::string_view     label;
        if (const auto* name = reg.Get<Comp::NameComponent>(row.entity)) {
            label = std::string_view(name->name);
        } else {
            label = ZHLN::FormatTo(fallbackBuf, "Entity {}", row.entity.index);
        }

        bool      isSelected = (state.selectedEntity == row.entity);
        JPH::Vec4 color      = isSelected ? JPH::Vec4(0.20f, 0.35f, 0.55f, 0.9f) : JPH::Vec4(0.10f, 0.12f, 0.16f, 0.7f);

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

void DrawInspectorPanel(GUI::Context& gui, ZHLN::ECS::Registry& reg, EditorState& state, std::string_view id) {
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
    const auto section = [&](std::string_view sectionId, std::string_view title, auto* comp, auto&& reflect) -> void {
        if (comp == nullptr)
            return;
        using CompT = std::remove_pointer_t<decltype(comp)>;
        if (gui.BeginCollapsingHeader(title, true)) {
            CompT local = *comp;
            reflect(local, MakeRowSink(gui, sectionId));
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
    section("rect", "Rect", reg.Get<UIComp::UIRectComponent>(sel), [](UIComp::UIRectComponent& c, auto&& sink) -> void {
        ZHLN::Reflect::ForEachFieldWithName(c, sink);
    });
    section("flex", "Flex", reg.Get<UIComp::UIFlexComponent>(sel), [](UIComp::UIFlexComponent& c, auto&& sink) -> void {
        ZHLN::Reflect::ForEachFieldWithName(c, sink);
    });
    section("panel", "Panel", reg.Get<UIComp::UIPanelComponent>(sel), [](UIComp::UIPanelComponent& c, auto&& sink) -> void {
        ZHLN::Reflect::ForEachFieldWithName(c, sink);
    });
    section("text", "Text", reg.Get<UIComp::TextComponent>(sel), [](UIComp::TextComponent& c, auto&& sink) -> void {
        ZHLN::Reflect::ForEachFieldWithName(c, sink);
    });

    gui.EndColumn();
}

} // namespace ZHLN::Editor
