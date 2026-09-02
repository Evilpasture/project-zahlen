// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/core/TestGUIPrimitives.cpp
//
// Behavioural tests for the widget primitives a UI designer needs on day one:
//
//   - ScrollBox: a fixed-height clipping viewport over natural-height content,
//     its scrollbar chrome, and the scroll state that drives both.
//   - Scrolling itself: content-extent measurement, wheel dispatch to the
//     innermost hovered scroller, smooth easing, and clamping.
//   - Clip-aware hit-testing: a row scrolled out of its viewport is invisible
//     AND inert — it must not swallow hover or clicks.
//   - ui.Image / ui.Icon: scale modes, sub-UV sprite regions, tile counts.
//   - The overlay layer: popups, dropdown menus and tooltips are parented
//     above the widget tree so no ancestor scissor can clip them.
//   - ui.Selectable / ui.TreeNode: selection state, double-click, and the
//     create/reclaim cycle of a tree branch's content.
//   - Automatic word wrapping, shared by the layout measure callback and the
//     renderer, including hard breaks for over-long words.
//
// Everything here is public API only (Zahlen/GUI.hpp + Zahlen/Components.hpp),
// matching the tests/ tree rule that engine internals stay out of tests.

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace {

using ZHLN::Entity;
using ZHLN::ECS::Registry;
namespace GUI = ZHLN::GUI;
using Comp    = ZHLN::Components;

// A synthetic font metric table with fully predictable numbers, so a test can
// state an expected break position instead of guessing at glyph shapes.
// Every glyph advances 10 units and inks an 8x12 box; at scale 1 a run of n
// characters therefore measures (n-1)*10 + 8 wide, and one line is 12 tall
// with a 36-unit baseline step.
[[nodiscard]] auto MakeTestFont() -> ZHLN::FontAtlas {
    ZHLN::FontAtlas font {};
    for (auto& g: font.glyphs) {
        g.x0       = 0.0f;
        g.y0       = 0.0f;
        g.x1       = 8.0f;
        g.y1       = 12.0f;
        g.xoff     = 0.0f;
        g.yoff     = -20.0f; // -> ink top at (yoff + 28) = 8, as AppendTextVertices does
        g.xadvance = 10.0f;
    }
    return font;
}

// Finds a cached child of `parent` by widget name, the way the engine's own
// compound widgets are keyed ("_sb_viewport", "_sel_label", ...).
[[nodiscard]] auto FindChildNamed(Registry& reg, Entity parent, std::string_view name) -> Entity {
    const auto* cache = reg.Get<Comp::UIChildCacheComponent>(parent);
    if (cache == nullptr) {
        return Entity::Null();
    }
    Entity found = Entity::Null();
    cache->children.ForEach([&](uint64_t, const Comp::UIChildCacheComponent::ChildRecord& rec) -> void {
        if (found != Entity::Null() || !reg.IsAlive(rec.entity)) {
            return;
        }
        if (const auto* n = reg.Get<Comp::NameComponent>(rec.entity)) {
            if (std::string_view(n->name) == name) {
                found = rec.entity;
            }
        }
    });
    return found;
}

// Gives a rect an explicit on-screen box, standing in for a layout pass.
void SetComputedRect(Registry& reg, Entity e, float x0, float y0, float x1, float y1) {
    reg.Patch<Comp::UIRectComponent>(e, [&](auto& r) -> auto {
        r.computedAbsMinX = x0;
        r.computedAbsMinY = y0;
        r.computedAbsMaxX = x1;
        r.computedAbsMaxY = y1;
    });
}

// The single overlay root every popup/tooltip is parented to.
[[nodiscard]] auto FindOverlayRoot(Registry& reg) -> Entity {
    for (Entity e: reg.GetEntitiesWith<Comp::UIRectComponent>()) {
        const auto* rect = reg.Get<Comp::UIRectComponent>(e);
        if (rect != nullptr && rect->hierarchyDepth == GUI::UI_OVERLAY_DEPTH) {
            return e;
        }
    }
    return Entity::Null();
}

// Publishes a pointer position through the same singleton the engine writes.
void SetPointer(Registry& reg, float x, float y) {
    auto settings = reg.GetEntitiesWith<Comp::InputStateComponent>();
    if (settings.empty()) {
        reg.Add<Comp::InputStateComponent>(reg.Create());
    }
    auto* state = reg.GetSingleton<Comp::InputStateComponent>();
    if (state != nullptr) {
        state->mouseX = x;
        state->mouseY = y;
    }
}

} // namespace

struct GUIPrimitivesTestSuite {
    struct Tests {
        // --------------------------------------------------------------
        // SCROLLBOX STRUCTURE
        // --------------------------------------------------------------
        auto scrollbox_builds_a_clipping_viewport_with_scroll_state() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   viewport = Entity::Null();
            Entity   track    = Entity::Null();
            Entity   root     = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                root = gui.ScrollBox("List", GUI::ScrollBoxConfig {.height = 120.0f, .scrollbarWidth = 8.0f}, [&]() -> void {
                    viewport = gui.GetCurrentParent();
                    gui.Label("Row 0");
                });
            }

            ZHLN::Test::ExpectTrue(root != Entity::Null());
            ZHLN::Test::ExpectTrue(viewport != Entity::Null());
            if (root == Entity::Null() || viewport == Entity::Null()) {
                return {};
            }

            // The scope handed to the closure is the viewport, not the root.
            ZHLN::Test::ExpectTrue(viewport != root);

            const auto* scroll = reg.Get<Comp::UIScrollComponent>(viewport);
            const auto* vrect  = reg.Get<Comp::UIRectComponent>(viewport);
            ZHLN::Test::ExpectTrue(scroll != nullptr);
            ZHLN::Test::ExpectTrue(vrect != nullptr);
            if (scroll == nullptr || vrect == nullptr) {
                return {};
            }

            // Clipping is what makes the overflow disappear instead of drawing
            // over the widgets below the box.
            ZHLN::Test::ExpectTrue(vrect->clipChildren);
            ZHLN::Test::ExpectEq(scroll->scrollY, 0.0f);

            track = FindChildNamed(reg, root, "_sb_track");
            ZHLN::Test::ExpectTrue(track != Entity::Null());
            if (track != Entity::Null()) {
                ZHLN::Test::ExpectTrue(FindChildNamed(reg, track, "_sb_thumb") != Entity::Null());
            }
            return {};
        }

        auto scrollbox_content_is_parented_under_the_viewport() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   viewport = Entity::Null();
            Entity   row      = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.ScrollBox("List", GUI::ScrollBoxConfig {.height = 100.0f}, [&]() -> void {
                    viewport = gui.GetCurrentParent();
                    row      = gui.Box("Row", GUI::BoxConfig {.height = 40.0f}, []() -> void {});
                });
            }

            ZHLN::Test::ExpectTrue(viewport != Entity::Null() && row != Entity::Null());
            if (viewport == Entity::Null() || row == Entity::Null()) {
                return {};
            }
            const auto* rowRect = reg.Get<Comp::UIRectComponent>(row);
            ZHLN::Test::ExpectTrue(rowRect != nullptr);
            if (rowRect == nullptr) {
                return {};
            }
            ZHLN::Test::ExpectEq(rowRect->parentEntity, viewport);
            return {};
        }

        auto scrollbox_entities_are_reused_across_frames() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   firstViewport = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.ScrollBox("List", GUI::ScrollBoxConfig {.height = 100.0f}, [&]() -> void {
                    firstViewport = gui.GetCurrentParent();
                    gui.Label("Row 0");
                });
            }

            Entity secondViewport = Entity::Null();
            {
                GUI::Context gui(reg, 2);
                gui.ScrollBox("List", GUI::ScrollBoxConfig {.height = 100.0f}, [&]() -> void {
                    secondViewport = gui.GetCurrentParent();
                    gui.Label("Row 0");
                });
            }

            ZHLN::Test::ExpectTrue(firstViewport != Entity::Null());
            ZHLN::Test::ExpectEq(secondViewport, firstViewport);
            return {};
        }

        // --------------------------------------------------------------
        // SCROLL MEASUREMENT AND WHEEL DISPATCH
        // --------------------------------------------------------------
        auto scroll_extents_are_measured_from_the_viewport_children() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   viewport = Entity::Null();
            Entity   rowA     = Entity::Null();
            Entity   rowB     = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.ScrollBox(
                    "List", GUI::ScrollBoxConfig {.height = 100.0f, .padding = 0.0f},
                    [&]() -> void {
                        viewport = gui.GetCurrentParent();
                        rowA     = gui.Box("A", GUI::BoxConfig {.height = 40.0f}, []() -> void {});
                        rowB     = gui.Box("B", GUI::BoxConfig {.height = 40.0f}, []() -> void {});
                    }
                );
            }
            ZHLN::Test::ExpectTrue(viewport != Entity::Null() && rowA != Entity::Null() && rowB != Entity::Null());
            if (viewport == Entity::Null() || rowA == Entity::Null() || rowB == Entity::Null()) {
                return {};
            }

            // Viewport is 100 tall; two 40-px rows with the default 4-px gap
            // are 84 of content, so nothing scrolls yet.
            SetComputedRect(reg, viewport, 0.0f, 0.0f, 200.0f, 100.0f);
            SetComputedRect(reg, rowA, 0.0f, 0.0f, 200.0f, 40.0f);
            SetComputedRect(reg, rowB, 0.0f, 44.0f, 200.0f, 84.0f);
            reg.Patch<Comp::UIFlexComponent>(viewport, [&](auto& f) -> auto { f.SetPadding(0.0f); });

            GUI::UpdateScrollExtents(reg);
            auto* scroll = reg.Get<Comp::UIScrollComponent>(viewport);
            ZHLN::Test::ExpectTrue(scroll != nullptr);
            if (scroll == nullptr) {
                return {};
            }
            ZHLN::Test::ExpectEq(scroll->contentHeight, 84.0f);
            ZHLN::Test::ExpectEq(scroll->maxScrollY, 0.0f);

            // Grow the content past the viewport: the range appears.
            SetComputedRect(reg, rowB, 0.0f, 44.0f, 200.0f, 244.0f);
            GUI::UpdateScrollExtents(reg);
            ZHLN::Test::ExpectEq(scroll->contentHeight, 244.0f);
            ZHLN::Test::ExpectEq(scroll->maxScrollY, 144.0f);
            return {};
        }

        auto wheel_scrolls_the_innermost_hovered_viewport() -> std::expected<void, ZHLN::Error> {
            Registry reg;

            // Two stacked scrollers, the inner one nested inside the outer, so
            // "innermost wins" is observable rather than assumed.
            Entity outer = reg.Create(
                Comp::NameComponent {.name = ZHLN::String64("outer")},
                Comp::UIRectComponent {.width = 200.0f, .height = 200.0f, .hierarchyDepth = 1, .clipChildren = true},
                Comp::UIFlexComponent {}, Comp::UIScrollComponent {.scrollSpeed = 10.0f, .smoothScroll = false}
            );
            Entity inner = reg.Create(
                Comp::NameComponent {.name = ZHLN::String64("inner")},
                Comp::UIRectComponent {.parentEntity = outer, .width = 100.0f, .height = 100.0f, .hierarchyDepth = 2, .clipChildren = true},
                Comp::UIScrollComponent {.scrollSpeed = 10.0f, .smoothScroll = false}
            );

            SetComputedRect(reg, outer, 0.0f, 0.0f, 200.0f, 200.0f);
            SetComputedRect(reg, inner, 50.0f, 50.0f, 150.0f, 150.0f);
            reg.Get<Comp::UIScrollComponent>(outer)->maxScrollY = 500.0f;
            reg.Get<Comp::UIScrollComponent>(inner)->maxScrollY = 500.0f;

            // Pointer over BOTH: the deeper one must take the wheel.
            bool consumed = GUI::ApplyScrollInput(reg, GUI::ScrollInput {.mouseX = 100.0f, .mouseY = 100.0f, .wheelDelta = -1.0f, .deltaTime = 0.016f});
            ZHLN::Test::ExpectTrue(consumed);
            ZHLN::Test::ExpectEq(reg.Get<Comp::UIScrollComponent>(inner)->scrollY, 10.0f);
            ZHLN::Test::ExpectEq(reg.Get<Comp::UIScrollComponent>(outer)->scrollY, 0.0f);

            // Pointer over the outer only: now the outer scrolls.
            GUI::ApplyScrollInput(reg, GUI::ScrollInput {.mouseX = 10.0f, .mouseY = 10.0f, .wheelDelta = -1.0f, .deltaTime = 0.016f});
            ZHLN::Test::ExpectEq(reg.Get<Comp::UIScrollComponent>(outer)->scrollY, 10.0f);
            ZHLN::Test::ExpectEq(reg.Get<Comp::UIScrollComponent>(inner)->scrollY, 10.0f);

            // Wheel up scrolls back down, and never past the top.
            GUI::ApplyScrollInput(reg, GUI::ScrollInput {.mouseX = 10.0f, .mouseY = 10.0f, .wheelDelta = 5.0f, .deltaTime = 0.016f});
            ZHLN::Test::ExpectEq(reg.Get<Comp::UIScrollComponent>(outer)->scrollY, 0.0f);
            return {};
        }

        auto smooth_scroll_eases_towards_its_target_and_clamps() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   sc = reg.Create(
                Comp::NameComponent {.name = ZHLN::String64("sc")},
                Comp::UIRectComponent {.width = 100.0f, .height = 100.0f, .clipChildren = true},
                Comp::UIScrollComponent {.scrollSpeed = 100.0f, .smoothSpeed = 10.0f, .smoothScroll = true}
            );
            SetComputedRect(reg, sc, 0.0f, 0.0f, 100.0f, 100.0f);

            auto* scroll  = reg.Get<Comp::UIScrollComponent>(sc);
            scroll->maxScrollY = 50.0f;

            GUI::ApplyScrollInput(reg, GUI::ScrollInput {.mouseX = 10.0f, .mouseY = 10.0f, .wheelDelta = -1.0f, .deltaTime = 0.1f});

            // One step of a 10/s ease over 0.1s moves 100% of the way, and the
            // target is clamped to the measured range, not the requested one.
            ZHLN::Test::ExpectEq(scroll->targetScrollY, 50.0f);
            ZHLN::Test::ExpectEq(scroll->scrollY, 50.0f);

            // Scrolling back up clamps at zero.
            GUI::ApplyScrollInput(reg, GUI::ScrollInput {.mouseX = 10.0f, .mouseY = 10.0f, .wheelDelta = 10.0f, .deltaTime = 0.1f});
            ZHLN::Test::ExpectEq(scroll->scrollY, 0.0f);
            return {};
        }

        auto content_clipped_out_of_a_viewport_is_inert() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   viewport = reg.Create(
                Comp::NameComponent {.name = ZHLN::String64("vp")},
                Comp::UIRectComponent {.width = 100.0f, .height = 100.0f, .clipChildren = true}, Comp::UIScrollComponent {}
            );
            Entity row = reg.Create(
                Comp::NameComponent {.name = ZHLN::String64("row")},
                Comp::UIRectComponent {.parentEntity = viewport, .width = 100.0f, .height = 40.0f}, Comp::UIButtonComponent {}
            );
            SetComputedRect(reg, viewport, 0.0f, 0.0f, 100.0f, 100.0f);

            // Row inside the viewport.
            SetComputedRect(reg, row, 0.0f, 20.0f, 100.0f, 60.0f);
            ZHLN::Test::ExpectTrue(GUI::IsPointVisible(reg, row, 50.0f, 40.0f));

            // Same row scrolled below the viewport's bottom edge: the rect
            // still contains the pointer's Y only if the clip is honoured.
            SetComputedRect(reg, row, 0.0f, 120.0f, 100.0f, 160.0f);
            ZHLN::Test::ExpectTrue(GUI::IsPointVisible(reg, row, 50.0f, 140.0f) == false);
            ZHLN::Test::ExpectTrue(GUI::IsPointVisible(reg, viewport, 50.0f, 140.0f) == false);
            return {};
        }

        // --------------------------------------------------------------
        // IMAGE / ICON
        // --------------------------------------------------------------
        auto image_carries_scale_mode_and_sub_uv_region() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   img = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                img = gui.Image(
                    "Portrait", ZHLN::TextureHandle {7},
                    GUI::ImageConfig {
                        .width  = 64.0f,
                        .height = 32.0f,
                        .mode   = ZHLN::ImageScaleMode::CropAspect,
                        .uv0x   = 0.25f,
                        .uv0y   = 0.5f,
                        .uv1x   = 0.75f,
                        .uv1y   = 1.0f,
                        .sourceWidth  = 128.0f,
                        .sourceHeight = 64.0f,
                    }
                );
            }

            const auto* image = reg.Get<Comp::UIImageComponent>(img);
            ZHLN::Test::ExpectTrue(image != nullptr);
            if (image == nullptr) {
                return {};
            }
            ZHLN::Test::ExpectEq(image->texture, ZHLN::TextureHandle {7});
            ZHLN::Test::ExpectEq(image->mode, ZHLN::ImageScaleMode::CropAspect);
            ZHLN::Test::ExpectEq(image->uv0x, 0.25f);
            ZHLN::Test::ExpectEq(image->uv1y, 1.0f);
            ZHLN::Test::ExpectEq(image->sourceWidth, 128.0f);

            // An image is its own primitive: no panel quad behind it.
            ZHLN::Test::ExpectTrue(reg.Get<Comp::UIPanelComponent>(img) == nullptr);
            return {};
        }

        auto image_geometry_follows_the_scale_mode() -> std::expected<void, ZHLN::Error> {
            Comp::UIRectComponent rect {};
            rect.computedAbsMinX = 0.0f;
            rect.computedAbsMinY = 0.0f;
            rect.computedAbsMaxX = 100.0f;
            rect.computedAbsMaxY = 50.0f;

            Comp::UIImageComponent img {};
            img.mode         = ZHLN::ImageScaleMode::Stretch;
            img.sourceWidth  = 200.0f;
            img.sourceHeight = 100.0f;

            // Stretch and the aspect modes all emit one quad.
            ZHLN::Test::ExpectEq(GUI::CountImageVertices(rect, img), 6u);
            img.mode = ZHLN::ImageScaleMode::FitAspect;
            ZHLN::Test::ExpectEq(GUI::CountImageVertices(rect, img), 6u);
            img.mode = ZHLN::ImageScaleMode::CropAspect;
            ZHLN::Test::ExpectEq(GUI::CountImageVertices(rect, img), 6u);

            // Tile repeats at the source size: a 200x100 sprite over a
            // 100x50 rect is cropped to a single tile.
            img.mode = ZHLN::ImageScaleMode::Tile;
            ZHLN::Test::ExpectEq(GUI::CountImageVertices(rect, img), 6u);

            // A 25x25 sprite tiles 4x2 over the same rect.
            img.sourceWidth  = 25.0f;
            img.sourceHeight = 25.0f;
            ZHLN::Test::ExpectEq(GUI::CountImageVertices(rect, img), 48u);

            // A fully transparent image emits nothing at all.
            img.tint = {1.0f, 1.0f, 1.0f, 0.0f};
            ZHLN::Test::ExpectEq(GUI::CountImageVertices(rect, img), 0u);
            return {};
        }

        auto fit_aspect_shrinks_the_quad_inside_its_rect() -> std::expected<void, ZHLN::Error> {
            Comp::UIRectComponent rect {};
            rect.computedAbsMaxX = 100.0f;
            rect.computedAbsMaxY = 50.0f;

            Comp::UIImageComponent img {};
            img.mode         = ZHLN::ImageScaleMode::FitAspect;
            img.sourceWidth  = 200.0f; // 2:1 into a 2:1 rect -> exact fit
            img.sourceHeight = 100.0f;

            std::array<ZHLN::VertexPosition, 6>   pos {};
            std::array<ZHLN::VertexAttributes, 6> attr {};
            const uint32_t written = GUI::AppendImageVertices(pos.data(), attr.data(), rect, img);
            ZHLN::Test::ExpectEq(written, 6u);
            if (written != 6) {
                return {};
            }

            float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
            for (const auto& p: pos) {
                minX = std::min(minX, p.position[0]);
                maxX = std::max(maxX, p.position[0]);
                minY = std::min(minY, p.position[1]);
                maxY = std::max(maxY, p.position[1]);
            }
            ZHLN::Test::ExpectEq(maxX - minX, 100.0f);
            ZHLN::Test::ExpectEq(maxY - minY, 50.0f);

            // A 1:1 source into the same 2:1 rect is letterboxed to 50x50.
            img.sourceWidth = 100.0f;
            const uint32_t written2 = GUI::AppendImageVertices(pos.data(), attr.data(), rect, img);
            ZHLN::Test::ExpectEq(written2, 6u);
            minX = 1e9f;
            maxX = -1e9f;
            for (const auto& p: pos) {
                minX = std::min(minX, p.position[0]);
                maxX = std::max(maxX, p.position[0]);
            }
            ZHLN::Test::ExpectEq(maxX - minX, 50.0f);
            return {};
        }

        // --------------------------------------------------------------
        // OVERLAY LAYER
        // --------------------------------------------------------------
        auto popup_is_parented_above_the_widget_tree() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   owner  = Entity::Null();
            Entity   popup  = Entity::Null();
            Entity   child  = Entity::Null();
            {
                GUI::Context gui(reg, 1);
                gui.Panel(
                    "Clipped", GUI::PanelConfig {.width = 200.0f, .height = 100.0f, .clipChildren = true},
                    [&]() -> void {
                        owner = gui.Button("Owner", "Owner", []() -> void {});
                        gui.Popup(
                            owner, GUI::PopupConfig {.width = 120.0f},
                            [&]() -> void {
                                popup = gui.GetCurrentParent();
                                child = gui.Box("Item", GUI::BoxConfig {.height = 20.0f}, []() -> void {});
                            }
                        );
                    }
                );
            }

            ZHLN::Test::ExpectTrue(owner != Entity::Null() && popup != Entity::Null() && child != Entity::Null());
            if (owner == Entity::Null() || popup == Entity::Null() || child == Entity::Null()) {
                return {};
            }

            // The popup is NOT a descendant of its owner: that is the whole
            // point, since the owner's panel clips its children.
            ZHLN::Test::ExpectTrue(popup != owner);
            const auto* popupRect = reg.Get<Comp::UIRectComponent>(popup);
            const auto* ownerRect = reg.Get<Comp::UIRectComponent>(owner);
            ZHLN::Test::ExpectTrue(popupRect != nullptr && ownerRect != nullptr);
            if (popupRect == nullptr || ownerRect == nullptr) {
                return {};
            }
            ZHLN::Test::ExpectTrue(popupRect->parentEntity != owner);

            // Its parent is the overlay root, and the overlay root has no
            // parent at all — that is what puts the popup outside every
            // ancestor scissor the renderer propagates.
            Entity overlay = FindOverlayRoot(reg);
            ZHLN::Test::ExpectTrue(overlay != Entity::Null());
            if (overlay != Entity::Null()) {
                ZHLN::Test::ExpectEq(popupRect->parentEntity, overlay);
                const auto* overlayRect = reg.Get<Comp::UIRectComponent>(overlay);
                ZHLN::Test::ExpectTrue(overlayRect != nullptr);
                if (overlayRect != nullptr) {
                    ZHLN::Test::ExpectTrue(overlayRect->parentEntity == Entity::Null());
                    ZHLN::Test::ExpectTrue(overlayRect->clipChildren == false);
                }
            }

            // It sits on the overlay layer, far above any real nesting depth.
            ZHLN::Test::ExpectEq(popupRect->hierarchyDepth, GUI::UI_OVERLAY_DEPTH + 1);
            ZHLN::Test::ExpectTrue(popupRect->hierarchyDepth > ownerRect->hierarchyDepth);

            // And it remembers who owns it, which is how the interaction pass
            // tells "clicked inside the menu" from "clicked outside".
            const auto* pop = reg.Get<Comp::UIPopupComponent>(popup);
            ZHLN::Test::ExpectTrue(pop != nullptr);
            if (pop != nullptr) {
                ZHLN::Test::ExpectEq(pop->owner, owner);
            }

            const auto* childRect = reg.Get<Comp::UIRectComponent>(child);
            ZHLN::Test::ExpectTrue(childRect != nullptr);
            if (childRect != nullptr) {
                ZHLN::Test::ExpectEq(childRect->parentEntity, popup);
            }
            return {};
        }

        auto dropdown_menu_lives_on_the_overlay_layer() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            int      selected = 0;
            Entity   dropdown = Entity::Null();
            constexpr std::array<std::string_view, 3> kOptions = {"One", "Two", "Three"};

            // Frame 1: open the dropdown.
            {
                GUI::Context gui(reg, 1);
                gui.Panel(
                    "Panel", GUI::PanelConfig {.width = 200.0f, .height = 80.0f, .clipChildren = true},
                    [&]() -> void { dropdown = gui.Dropdown("Quality", "Quality", selected, std::span<const std::string_view>(kOptions)); }
                );
                // Simulate the interaction pass: a click landed on the header.
                if (auto* btn = reg.Get<Comp::UIButtonComponent>(dropdown)) {
                    btn->Set(ZHLN::UIButton::Clicked, true);
                }
            }
            // Frame 2: the click from frame 1 is consumed here, so the menu is
            // built in this frame.
            Entity optionEntity = Entity::Null();
            {
                GUI::Context gui(reg, 2);
                gui.Panel(
                    "Panel", GUI::PanelConfig {.width = 200.0f, .height = 80.0f, .clipChildren = true},
                    [&]() -> void {
                        dropdown = gui.Dropdown("Quality", "Quality", selected, std::span<const std::string_view>(kOptions));
                        Entity overlay = FindOverlayRoot(reg);
                        if (overlay != Entity::Null()) {
                            if (Entity popup = FindChildNamed(reg, overlay, "_ui_popup"); popup != Entity::Null()) {
                                optionEntity = FindChildNamed(reg, popup, "Quality_opt0");
                            }
                        }
                    }
                );
            }

            ZHLN::Test::ExpectTrue(dropdown != Entity::Null());
            const auto* dd = reg.Get<Comp::UIDropdownComponent>(dropdown);
            ZHLN::Test::ExpectTrue(dd != nullptr);
            if (dd == nullptr) {
                return {};
            }
            ZHLN::Test::ExpectTrue(dd->expanded);
            ZHLN::Test::ExpectTrue(optionEntity != Entity::Null());
            if (optionEntity == Entity::Null()) {
                return {};
            }

            // The option row is on the overlay, not under the clipped panel.
            const auto* optRect = reg.Get<Comp::UIRectComponent>(optionEntity);
            ZHLN::Test::ExpectTrue(optRect != nullptr);
            if (optRect == nullptr) {
                return {};
            }
            ZHLN::Test::ExpectTrue(optRect->hierarchyDepth > GUI::UI_OVERLAY_DEPTH);
            ZHLN::Test::ExpectTrue(optRect->parentEntity != dropdown);
            return {};
        }

        // --------------------------------------------------------------
        // SELECTABLE / TREENODE
        // --------------------------------------------------------------
        auto selectable_toggles_on_click() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            bool     selected = false;
            int      clicks   = 0;
            Entity   row      = Entity::Null();

            {
                GUI::Context gui(reg, 1);
                row = gui.Selectable(
                    "Row1", "Row 1", selected, GUI::SelectableConfig {},
                    [&](bool nowSelected) -> void {
                        ++clicks;
                        ZHLN::Test::ExpectTrue(nowSelected);
                    }
                );
            }
            ZHLN::Test::ExpectTrue(row != Entity::Null());
            ZHLN::Test::ExpectTrue(selected == false);
            ZHLN::Test::ExpectEq(clicks, 0);

            // Simulate the interaction pass reporting a click, then rebuild.
            if (auto* btn = reg.Get<Comp::UIButtonComponent>(row)) {
                btn->Set(ZHLN::UIButton::Clicked, true);
            }
            {
                GUI::Context gui(reg, 2);
                gui.Selectable("Row1", "Row 1", selected, GUI::SelectableConfig {}, [&](bool) -> void { ++clicks; });
            }
            ZHLN::Test::ExpectTrue(selected);
            ZHLN::Test::ExpectEq(clicks, 1);

            // The row shows its selected state through the panel colour.
            const auto* panel = reg.Get<Comp::UIPanelComponent>(row);
            const auto  cfg   = GUI::SelectableConfig {};
            ZHLN::Test::ExpectTrue(panel != nullptr);
            if (panel != nullptr) {
                ZHLN::Test::ExpectEq(panel->color.GetX(), cfg.selectedColor.GetX());
            }
            return {};
        }

        auto selectable_reports_a_double_click_inside_the_frame_window() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            bool     selected     = false;
            int      singles      = 0;
            int      doubles      = 0;
            Entity   row          = Entity::Null();
            const auto cfg        = GUI::SelectableConfig {.doubleClickSpan = 18};

            auto ClickAt = [&](uint64_t frame) -> void {
                if (auto* btn = reg.Get<Comp::UIButtonComponent>(row)) {
                    btn->Set(ZHLN::UIButton::Clicked, true);
                }
                GUI::Context gui(reg, frame);
                gui.Selectable(
                    "Row", "Row", selected, cfg, [&](bool) -> void { ++singles; }, [&]() -> void { ++doubles; }
                );
            };

            {
                GUI::Context gui(reg, 1);
                row = gui.Selectable("Row", "Row", selected, cfg, [&](bool) -> void { ++singles; }, [&]() -> void { ++doubles; });
            }

            ClickAt(2);  // first click of the pair
            ZHLN::Test::ExpectEq(doubles, 0);
            ClickAt(10); // second click, 8 frames later -> inside the window
            ZHLN::Test::ExpectEq(doubles, 1);

            ClickAt(50); // first click of a new pair
            ClickAt(90); // 40 frames later -> too slow, stays a single click
            ZHLN::Test::ExpectEq(doubles, 1);
            return {};
        }

        auto treenode_creates_and_reclaims_its_content() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            bool     open    = true;
            Entity   node    = Entity::Null();
            Entity   content = Entity::Null();

            {
                GUI::Context gui(reg, 1);
                node = gui.TreeNode(
                    "Branch", "Branch", open,
                    [&]() -> void { content = gui.Box("Leaf", GUI::BoxConfig {.height = 20.0f}, []() -> void {}); }
                );
            }
            ZHLN::Test::ExpectTrue(node != Entity::Null() && content != Entity::Null());
            ZHLN::Test::ExpectTrue(reg.IsAlive(content));

            // Close the branch: the click toggles `open`, and the next frame's
            // build must not recreate the content box.
            if (auto* btn = reg.Get<Comp::UIButtonComponent>(node)) {
                btn->Set(ZHLN::UIButton::Clicked, true);
            }
            {
                GUI::Context gui(reg, 2);
                gui.TreeNode("Branch", "Branch", open, [&]() -> void { content = gui.Box("Leaf", GUI::BoxConfig {.height = 20.0f}, []() -> void {}); });
            }
            ZHLN::Test::ExpectTrue(open == false);
            ZHLN::Test::ExpectTrue(FindChildNamed(reg, node, "Branch_children") == Entity::Null());
            return {};
        }

        auto treenode_double_click_activates_without_closing_the_branch() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            bool     open      = false;
            int      activates = 0;
            Entity   node      = Entity::Null();
            GUI::TreeNodeConfig cfg {};
            cfg.row.doubleClickSpan = 18;

            auto BuildAt = [&](uint64_t frame) -> void {
                GUI::Context gui(reg, frame);
                node = gui.TreeNode(
                    "Branch", "Branch", open,
                    [&]() -> void { gui.Box("Leaf", GUI::BoxConfig {.height = 20.0f}, []() -> void {}); }, cfg,
                    [&]() -> void { ++activates; }
                );
            };

            auto Click = [&]() -> void {
                if (auto* btn = reg.Get<Comp::UIButtonComponent>(node)) {
                    btn->Set(ZHLN::UIButton::Clicked, true);
                }
            };

            BuildAt(1);
            ZHLN::Test::ExpectTrue(open == false);

            // First click of the pair: the branch opens, nothing activates.
            Click();
            BuildAt(2);
            ZHLN::Test::ExpectTrue(open);
            ZHLN::Test::ExpectEq(activates, 0);

            // Second click four frames later: activation fires, and the
            // branch stays open instead of toggling shut under the callback.
            Click();
            BuildAt(6);
            ZHLN::Test::ExpectEq(activates, 1);
            ZHLN::Test::ExpectTrue(open);
            ZHLN::Test::ExpectTrue(FindChildNamed(reg, node, "Branch_children") != Entity::Null());

            // The row still reports itself as open — the skipped toggle must
            // not leave the row's own selected flag flipped behind it.
            const auto* sel = reg.Get<Comp::UISelectableComponent>(node);
            const auto* panel = reg.Get<Comp::UIPanelComponent>(node);
            ZHLN::Test::ExpectTrue(sel != nullptr && panel != nullptr);
            if (sel != nullptr) {
                ZHLN::Test::ExpectTrue(sel->selected);
            }
            if (panel != nullptr) {
                // Nothing is hovered here, so an open branch paints its
                // selected colour — hover would upgrade it to activeColor.
                ZHLN::Test::ExpectEq(panel->color.GetX(), cfg.row.selectedColor.GetX());
            }

            // A lone click still closes the branch.
            Click();
            BuildAt(40);
            ZHLN::Test::ExpectTrue(open == false);
            ZHLN::Test::ExpectEq(activates, 1);
            return {};
        }

        // --------------------------------------------------------------
        // TOOLTIPS
        // --------------------------------------------------------------
        auto tooltip_appears_after_the_hover_delay() -> std::expected<void, ZHLN::Error> {
            Registry reg;
            Entity   button = Entity::Null();
            const auto cfg  = GUI::TooltipConfig {.delayFrames = 5};

            auto BuildAt = [&](uint64_t frame, bool hovered) -> Entity {
                Entity tooltip = Entity::Null();
                GUI::Context gui(reg, frame);
                gui.Panel(
                    "P", GUI::PanelConfig {.width = 200.0f, .height = 100.0f},
                    [&]() -> void {
                        button = gui.Button("Save", "Save", []() -> void {});
                        // Mirror the interaction pass, which rewrites the
                        // Hovered flag on every button each frame: a widget
                        // that is no longer under the pointer must not keep
                        // last frame's flag.
                        if (auto* btn = reg.Get<Comp::UIButtonComponent>(button)) {
                            btn->Set(ZHLN::UIButton::Hovered, hovered);
                        }
                        tooltip = gui.Tooltip("Writes the scene to disk", cfg);
                    }
                );
                return tooltip;
            };

            ZHLN::Test::ExpectTrue(BuildAt(1, false) == Entity::Null());
            ZHLN::Test::ExpectTrue(BuildAt(2, true) == Entity::Null());  // hover starts here
            ZHLN::Test::ExpectTrue(BuildAt(4, true) == Entity::Null());  // still inside the delay

            // The engine publishes the pointer through InputStateComponent, and
            // the bubble hangs off it rather than off the owner's rect.
            SetPointer(reg, 300.0f, 200.0f);

            Entity shown = BuildAt(8, true);                             // delay elapsed
            ZHLN::Test::ExpectTrue(shown != Entity::Null());

            if (shown != Entity::Null()) {
                const auto* rect = reg.Get<Comp::UIRectComponent>(shown);
                ZHLN::Test::ExpectTrue(rect != nullptr);
                if (rect != nullptr) {
                    ZHLN::Test::ExpectEq(rect->hierarchyDepth, GUI::UI_OVERLAY_DEPTH + 1);
                    ZHLN::Test::ExpectTrue(rect->width > 0.0f && rect->height > 0.0f);
                    ZHLN::Test::ExpectEq(rect->x, 300.0f + cfg.offsetX);
                    ZHLN::Test::ExpectEq(rect->y, 200.0f + cfg.offsetY);
                }
                ZHLN::Test::ExpectTrue(FindChildNamed(reg, shown, "_ui_tooltip_text") != Entity::Null());
            }

            // Moving off the widget drops it again (nothing rebuilds it, so
            // the overlay sweep reclaims it at frame end).
            ZHLN::Test::ExpectTrue(BuildAt(9, false) == Entity::Null());
            ZHLN::Test::ExpectTrue(reg.GetEntitiesWith<Comp::UITooltipComponent>().size() >= 1);
            return {};
        }

        // --------------------------------------------------------------
        // WORD WRAPPING
        // --------------------------------------------------------------
        auto text_wraps_at_word_boundaries() -> std::expected<void, ZHLN::Error> {
            const auto font = MakeTestFont();

            ZHLN::FixedString<256> out;
            const uint32_t         lines = GUI::WrapTextInto(font, "aa bb cc", 1.0f, 25.0f, out);

            // Each word is 18 wide, two words joined are 48, so every word
            // gets a line of its own at a 25-unit width.
            ZHLN::Test::ExpectEq(lines, 3u);
            ZHLN::Test::ExpectTrue(std::string_view(out) == "aa\nbb\ncc");

            // A generous width keeps the text on one line, untouched.
            const uint32_t oneLine = GUI::WrapTextInto(font, "aa bb cc", 1.0f, 500.0f, out);
            ZHLN::Test::ExpectEq(oneLine, 1u);
            ZHLN::Test::ExpectTrue(std::string_view(out) == "aa bb cc");

            // maxWidth <= 0 disables wrapping entirely.
            const uint32_t noWrap = GUI::WrapTextInto(font, "aa bb cc", 1.0f, 0.0f, out);
            ZHLN::Test::ExpectEq(noWrap, 1u);
            ZHLN::Test::ExpectTrue(std::string_view(out) == "aa bb cc");
            return {};
        }

        auto wrapping_preserves_explicit_line_breaks() -> std::expected<void, ZHLN::Error> {
            const auto font = MakeTestFont();

            ZHLN::FixedString<256> out;
            const uint32_t         lines = GUI::WrapTextInto(font, "aa\nbb cc", 1.0f, 25.0f, out);
            ZHLN::Test::ExpectEq(lines, 3u);
            ZHLN::Test::ExpectTrue(std::string_view(out) == "aa\nbb\ncc");

            // A blank source line still occupies a row.
            const uint32_t blank = GUI::WrapTextInto(font, "aa\n\nbb", 1.0f, 500.0f, out);
            ZHLN::Test::ExpectEq(blank, 3u);
            ZHLN::Test::ExpectTrue(std::string_view(out) == "aa\n\nbb");
            return {};
        }

        auto overlong_words_are_hard_broken() -> std::expected<void, ZHLN::Error> {
            const auto font = MakeTestFont();

            ZHLN::FixedString<256> out;
            const uint32_t         lines = GUI::WrapTextInto(font, "aaaaa", 1.0f, 25.0f, out);

            // Two characters fit per 25-unit line (18 wide), so a five
            // character word becomes "aa" / "aa" / "a" rather than running
            // horizontally out of its container.
            ZHLN::Test::ExpectEq(lines, 3u);
            ZHLN::Test::ExpectTrue(std::string_view(out) == "aa\naa\na");
            return {};
        }

        auto wrapped_text_measures_by_whole_lines() -> std::expected<void, ZHLN::Error> {
            const auto font = MakeTestFont();

            const GUI::TextBounds one  = GUI::MeasureWrappedTextBounds(font, "aa bb", 1.0f, 500.0f);
            const GUI::TextBounds many = GUI::MeasureWrappedTextBounds(font, "aa bb", 1.0f, 25.0f);

            // Same words, narrower container: the width shrinks to one word
            // and the height grows by a full line step.
            ZHLN::Test::ExpectTrue(many.width() < one.width());
            ZHLN::Test::ExpectEq(many.height() - one.height(), GUI::TextLineHeight(1.0f));

            // Explicit breaks measure the same way, which is what keeps the
            // Yoga measure callback and the renderer in agreement.
            const GUI::TextBounds broken = GUI::MeasureTextBounds(font, "aa\nbb", 1.0f);
            ZHLN::Test::ExpectEq(broken.height(), many.height());
            return {};
        }

        auto multiline_bounds_are_not_the_width_of_every_line_joined() -> std::expected<void, ZHLN::Error> {
            const auto font = MakeTestFont();

            const GUI::TextBounds joined = GUI::MeasureTextBounds(font, "aaaa", 1.0f);
            const GUI::TextBounds broken = GUI::MeasureTextBounds(font, "aa\naa", 1.0f);

            ZHLN::Test::ExpectTrue(broken.width() < joined.width());
            ZHLN::Test::ExpectEq(broken.height(), GUI::TextLineHeight(1.0f) + (joined.height()));
            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which aggregates
// every suite in this directory through Runner::RunDeferred.
auto RunGUIPrimitivesSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<GUIPrimitivesTestSuite>();
}
