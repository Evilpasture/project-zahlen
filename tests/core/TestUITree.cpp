// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Data-driven GUI trees: the ActionRegistry / PropertyStore tables a document
// can name, and RenderUITree walking a UINode into Clay without a GPU.

#include "TestsFramework.hpp"
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/UITree.hpp>
#include <expected>
#include <string>
#include <string_view>

enum class UITreeTestError : uint8_t {
    ActionRegistryFailed ZHLN_ANNOTATION(ZHLN::Description<"ActionRegistry bind / invoke did not behave as specified."> {}) = 1,
    PropertyStoreFailed  ZHLN_ANNOTATION(ZHLN::Description<"PropertyStore get / set did not round-trip or keep types."> {}),
    RenderWalkFailed     ZHLN_ANNOTATION(ZHLN::Description<"RenderUITree did not walk a CPU-constructed tree."> {}),
};

struct UITreeTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> action_registry_bind_invoke_and_missing_ids() {
            ZHLN::GUI::ActionRegistry actions;
            int                       saves = 0;
            actions.Bind("editor.save_scene", [&]() { ++saves; });

            ZHLN::Test::ExpectTrue(actions.Contains("editor.save_scene"));
            ZHLN::Test::ExpectFalse(actions.Contains("editor.open_scene"));
            ZHLN::Test::ExpectFalse(actions.Contains(""));

            actions.Invoke("editor.save_scene");
            ZHLN::Test::ExpectEq(saves, 1);

            // A document can name an action this host has not installed.
            actions.Invoke("editor.open_scene");
            actions.Invoke("");
            ZHLN::Test::ExpectEq(saves, 1);

            actions.Bind("editor.save_scene", [&]() { saves += 10; });
            actions.Invoke("editor.save_scene");
            ZHLN::Test::ExpectEq(saves, 11);

            actions.Unbind("editor.save_scene");
            ZHLN::Test::ExpectFalse(actions.Contains("editor.save_scene"));
            actions.Invoke("editor.save_scene");
            ZHLN::Test::ExpectEq(saves, 11);

            return {};
        }

        std::expected<void, ZHLN::Error> property_store_round_trips_and_rejects_type_mismatch() {
            ZHLN::GUI::PropertyStore store;
            store.SetBool("post.enableSSR", true);
            store.SetFloat("camera.speed", 4.5f);
            store.SetString("scene.name", "Fallback");

            ZHLN::Test::ExpectTrue(store.GetBool("post.enableSSR"));
            ZHLN::Test::ExpectEq(store.GetFloat("camera.speed"), 4.5f);
            ZHLN::Test::ExpectEq(std::string_view(store.GetString("scene.name")), std::string_view("Fallback"));

            // Wrong kind and missing path both return the caller's fallback.
            ZHLN::Test::ExpectEq(store.GetFloat("post.enableSSR", 1.0f), 1.0f);
            ZHLN::Test::ExpectFalse(store.GetBool("camera.speed", false));
            ZHLN::Test::ExpectEq(std::string_view(store.GetString("missing", "none")), std::string_view("none"));
            ZHLN::Test::ExpectFalse(store.Contains(""));

            store.SetFloat("camera.speed", 9.0f);
            ZHLN::Test::ExpectEq(store.GetFloat("camera.speed"), 9.0f);

            return {};
        }

        std::expected<void, ZHLN::Error> render_ui_tree_walks_without_a_click() {
            ZHLN::ECS::Registry registry;
            ZHLN::GUI::Context  gui(registry, {640, 480});
            gui.BeginFrame(0.016f);

            ZHLN::GUI::UINode root;
            root.id   = "panel";
            root.kind = ZHLN::GUI::NodeKind::Column;
            root.children.push_back(ZHLN::GUI::UINode {
                .id    = "title",
                .kind  = ZHLN::GUI::NodeKind::Text,
                .label = "Hello",
            });
            root.children.push_back(ZHLN::GUI::UINode {
                .id            = "save",
                .kind          = ZHLN::GUI::NodeKind::Button,
                .label         = "Save",
                .onClickAction = "editor.save_scene",
            });
            root.children.push_back(ZHLN::GUI::UINode {
                .id           = "ssr",
                .kind         = ZHLN::GUI::NodeKind::Checkbox,
                .label        = "SSR",
                .bindProperty = "post.enableSSR",
            });

            ZHLN::GUI::ActionRegistry actions;
            int                       saves = 0;
            actions.Bind("editor.save_scene", [&]() { ++saves; });

            ZHLN::GUI::PropertyStore properties;
            properties.SetBool("post.enableSSR", true);

            const auto result = ZHLN::GUI::RenderUITree(gui, root, actions, properties);
            gui.EndFrame();

            // No pointer, so the walk must not fire the bound action.
            ZHLN::Test::ExpectFalse(result.actionInvoked);
            ZHLN::Test::ExpectTrue(result.clickedId.empty());
            ZHLN::Test::ExpectEq(saves, 0);
            ZHLN::Test::ExpectTrue(properties.GetBool("post.enableSSR"));

            return {};
        }
    };
};

auto RunUITreeSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<UITreeTestSuite>();
}
