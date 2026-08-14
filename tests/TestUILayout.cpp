// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <engine/system/UILayoutSystem.hpp>
#include <expected>

struct UILayoutTestSuite {
    struct Tests {
        std::expected<void, ZHLN::Error> flexbox_column_layout_resolution() {
            ZHLN::ECS::Registry  reg;
            ZHLN::UILayoutSystem layoutSystem;

            // Root panel: 400x300, Column, 10px padding, 10px gap
            ZHLN::Entity root = reg.Create(
                ZHLN::Components::UIRectComponent {.width = 400.0f, .height = 300.0f}, ZHLN::Components::UIFlexComponent {
                                                                                           .direction     = ZHLN::FlexDirection::Column,
                                                                                           .justify       = ZHLN::FlexJustify::FlexStart,
                                                                                           .paddingLeft   = 10.0f,
                                                                                           .paddingTop    = 10.0f,
                                                                                           .paddingRight  = 10.0f,
                                                                                           .paddingBottom = 10.0f,
                                                                                           .gapX          = 10.0f,
                                                                                           .gapY          = 10.0f
                                                                                       }
            );

            // Child 1: height 50
            ZHLN::Entity c1 = reg.Create(ZHLN::Components::UIRectComponent {.parentEntity = root, .height = 50.0f});

            // Child 2: height 50
            ZHLN::Entity c2 = reg.Create(ZHLN::Components::UIRectComponent {.parentEntity = root, .height = 50.0f});

            layoutSystem.ResolveLayouts(reg, {.width = 1920.0f, .height = 1080.0f});

            auto* r1 = reg.Get<ZHLN::Components::UIRectComponent>(c1);
            auto* r2 = reg.Get<ZHLN::Components::UIRectComponent>(c2);

            ZHLN::Test::ExpectTrue(r1 != nullptr && r2 != nullptr);
            if (r1 && r2) {
                // Child 1 starts at top padding: 10.0
                ZHLN::Test::ExpectEq(r1->computedAbsMinY, 10.0f);
                ZHLN::Test::ExpectEq(r1->computedAbsMaxY, 60.0f);

                // Child 2 starts at top padding (10) + c1 height (50) + gap (10) = 70.0
                ZHLN::Test::ExpectEq(r2->computedAbsMinY, 70.0f);
                ZHLN::Test::ExpectEq(r2->computedAbsMaxY, 120.0f);
            }

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<UILayoutTestSuite>();
}
