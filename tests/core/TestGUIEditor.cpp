// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests/core/TestGUIEditor.cpp
//
// Behavioural tests for the native editor panels (Zahlen/GUIEditor.hpp):
//
//   - The hierarchy lists exactly the scene's UI entities, hides the editor's
//     own subtree, and tracks selection state through EditorState.
//   - The inspector shows a placeholder without a selection, builds one
//     collapsing section per editable component, and — on toolchains with
//     native reflection — one row per reflected field.
//
// The reflection-driven rows are the interesting part, and they are gated on
// ZHLN::Reflect::ReflectionAvailable: on a compiler without P2996 (and no
// transpiled build) the field iterator is a no-op by design, so the tests
// assert the panel still builds cleanly instead of expecting rows.
//
// Public API only (Zahlen/GUIEditor.hpp + Zahlen/GUI.hpp +
// Zahlen/Components.hpp), matching the tests/ tree rule.

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/GUIEditor.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ZHLN::Entity;
using ZHLN::ECS::Registry;
namespace GUI    = ZHLN::GUI;
namespace Editor = ZHLN::Editor;
using Comp       = ZHLN::Components;
using UIComp     = ZHLN::GUI::UIComponents;

// Finds the (first) entity whose NameComponent matches `name`, anywhere in the
// registry. Editor widgets are named by their id, exactly like the compound
// widgets in GUI.hpp, so this doubles as "did the panel build this child".
[[nodiscard]] auto FindEntityNamed(Registry& reg, std::string_view name) -> Entity {
    for (const Entity e: reg.GetEntitiesWith<Comp::NameComponent>()) {
        if (const auto* n = reg.Get<Comp::NameComponent>(e)) {
            if (std::string_view(n->name) == name) {
                return e;
            }
        }
    }
    return Entity::Null();
}

[[nodiscard]] auto HasText(Registry& reg, std::string_view text) -> bool {
    for (const Entity e: reg.GetEntitiesWith<UIComp::TextComponent>()) {
        if (const auto* t = reg.Get<UIComp::TextComponent>(e)) {
            if (std::string_view(t->text) == text) {
                return true;
            }
        }
    }
    return false;
}

// A scene entity: rect (so the hierarchy lists it) + name (so the row is
// readable). depth/order feed the hierarchy sort key.
[[nodiscard]] auto MakeSceneEntity(
    Registry&          reg,
    std::string_view   name,
    uint32_t           depth,
    uint32_t           order,
    Entity             parent = Entity::Null()
) -> Entity {
    return reg.Create(
        UIComp::UIRectComponent {.parentEntity = parent, .hierarchyDepth = depth, .layoutOrder = order},
        Comp::NameComponent {.name = ZHLN::String64(name)}
    );
}

// Collects the row entities of a hierarchy panel in child-cache order.
[[nodiscard]] auto CollectRows(Registry& reg, Entity panel, std::string_view idPrefix) -> std::vector<Entity> {
    std::vector<Entity> rows;
    for (const Entity e: reg.GetEntitiesWith<UIComp::UISelectableComponent>()) {
        const auto* n = reg.Get<Comp::NameComponent>(e);
        if (n == nullptr) {
            continue;
        }
        if (std::string_view(n->name).starts_with(idPrefix)) {
            rows.push_back(e);
        }
    }
    return rows;
}

} // namespace

// A synthetic font metric table, matching the pattern the other GUI suites
// use: the inspector's TextInput rows resolve the font through the
// UISettingsComponent singleton, so inspector tests install one.
[[nodiscard]] auto MakeTestFont() -> ZHLN::FontAtlas {
    ZHLN::FontAtlas font {};
    for (auto& g: font.glyphs) {
        g.x0       = 0.0f;
        g.y0       = 0.0f;
        g.x1       = 8.0f;
        g.y1       = 12.0f;
        g.xoff     = 0.0f;
        g.yoff     = -20.0f;
        g.xadvance = 10.0f;
    }
    return font;
}

struct GUIEditorTestSuite {
    struct Tests {
        // ------------------------------------------------------------------
        // HIERARCHY
        // ------------------------------------------------------------------
        auto hierarchy_lists_every_scene_entity() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   a = MakeSceneEntity(reg, "Alpha", 0, 2);
            Entity   b = MakeSceneEntity(reg, "Zeta", 0, 5);
            Entity   c = MakeSceneEntity(reg, "Child", 1, 1, a);

            Editor::EditorState st;
            Entity              panel = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                panel = Editor::DrawHierarchyPanel(gui, st, "H");
                ZHLN::Test::ExpectTrue(gui.Status().has_value());
            }

            ZHLN::Test::ExpectTrue(reg.IsAlive(panel));
            // One row per scene entity, keyed by entity index.
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "H_row" + std::to_string(a.index)) != Entity::Null());
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "H_row" + std::to_string(b.index)) != Entity::Null());
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "H_row" + std::to_string(c.index)) != Entity::Null());
            // Row labels come from NameComponent.
            ZHLN::Test::ExpectTrue(HasText(reg, "Alpha"));
            ZHLN::Test::ExpectTrue(HasText(reg, "Zeta"));
            return {};
        }

        auto hierarchy_hides_the_editor_subtree() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   world = MakeSceneEntity(reg, "World", 0, 1);
            Entity   root  = MakeSceneEntity(reg, "EditorChrome", 0, 2);
            Entity   kid   = MakeSceneEntity(reg, "EditorButton", 1, 1, root);

            Editor::EditorState st;
            st.editorRoot = root;
            {
                GUI::Context gui(reg, 1);
                const Entity panel = Editor::DrawHierarchyPanel(gui, st, "H");
                ZHLN::Test::ExpectTrue(gui.Status().has_value());
            }

            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "H_row" + std::to_string(world.index)) != Entity::Null());
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "H_row" + std::to_string(root.index)) == Entity::Null());
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "H_row" + std::to_string(kid.index)) == Entity::Null());
            return {};
        }

        auto hierarchy_selection_flag_follows_editor_state() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   a = MakeSceneEntity(reg, "Alpha", 0, 1);
            Entity   b = MakeSceneEntity(reg, "Beta", 0, 2);

            Editor::EditorState st;
            st.selectedEntity = b;
            {
                GUI::Context gui(reg, 1);
                [[maybe_unused]] const Entity panel = Editor::DrawHierarchyPanel(gui, st, "H");
            }

            const Entity rowA = FindEntityNamed(reg, "H_row" + std::to_string(a.index));
            const Entity rowB = FindEntityNamed(reg, "H_row" + std::to_string(b.index));
            ZHLN::Test::ExpectTrue(rowA != Entity::Null() && rowB != Entity::Null());
            if (const auto* sa = reg.Get<UIComp::UISelectableComponent>(rowA)) {
                ZHLN::Test::ExpectTrue(!sa->selected);
            } else {
                ZHLN::Test::ExpectTrue(false);
            }
            if (const auto* sb = reg.Get<UIComp::UISelectableComponent>(rowB)) {
                ZHLN::Test::ExpectTrue(sb->selected);
            } else {
                ZHLN::Test::ExpectTrue(false);
            }
            return {};
        }

        // ------------------------------------------------------------------
        // INSPECTOR
        // ------------------------------------------------------------------
        auto inspector_shows_placeholder_without_selection() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Editor::EditorState st;
            {
                GUI::Context gui(reg, 1);
                const Entity panel = Editor::DrawInspectorPanel(gui, st, "I");
                ZHLN::Test::ExpectTrue(reg.IsAlive(panel));
                ZHLN::Test::ExpectTrue(gui.Status().has_value());
            }
            ZHLN::Test::ExpectTrue(HasText(reg, "No selection"));
            return {};
        }

        auto inspector_builds_a_section_per_component() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            reg.Create(UIComp::UISettingsComponent {.fontAtlas = MakeTestFont()});
            Entity   e = reg.Create(
                UIComp::UIRectComponent {.hierarchyDepth = 0, .layoutOrder = 1},
                UIComp::UIFlexComponent {},
                Comp::NameComponent {.name = ZHLN::String64("Foo")}
            );

            Editor::EditorState st;
            st.selectedEntity = e;
            {
                GUI::Context gui(reg, 1);
                const Entity panel = Editor::DrawInspectorPanel(gui, st, "I");
                ZHLN::Test::ExpectTrue(reg.IsAlive(panel));
                ZHLN::Test::ExpectTrue(gui.Status().has_value());
            }

            // Section headers are keyed by their ids.
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "name") != Entity::Null());
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "rect") != Entity::Null());
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "flex") != Entity::Null());
            // The entity has no UIPanelComponent/TextComponent, so those
            // sections must NOT exist.
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "panel") == Entity::Null());
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "text") == Entity::Null());
            return {};
        }

        auto inspector_rows_follow_reflection() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            reg.Create(UIComp::UISettingsComponent {.fontAtlas = MakeTestFont()});
            Entity   e = reg.Create(
                UIComp::UIRectComponent {.hierarchyDepth = 0, .layoutOrder = 1},
                UIComp::UIFlexComponent {}
            );

            Editor::EditorState st;
            st.selectedEntity = e;
            {
                GUI::Context gui(reg, 1);
                const Entity panel = Editor::DrawInspectorPanel(gui, st, "I");
                ZHLN::Test::ExpectTrue(gui.Status().has_value());
            }

            if constexpr (ZHLN::Reflect::ReflectionAvailable) {
                // One row per public field: slider for floats, dropdown for
                // enums, section-scoped ids.
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "flex_flexGrow") != Entity::Null());
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "flex_direction") != Entity::Null());
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "rect_width") != Entity::Null());
                // Padding members are skipped.
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "rect__free_space") == Entity::Null());
            } else {
                // No native reflection and no transpiled build: the field
                // iterator is the documented no-op. The panel still builds.
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "flex_flexGrow") == Entity::Null());
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "rect") != Entity::Null());
            }
            return {};
        }

        auto hierarchy_lists_pure_3d_entities() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            // A 3D scene entity: named + transform, but no UIRectComponent.
            Entity   prop = reg.Create(
                Comp::NameComponent {.name = ZHLN::String64("Barrel")},
                Comp::TransformComponent {}
            );
            Entity   ui = MakeSceneEntity(reg, "Panel", 0, 3);

            Editor::EditorState st;
            {
                GUI::Context gui(reg, 1);
                const Entity panel = Editor::DrawHierarchyPanel(gui, st, "H");
                ZHLN::Test::ExpectTrue(reg.IsAlive(panel));
            }
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "H_row" + std::to_string(prop.index)) != Entity::Null());
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "H_row" + std::to_string(ui.index)) != Entity::Null());
            ZHLN::Test::ExpectTrue(HasText(reg, "Barrel"));
            return {};
        }

        auto inspector_covers_3d_components() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            reg.Create(UIComp::UISettingsComponent {.fontAtlas = MakeTestFont()});
            Entity   e = reg.Create(
                Comp::NameComponent {.name = ZHLN::String64("Lamp")},
                Comp::TransformComponent {},
                Comp::PBRComponent {},
                Comp::LightComponent {}
            );

            Editor::EditorState st;
            st.selectedEntity = e;
            {
                GUI::Context gui(reg, 1);
                const Entity panel = Editor::DrawInspectorPanel(gui, st, "I");
                ZHLN::Test::ExpectTrue(reg.IsAlive(panel));
                ZHLN::Test::ExpectTrue(gui.Status().has_value());
            }

            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "transform") != Entity::Null());
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "pbr") != Entity::Null());
            ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "light") != Entity::Null());

            if constexpr (ZHLN::Reflect::ReflectionAvailable) {
                // Vec3 -> three axis rows, Quat -> three Euler-degree rows.
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "transform_position_x") != Entity::Null());
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "transform_position_z") != Entity::Null());
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "transform_rotation_rot_y") != Entity::Null());
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "transform_scale_x") != Entity::Null());
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "light_intensity") != Entity::Null());
                // Mat44 (LightComponent::points) has no row type yet.
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "light_points") == Entity::Null());
            }
            return {};
        }

        auto inspector_rotation_roundtrip_is_stable_when_unedited() -> std::expected<void, ZHLN::Error> {
            if constexpr (!ZHLN::Reflect::ReflectionAvailable) {
                return {}; // no rows, no write-back path to exercise
            } else {
                Registry reg;
                reg.Create(UIComp::UISettingsComponent {.fontAtlas = MakeTestFont()});
                const JPH::Quat original = ZHLN::Math::EulerDegreesToQuat(JPH::Vec3(10.0f, -45.0f, 170.0f));
                Entity          e        = reg.Create(Comp::TransformComponent {.rotation = original});

                Editor::EditorState st;
                st.selectedEntity = e;
                // Draw twice; nothing edits the rows, so the stored quaternion
                // must survive both frames bit-identical (no Euler round-trip
                // drift on untouched rotations).
                for (int frame = 0; frame < 2; ++frame) {
                    GUI::Context gui(reg, frame + 1);
                    const Entity panel = Editor::DrawInspectorPanel(gui, st, "I");
                    ZHLN::Test::ExpectTrue(reg.IsAlive(panel));
                }
                const auto* tf = reg.Get<Comp::TransformComponent>(e);
                ZHLN::Test::ExpectTrue(tf != nullptr);
                if (tf != nullptr) {
                    ZHLN::Test::ExpectTrue(tf->rotation == original);
                }
                return {};
            }
        }

        // Regression: rows used to be drawn through a live reference into the
        // component pool. Every row widget can create entities, entity
        // creation can reallocate the pool, and the reference went stale
        // mid-iteration — later rows wrote through freed memory (flaky
        // SIGSEGV inside malloc on the transpiled-path verifier). The
        // inspector now draws against a local copy and patches back, so the
        // component must survive a full row build byte-identical.
        auto inspector_preserves_component_values_across_row_builds() -> std::expected<void, ZHLN::Error> {
            if constexpr (!ZHLN::Reflect::ReflectionAvailable) {
                // The stub iterator builds zero rows, so there is nothing to
                // exercise; the hazard only materialises when rows exist.
                return {};
            } else {
                Registry reg;
                reg.Create(UIComp::UISettingsComponent {.fontAtlas = MakeTestFont()});
                Entity   e = reg.Create(
                    UIComp::UIRectComponent {.x = 3.0f, .y = 4.0f, .hierarchyDepth = 0, .layoutOrder = 1},
                    UIComp::UIFlexComponent {.flexGrow = 0.75f},
                    Comp::NameComponent {.name = ZHLN::String64("Foo")}
                );

                Editor::EditorState st;
                st.selectedEntity = e;
                {
                    GUI::Context gui(reg, 1);
                    const Entity panel = Editor::DrawInspectorPanel(gui, st, "I");
                    ZHLN::Test::ExpectTrue(gui.Status().has_value());
                    ZHLN::Test::ExpectTrue(reg.IsAlive(panel));
                }

                // Rows built (so the write-back path actually ran)...
                ZHLN::Test::ExpectTrue(FindEntityNamed(reg, "flex_flexGrow") != Entity::Null());
                // ...and every value round-tripped through the local copy.
                const auto* rect = reg.Get<UIComp::UIRectComponent>(e);
                const auto* flex = reg.Get<UIComp::UIFlexComponent>(e);
                const auto* name = reg.Get<Comp::NameComponent>(e);
                ZHLN::Test::ExpectTrue(rect != nullptr && flex != nullptr && name != nullptr);
                if (rect != nullptr) {
                    ZHLN::Test::ExpectEq(rect->x, 3.0f);
                    ZHLN::Test::ExpectEq(rect->y, 4.0f);
                }
                if (flex != nullptr) {
                    ZHLN::Test::ExpectEq(flex->flexGrow, 0.75f);
                }
                if (name != nullptr) {
                    ZHLN::Test::ExpectEq(std::string_view(name->name), std::string_view("Foo"));
                }
                return {};
            }
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which aggregates
// every suite in this directory through Runner::RunDeferred.
auto RunGUIEditorSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<GUIEditorTestSuite>();
}
