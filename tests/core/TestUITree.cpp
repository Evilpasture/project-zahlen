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
    TreeMutationFailed   ZHLN_ANNOTATION(ZHLN::Description<"FindNodeById / InsertChild / RemoveNodeById did not mutate the tree as specified."> {}),
};

struct UITreeTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> action_registry_bind_invoke_and_missing_ids() {
            ZHLN::ECS::EventBus       bus;
            ZHLN::GUI::ActionRegistry actions(bus);
            int                       saves = 0;
            struct SaveEvent {
                int add = 1;
            };
            actions.Bind("editor.save_scene", SaveEvent {.add = 1});

            ZHLN::Test::ExpectTrue(actions.Contains("editor.save_scene"));
            ZHLN::Test::ExpectFalse(actions.Contains("editor.open_scene"));
            ZHLN::Test::ExpectFalse(actions.Contains(""));

            ZHLN::Test::ExpectTrue(actions.Invoke("editor.save_scene"));
            bus.Drain<SaveEvent>([&](const SaveEvent& e) { saves += e.add; });
            ZHLN::Test::ExpectEq(saves, 1);

            // A document can name an action this host has not installed.
            ZHLN::Test::ExpectFalse(actions.Invoke("editor.open_scene"));
            ZHLN::Test::ExpectFalse(actions.Invoke(""));
            ZHLN::Test::ExpectEq(saves, 1);
            ZHLN::Test::ExpectTrue(bus.View<SaveEvent>().empty());

            actions.Bind("editor.save_scene", SaveEvent {.add = 10});
            ZHLN::Test::ExpectTrue(actions.Invoke("editor.save_scene"));
            bus.Drain<SaveEvent>([&](const SaveEvent& e) { saves += e.add; });
            ZHLN::Test::ExpectEq(saves, 11);

            actions.Unbind("editor.save_scene");
            ZHLN::Test::ExpectFalse(actions.Contains("editor.save_scene"));
            ZHLN::Test::ExpectFalse(actions.Invoke("editor.save_scene"));
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

            ZHLN::ECS::EventBus       bus;
            ZHLN::GUI::ActionRegistry actions(bus);
            int                       saves = 0;
            struct SaveEvent {};
            actions.Bind("editor.save_scene", SaveEvent {});

            ZHLN::GUI::PropertyStore properties;
            properties.SetBool("post.enableSSR", true);

            const auto result = ZHLN::GUI::RenderUITree(gui, root, actions, properties);
            bus.Drain<SaveEvent>([&](const SaveEvent&) { ++saves; });
            gui.EndFrame();

            // No pointer, so the walk must not fire the bound action.
            ZHLN::Test::ExpectFalse(result.actionInvoked);
            ZHLN::Test::ExpectTrue(result.clickedId.empty());
            ZHLN::Test::ExpectEq(saves, 0);
            ZHLN::Test::ExpectTrue(properties.GetBool("post.enableSSR"));

            return {};
        }

        std::expected<void, ZHLN::Error> find_insert_and_remove_use_the_same_ids_as_the_walk() {
            ZHLN::GUI::UINode root;
            root.id   = "panel";
            root.kind = ZHLN::GUI::NodeKind::Column;
            root.children.push_back(ZHLN::GUI::UINode {.kind = ZHLN::GUI::NodeKind::Text, .label = "Hello"});
            root.children.push_back(ZHLN::GUI::UINode {
                .id            = "save",
                .kind          = ZHLN::GUI::NodeKind::Button,
                .label         = "Save",
                .onClickAction = "editor.save_scene",
            });

            // Named nodes resolve by id; anonymous children by the path
            // RenderUITree would report ("panel/0").
            ZHLN::GUI::UINode* hello = ZHLN::GUI::FindNodeById(root, "panel/0");
            ZHLN::GUI::UINode* save  = ZHLN::GUI::FindNodeById(root, "save");
            if (!ZHLN::Test::ExpectTrue(hello != nullptr && save != nullptr && ZHLN::GUI::FindNodeById(root, "panel") == &root)) {
                return std::unexpected(UITreeTestError::TreeMutationFailed);
            }
            ZHLN::Test::ExpectEq(std::string_view(hello->label), std::string_view("Hello"));
            ZHLN::Test::ExpectEq(std::string_view(save->label), std::string_view("Save"));
            ZHLN::Test::ExpectTrue(ZHLN::GUI::FindNodeById(root, "missing") == nullptr);
            ZHLN::Test::ExpectTrue(ZHLN::GUI::FindNodeById(root, "") == nullptr);

            const ZHLN::GUI::UINode& constRoot = root;
            ZHLN::Test::ExpectTrue(ZHLN::GUI::FindNodeById(constRoot, "save") == save);

            ZHLN::Test::ExpectTrue(ZHLN::GUI::InsertChild(
                root, "panel", ZHLN::GUI::UINode {.id = "ssr", .kind = ZHLN::GUI::NodeKind::Checkbox, .label = "SSR"}
            ));
            ZHLN::Test::ExpectEq(root.children.size(), size_t {3});
            ZHLN::Test::ExpectTrue(ZHLN::GUI::FindNodeById(root, "ssr") != nullptr);
            ZHLN::Test::ExpectFalse(ZHLN::GUI::InsertChild(root, "nope", ZHLN::GUI::UINode {}));

            // The root cannot be deleted; a named child can, and so can an
            // anonymous one addressed by path.
            ZHLN::Test::ExpectFalse(ZHLN::GUI::RemoveNodeById(root, "panel"));
            ZHLN::Test::ExpectTrue(ZHLN::GUI::RemoveNodeById(root, "save"));
            ZHLN::Test::ExpectTrue(ZHLN::GUI::FindNodeById(root, "save") == nullptr);
            ZHLN::Test::ExpectTrue(ZHLN::GUI::RemoveNodeById(root, "panel/0"));
            ZHLN::Test::ExpectEq(root.children.size(), size_t {1});
            ZHLN::Test::ExpectEq(std::string_view(root.children[0].id), std::string_view("ssr"));

            return {};
        }

        std::expected<void, ZHLN::Error> design_mode_does_not_invoke_and_accepts_a_selection() {
            ZHLN::ECS::Registry registry;
            ZHLN::GUI::Context  gui(registry, {640, 480});
            gui.BeginFrame(0.016f);

            ZHLN::GUI::UINode root;
            root.id   = "panel";
            root.kind = ZHLN::GUI::NodeKind::Column;
            root.box.color = {0.1f, 0.1f, 0.1f, 1.0f};
            root.children.push_back(ZHLN::GUI::UINode {
                .id            = "save",
                .kind          = ZHLN::GUI::NodeKind::Button,
                .label         = "Save",
                .onClickAction = "editor.save_scene",
            });

            ZHLN::ECS::EventBus       bus;
            ZHLN::GUI::ActionRegistry actions(bus);
            int                       saves = 0;
            struct SaveEvent {};
            actions.Bind("editor.save_scene", SaveEvent {});
            ZHLN::GUI::PropertyStore properties;

            const auto result =
                ZHLN::GUI::RenderUITree(gui, root, actions, properties, ZHLN::GUI::TreeMode::Design, "panel");
            gui.EndFrame();
            bus.Drain<SaveEvent>([&](const SaveEvent&) { ++saves; });

            ZHLN::Test::ExpectFalse(result.actionInvoked);
            ZHLN::Test::ExpectEq(saves, 0);
            return {};
        }
    };
};

auto RunUITreeSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<UITreeTestSuite>();
}
