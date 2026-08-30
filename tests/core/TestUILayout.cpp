// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <expected>

struct UILayoutTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> public_gui_builds_flex_column_hierarchy() {
            ZHLN::ECS::Registry reg;
            ZHLN::GUI::Context  gui(reg);

            const ZHLN::GUI::PanelConfig rootCfg {
                .width     = 400.0f,
                .height    = 300.0f,
                .direction = ZHLN::FlexDirection::Column,
                .justify   = ZHLN::FlexJustify::FlexStart,
                .gap       = 10.0f,
                .padding   = 10.0f,
            };
            const ZHLN::GUI::BoxConfig childCfg {.height = 50.0f};

            ZHLN::Entity child1 = ZHLN::Entity::Null();
            ZHLN::Entity child2 = ZHLN::Entity::Null();
            ZHLN::Entity root   = gui.Panel("root", rootCfg, [&] {
                child1 = gui.Box("c1", childCfg, [] {});
                child2 = gui.Box("c2", childCfg, [] {});
            });

            const auto* rootRect = reg.Get<ZHLN::Components::UIRectComponent>(root);
            const auto* rootFlex = reg.Get<ZHLN::Components::UIFlexComponent>(root);
            const auto* r1       = reg.Get<ZHLN::Components::UIRectComponent>(child1);
            const auto* r2       = reg.Get<ZHLN::Components::UIRectComponent>(child2);

            ZHLN::Test::ExpectTrue(rootRect != nullptr && rootFlex != nullptr && r1 != nullptr && r2 != nullptr);
            if (rootRect == nullptr || rootFlex == nullptr || r1 == nullptr || r2 == nullptr) {
                return {};
            }

            ZHLN::Test::ExpectEq(rootRect->width, 400.0f);
            ZHLN::Test::ExpectEq(rootRect->height, 300.0f);
            ZHLN::Test::ExpectEq(rootFlex->direction, ZHLN::FlexDirection::Column);
            ZHLN::Test::ExpectEq(rootFlex->justify, ZHLN::FlexJustify::FlexStart);
            ZHLN::Test::ExpectEq(rootFlex->paddingTop, 10.0f);
            ZHLN::Test::ExpectEq(rootFlex->gapY, 10.0f);

            ZHLN::Test::ExpectEq(r1->parentEntity, root);
            ZHLN::Test::ExpectEq(r2->parentEntity, root);
            ZHLN::Test::ExpectEq(r1->height, 50.0f);
            ZHLN::Test::ExpectEq(r2->height, 50.0f);
            ZHLN::Test::ExpectTrue(r1->hierarchyDepth > rootRect->hierarchyDepth);
            ZHLN::Test::ExpectEq(r1->hierarchyDepth, r2->hierarchyDepth);

            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunUILayoutSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<UILayoutTestSuite>();
}

