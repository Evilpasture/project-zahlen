// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <engine/system/UILayoutSystem.hpp>
#include <cstdio>
#include <expected>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Write a PPM (P6 / binary RGB) screenshot of all UIPanel rects as solid
// filled rectangles over a white background. Panels are colored by depth
// (hue mod 6 based on hierarchyDepth mod 6) so we can visually confirm
// positioning/sizing. Cheap - no GPU needed, CPU-only rasterisation of
// the already-computed Yoga layout rects.
void WritePanelsPpm(const char* path, ZHLN::ECS::Registry& reg,
                    unsigned vw, unsigned vh) {
    std::vector<std::uint8_t> pixels(static_cast<size_t>(vw) * vh * 3, 255);
    auto put = [&](unsigned x, unsigned y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        if (x >= vw || y >= vh) return;
        size_t i = (static_cast<size_t>(y) * vw + x) * 3;
        pixels[i + 0] = r; pixels[i + 1] = g; pixels[i + 2] = b;
    };
    auto fillRect = [&](float x0f, float y0f, float x1f, float y1f,
                        std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        int x0 = std::max(0, static_cast<int>(x0f));
        int y0 = std::max(0, static_cast<int>(y0f));
        int x1 = std::min(static_cast<int>(vw) - 1, static_cast<int>(x1f));
        int y1 = std::min(static_cast<int>(vh) - 1, static_cast<int>(y1f));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) put(x, y, r, g, b);
    };

    // Depth-based palette (root = dark, deeper = brighter)
    static constexpr std::uint8_t palette[6][3] = {
        {20, 30, 50}, {40, 80, 140}, {80, 140, 200}, {140, 180, 220},
        {210, 140, 90}, {200, 80, 80}
    };

    for (ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::UIPanelComponent>()) {
        auto* rect  = reg.Get<ZHLN::Components::UIRectComponent>(e);
        auto* panel = reg.Get<ZHLN::Components::UIPanelComponent>(e);
        if (rect == nullptr || panel == nullptr) continue;
        auto c = palette[rect->hierarchyDepth % 6];
        // Tint by panel alpha (cheap)
        std::uint8_t r = static_cast<std::uint8_t>(panel->color.GetX() * 255.0f * 0.6f + c[0] * 0.4f);
        std::uint8_t g = static_cast<std::uint8_t>(panel->color.GetY() * 255.0f * 0.6f + c[1] * 0.4f);
        std::uint8_t b = static_cast<std::uint8_t>(panel->color.GetZ() * 255.0f * 0.6f + c[2] * 0.4f);
        fillRect(rect->computedAbsMinX, rect->computedAbsMinY,
                 rect->computedAbsMaxX, rect->computedAbsMaxY, r, g, b);
    }

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", vw, vh);
    std::fwrite(pixels.data(), 1, pixels.size(), f);
    std::fclose(f);
}

} // namespace

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

        // ------------------------------------------------------------------
        // A centered anchored panel (anchor 0.5,0.5) with explicit width must
        // be centered horizontally and keep its explicit width at every
        // viewport size. This is the regression guard for the "panel stretches
        // on window resize" bug.
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> centered_anchored_panel_stays_centered_and_fixed_width() {
            for (unsigned vw : {640u, 1280u, 1920u}) {
                for (unsigned vh : {480u, 720u, 1080u}) {
                    ZHLN::ECS::Registry reg;
                    ZHLN::GUI::Context  gui(reg, 1);
                    ZHLN::Entity panel = gui.Panel("RenderSettings",
                        ZHLN::GUI::PanelConfig {
                            .width   = 400.0f,
                            .height  = 0.0f, // auto height
                            .x       = 0.0f,
                            .y       = 0.0f,
                            .padding = 14.0f,
                        },
                        [&]() -> void {
                            gui.Label("Title", ZHLN::GUI::LabelConfig {.height = 28.0f});
                        });

                    ZHLN::UILayoutSystem layout;
                    layout.ResolveLayouts(reg, {static_cast<float>(vw), static_cast<float>(vh)});

                    auto* rr = reg.Get<ZHLN::Components::UIRectComponent>(panel);
                    ZHLN::Test::ExpectTrue(rr != nullptr);
                    if (rr == nullptr) return {};

                    const float panelW = rr->computedAbsMaxX - rr->computedAbsMinX;
                    const float centerX = rr->computedAbsMinX + panelW * 0.5f;
                    const float vpCenter = static_cast<float>(vw) * 0.5f;

                    // Width preserved
                    ZHLN::Test::ExpectTrue(std::abs(panelW - 400.0f) < 1.0f);
                    // Centered (within 1px)
                    ZHLN::Test::ExpectTrue(std::abs(centerX - vpCenter) < 1.0f);
                }
            }
            return {};
        }

        // ------------------------------------------------------------------
        // Top-left anchored panel (anchor 0,0) must stay pinned to the
        // corner and keep its fixed size on resize.
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> top_left_anchored_panel_stays_pinned() {
            for (unsigned vw : {640u, 1920u}) {
                ZHLN::ECS::Registry reg;
                ZHLN::GUI::Context  gui(reg, 1);
                ZHLN::Entity panel = gui.Panel("P", ZHLN::GUI::PanelConfig {
                    .width   = 200.0f,
                    .height  = 120.0f,
                    .x       = 16.0f,
                    .y       = 16.0f,
                    .anchorMinX = 0.0f, .anchorMinY = 0.0f,
                    .anchorMaxX = 0.0f, .anchorMaxY = 0.0f,
                }, [] {});

                ZHLN::UILayoutSystem layout;
                layout.ResolveLayouts(reg, {static_cast<float>(vw), 720.0f});

                auto* rr = reg.Get<ZHLN::Components::UIRectComponent>(panel);
                ZHLN::Test::ExpectTrue(rr != nullptr);
                if (rr == nullptr) return {};
                ZHLN::Test::ExpectTrue(std::abs(rr->computedAbsMinX - 16.0f) < 1.0f);
                ZHLN::Test::ExpectTrue(std::abs(rr->computedAbsMinY - 16.0f) < 1.0f);
                ZHLN::Test::ExpectTrue(std::abs((rr->computedAbsMaxX - rr->computedAbsMinX) - 200.0f) < 1.0f);
                ZHLN::Test::ExpectTrue(std::abs((rr->computedAbsMaxY - rr->computedAbsMinY) - 120.0f) < 1.0f);
            }
            return {};
        }

        // ------------------------------------------------------------------
        // A panel anchored with anchorMinX=0, anchorMaxX=1 (full-width) must
        // stretch horizontally with the viewport, honoring left/right insets.
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> stretch_anchored_panel_fills_viewport_on_resize() {
            for (unsigned vw : {640u, 1280u, 1920u}) {
                ZHLN::ECS::Registry reg;
                ZHLN::GUI::Context  gui(reg, 1);
                const float inset = 20.0f;
                ZHLN::Entity panel = gui.Panel("P", ZHLN::GUI::PanelConfig {
                    .width   = inset,       // used as right-inset when stretching
                    .height  = 40.0f,
                    .x       = inset,       // left inset
                    .y       = 0.0f,
                    .anchorMinX = 0.0f, .anchorMinY = 0.0f,
                    .anchorMaxX = 1.0f, .anchorMaxY = 0.0f,
                }, [] {});

                ZHLN::UILayoutSystem layout;
                layout.ResolveLayouts(reg, {static_cast<float>(vw), 720.0f});

                auto* rr = reg.Get<ZHLN::Components::UIRectComponent>(panel);
                ZHLN::Test::ExpectTrue(rr != nullptr);
                if (rr == nullptr) return {};
                const float expectedW = static_cast<float>(vw) - 2 * inset;
                const float w = rr->computedAbsMaxX - rr->computedAbsMinX;
                ZHLN::Test::ExpectTrue(std::abs(w - expectedW) < 1.0f);
                ZHLN::Test::ExpectTrue(std::abs(rr->computedAbsMinX - inset) < 1.0f);
            }
            return {};
        }

        // ------------------------------------------------------------------
        // PPM raster smoke test. Writes three screenshots at three viewport
        // sizes into the build directory so a developer can open them and
        // visually confirm centering, sizing, and resize behaviour. This is
        // the CPU-side "golden image" the user asked for - no GPU required.
        // ------------------------------------------------------------------
        std::expected<void, ZHLN::Error> writes_ppm_screenshots_at_multiple_viewports() {
            struct Sz { unsigned w, h; const char* name; };
            for (const Sz& sz : {Sz{640, 480, "gui_layout_640x480.ppm"},
                                 Sz{1280, 720, "gui_layout_1280x720.ppm"},
                                 Sz{1920, 1080, "gui_layout_1920x1080.ppm"}}) {
                ZHLN::ECS::Registry reg;
                {
                    ZHLN::GUI::Context gui(reg, 1);
                    gui.Panel("Center", ZHLN::GUI::PanelConfig {
                        .width = 400.0f, .height = 0.0f,
                        .x = 0.0f, .y = 0.0f,
                        .gap     = 6.0f,
                        .padding = 14.0f,
                    }, [&]() -> void {
                        gui.Label("Centered Panel");
                        bool vsync = true;
                        gui.Checkbox("cb", "Vsync", vsync);
                    });
                    gui.Panel("TL", ZHLN::GUI::PanelConfig {
                        .width = 200.0f, .height = 100.0f,
                        .x = 16.0f, .y = 16.0f,
                        .anchorMinX = 0.0f, .anchorMinY = 0.0f,
                        .anchorMaxX = 0.0f, .anchorMaxY = 0.0f,
                    }, [] {});
                }
                ZHLN::UILayoutSystem layout;
                layout.ResolveLayouts(reg, {static_cast<float>(sz.w), static_cast<float>(sz.h)});
                WritePanelsPpm(sz.name, reg, sz.w, sz.h);
            }
            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunUILayoutSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<UILayoutTestSuite>();
}

