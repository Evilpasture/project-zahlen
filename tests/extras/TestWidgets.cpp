// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The widget layer (extensions/Widget): the rolling window a history is kept in,
// the fit that keeps a line inside its box, the scale and height a bar strip
// draws with, and where the card an element opens ends up.
//
// Headless: the registry-only GUI::Context constructor, no GPU, no window, no
// frame source -- the same walk the UI schema suite takes. What it is here to
// catch is the class of bug these widgets were extracted for: a projection that
// stops moving once its window fills, a fit that lets a line overflow the box it
// is drawn in, a scale that divides by zero on an idle row, and a card placed
// over the panel it belongs to.

#include "TestsFramework.hpp"
#include <Zahlen/ecs/ECS.hpp>
#include <Widget/Widget.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace Widget = ZHLN::GUI::Widget;

enum class WidgetTestError : uint8_t {
    RollingWindowFailed ZHLN_ANNOTATION(ZHLN::Description<"The rolling window did not hold or project its samples as specified."> {}) = 1,
    TextFitFailed       ZHLN_ANNOTATION(ZHLN::Description<"A line was fitted to its box wrongly, or a measurement fell back where it should not have."> {}),
    BarStripFailed      ZHLN_ANNOTATION(ZHLN::Description<"A bar strip's scale or bar height did not follow the style it was drawn with."> {}),
    HoverCardFailed     ZHLN_ANNOTATION(ZHLN::Description<"The hover card was not stateful, or was placed over the panel it belongs to."> {}),
};

struct WidgetTestSuite {
    struct Tests {
        // The invariant the frame-time chart got wrong: a window that has filled
        // must keep moving, and its newest sample must be the one that just
        // arrived -- whatever slot the ring's cursor happens to be on.
        std::expected<void, ZHLN::ErrorCode> rolling_window_holds_the_newest_samples_in_order() {
            static_assert(Widget::RollingWindow<float, 8>::kCapacity == 8, "a window's capacity is the number it was declared with");

            Widget::RollingWindow<float, 8> window;
            if (!ZHLN::Test::ExpectTrue(window.Empty())) {
                return std::unexpected(WidgetTestError::RollingWindowFailed);
            }
            ZHLN::Test::ExpectEq(window.Count(), size_t {0});

            for (int i = 1; i <= 8; ++i) {
                window.Push(static_cast<float>(i));
            }
            if (!ZHLN::Test::ExpectTrue(window.Full())) {
                return std::unexpected(WidgetTestError::RollingWindowFailed);
            }
            ZHLN::Test::ExpectEq(window.Count(), size_t {8});
            ZHLN::Test::ExpectEq(window.Newest(), 8.0f);
            ZHLN::Test::ExpectEq(window.Oldest(), 1.0f);
            ZHLN::Test::ExpectEq(window.At(0), 8.0f);
            ZHLN::Test::ExpectEq(window.At(7), 1.0f);

            // A full window drops its oldest to make room, and the ages renumber
            // with it.
            window.Push(9.0f);
            ZHLN::Test::ExpectEq(window.Count(), size_t {8});
            ZHLN::Test::ExpectEq(window.Newest(), 9.0f);
            ZHLN::Test::ExpectEq(window.Oldest(), 2.0f);

            // The newest four into a caller's strip: the newest sample is the
            // strip's last element, so a chart's rightmost bar is the frame that
            // just happened.
            std::array<float, 4> newest {};
            ZHLN::Test::ExpectEq(window.CopyNewest(std::span<float>(newest)), size_t {4});
            ZHLN::Test::ExpectEq(newest[0], 6.0f);
            ZHLN::Test::ExpectEq(newest[3], 9.0f);

            // A strip longer than the window is filled from its end, and the
            // slots before it are left alone -- a chart that has not filled
            // blanks its own bars rather than reading slots the window never
            // wrote.
            std::array<float, 12> strip {};
            strip.fill(-1.0f);
            ZHLN::Test::ExpectEq(window.CopyNewest(std::span<float>(strip)), size_t {8});
            ZHLN::Test::ExpectEq(strip[3], -1.0f);
            ZHLN::Test::ExpectEq(strip[4], 2.0f);
            ZHLN::Test::ExpectEq(strip[11], 9.0f);

            std::array<float, 3> oldest {};
            ZHLN::Test::ExpectEq(window.CopyOldest(std::span<float>(oldest)), size_t {3});
            ZHLN::Test::ExpectEq(oldest[0], 2.0f);
            ZHLN::Test::ExpectEq(oldest[2], 4.0f);

            // A window that has filled and keeps filling: every sample in the
            // strip has to move on. Indexed from the wrong end it repeats the
            // same 128 samples for the rest of the run -- a chart drawn
            // perfectly and frozen, which is what this widget was extracted from.
            Widget::RollingWindow<float, 128> chart {};
            std::array<float, 128>            bars {};
            for (int i = 0; i < 200; ++i) {
                chart.Push(static_cast<float>(i));
                chart.CopyNewest(std::span<float>(bars));
                if (bars.back() != static_cast<float>(i)) {
                    ZHLN::Test::ExpectEq(bars.back(), static_cast<float>(i));
                    return std::unexpected(WidgetTestError::RollingWindowFailed);
                }
            }
            ZHLN::Test::ExpectEq(bars.front(), 72.0f); // 200 samples, 128 of them held
            ZHLN::Test::ExpectEq(bars.back(), 199.0f);

            window.Clear();
            if (!ZHLN::Test::ExpectTrue(window.Empty())) {
                return std::unexpected(WidgetTestError::RollingWindowFailed);
            }
            ZHLN::Test::ExpectEq(window.Count(), size_t {0});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> fits_a_line_to_the_box_it_is_drawn_in() {
            // No atlas: the documented fallback ratio, which is the width of the
            // font this tree vendors -- 0.6 em, so 9.6 px at 16.
            ZHLN::Test::ExpectTrue(Widget::AdvancePerChar(nullptr, 16.0f) > 9.59f);
            ZHLN::Test::ExpectTrue(Widget::AdvancePerChar(nullptr, 16.0f) < 9.61f);
            ZHLN::Test::ExpectEq(Widget::AdvancePerChar(nullptr, 0.0f), 0.0f);

            // The atlas's own advance at the size the line will be drawn: 6 px
            // per glyph on a 20 px bake is 12 px at 40 and 3 px at 10.
            ZHLN::FontAtlas atlas {};
            atlas.fontSize       = 20.0f;
            atlas.firstCodepoint = 32;
            atlas.glyphCount     = 128;
            for (ZHLN::GlyphMetric& glyph: atlas.glyphs) {
                glyph.xadvance = 6.0f;
            }
            ZHLN::Test::ExpectEq(Widget::AdvancePerChar(&atlas, 40.0f), 12.0f);
            ZHLN::Test::ExpectEq(Widget::AdvancePerChar(&atlas, 10.0f), 3.0f);

            // An atlas with no glyphs in it is not a measurement.
            ZHLN::FontAtlas emptyAtlas {};
            ZHLN::Test::ExpectTrue(Widget::AdvancePerChar(&emptyAtlas, 10.0f) > 5.99f);
            ZHLN::Test::ExpectTrue(Widget::AdvancePerChar(&emptyAtlas, 10.0f) < 6.01f);

            // One measured size scales to the others, which is what a panel
            // drawing at several sizes off one measurement needs.
            ZHLN::Test::ExpectTrue(Widget::AdvanceAt(9.6f, 16.0f, 12.0f) > 7.19f);
            ZHLN::Test::ExpectTrue(Widget::AdvanceAt(9.6f, 16.0f, 12.0f) < 7.21f);
            ZHLN::Test::ExpectTrue(Widget::AdvanceAt(0.0f, 0.0f, 12.0f) > 7.19f);

            // 10 characters at 6 px is 60 px of ink, and the slack is one
            // character: a box that holds 11 holds the 10, and a box that holds
            // exactly 10 does not. The slack is what keeps a cut deliberate
            // instead of a wrap.
            ZHLN::Test::ExpectTrue(Widget::FitsWidth("abcdefghij", 66.0f, 6.0f));
            ZHLN::Test::ExpectFalse(Widget::FitsWidth("abcdefghij", 60.0f, 6.0f));
            ZHLN::Test::ExpectFalse(Widget::FitsWidth("abcdefghijk", 66.0f, 6.0f));

            // The cut: as much as fits, then the two dots -- nine characters in
            // the room of ten.
            ZHLN::Test::ExpectEq(Widget::FitToWidth("abcdefghijklmno", 60.0f, 6.0f), std::string("abcdefg.."));
            ZHLN::Test::ExpectEq(Widget::FitToWidth("abcdefghij", 66.0f, 6.0f), std::string("abcdefghij"));
            ZHLN::Test::ExpectEq(Widget::FitToWidth("abcdefghij", 0.0f, 6.0f), std::string("abcdefghij"));
            ZHLN::Test::ExpectEq(Widget::FitToWidth("abcdefghij", 60.0f, 0.0f), std::string("abcdefghij"));

            // The buffer form, which is the one a panel drawing rows every frame
            // uses: the same answer, no allocation, and it may cut a buffer in
            // place -- a row is formatted into the buffer it is cut back into.
            std::array<char, 32> buffer {};
            std::string_view     longLine = "abcdefghijklmno";
            std::copy(longLine.begin(), longLine.end(), buffer.begin());
            ZHLN::Test::ExpectEq(
                Widget::FitToWidthInto(std::string_view(buffer.data(), longLine.size()), 60.0f, 6.0f, buffer), std::string_view("abcdefg..")
            );

            // A whole line that fits, and a caller's buffer with no room for it.
            std::array<char, 32> roomy {};
            ZHLN::Test::ExpectEq(Widget::FitToWidthInto("abc", 60.0f, 6.0f, roomy), std::string_view("abc"));
            std::array<char, 4> tight {};
            ZHLN::Test::ExpectEq(Widget::FitToWidthInto("abcdefghijklmno", 60.0f, 6.0f, tight), std::string_view {});

            // A box with room for the dots and nothing else keeps the prefix it
            // can: a ".." with nothing in front of it is not a cut, it is a line
            // with no reading in it.
            std::array<char, 8> tiny {};
            ZHLN::Test::ExpectEq(Widget::FitToWidthInto("abcdefghij", 18.0f, 6.0f, tiny), std::string_view("ab"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> bar_strips_scale_by_the_rule_their_style_names() {
            Widget::BarStripStyle style {};
            style.height       = 20.0f;
            style.minBarHeight = 2.0f;

            // A row's own peak, with the floor that keeps an idle row from
            // filling every bar and reading as the busiest thing on the panel.
            style.scale     = Widget::BarScale::OwnPeak;
            style.peakFloor = 0.05f;
            const std::array<float, 3> quiet {0.01f, 0.02f, 0.03f};
            ZHLN::Test::ExpectEq(Widget::BarPeak(quiet, style), 0.05f);

            const std::array<float, 3> busy {0.01f, 4.0f, 0.5f};
            ZHLN::Test::ExpectEq(Widget::BarPeak(busy, style), 4.0f);
            ZHLN::Test::ExpectEq(Widget::BarHeight(4.0f, Widget::BarPeak(busy, style), style), 20.0f);
            ZHLN::Test::ExpectEq(Widget::BarHeight(2.0f, Widget::BarPeak(busy, style), style), 10.0f);
            ZHLN::Test::ExpectEq(Widget::BarHeight(0.0f, Widget::BarPeak(busy, style), style), 2.0f);
            ZHLN::Test::ExpectEq(Widget::BarHeight(-5.0f, Widget::BarPeak(busy, style), style), 2.0f);

            // A sample over the top clips rather than overflows, and a window
            // with nothing in it has a scale rather than a division by zero.
            ZHLN::Test::ExpectEq(Widget::BarHeight(8.0f, 4.0f, style), 20.0f);
            const std::array<float, 0> none {};
            style.peakFloor = 0.0f;
            ZHLN::Test::ExpectGt(Widget::BarPeak(none, style), 0.0f);

            // A shared scale, which is what a chart and a core grid need: one
            // value fills the height of every strip drawn with it.
            style.scale      = Widget::BarScale::Shared;
            style.sharedPeak = 100.0f;
            ZHLN::Test::ExpectEq(Widget::BarPeak(busy, style), 100.0f);
            ZHLN::Test::ExpectEq(Widget::BarHeight(50.0f, Widget::BarPeak(busy, style), style), 10.0f);
            style.sharedPeak = 0.0f;
            ZHLN::Test::ExpectGt(Widget::BarPeak(busy, style), 0.0f);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> hover_card_state_and_placement() {
            Widget::HoverState hover {};
            ZHLN::Test::ExpectFalse(Widget::HasHover(hover));

            // A line with no element to anchor it to is not a card.
            Widget::AddHoverLine(hover, "line");
            ZHLN::Test::ExpectFalse(Widget::HasHover(hover));

            Widget::SetHover(hover, "row-1");
            ZHLN::Test::ExpectEq(hover.id, std::string("row-1"));
            ZHLN::Test::ExpectFalse(Widget::HasHover(hover)); // an element with nothing to say
            Widget::AddHoverLine(hover, "line");
            ZHLN::Test::ExpectTrue(Widget::HasHover(hover));

            // The frame's lines are appended to the element's card, and an
            // element of its own is a card of its own.
            Widget::SetHover(hover, "row-2");
            Widget::AddHoverLine(hover, "other line");
            ZHLN::Test::ExpectEq(hover.lines.size(), size_t {1});
            Widget::SetHover(hover, "row-1"); // back to the element the card was about
            ZHLN::Test::ExpectEq(hover.lines.size(), size_t {0});
            Widget::AddHoverLine(hover, "first");
            Widget::SetHover(hover, "row-1"); // the same element: the line stays
            Widget::AddHoverLine(hover, "second");
            ZHLN::Test::ExpectEq(hover.lines.size(), size_t {2});

            Widget::ResetHover(hover);
            ZHLN::Test::ExpectFalse(Widget::HasHover(hover));

            // Placement, through a real Context: the card goes beside its panel
            // when the window has room for it there, and under its element when
            // it does not. Headless -- a registry-backed Context, no GPU.
            const ZHLN::Extent2D   viewport {.width = 800u, .height = 600u};
            Widget::HoverCardStyle style {};
            style.fontSize     = 11.0f;
            style.lineHeight   = 14.0f;
            style.lineGap      = 3.0f;
            style.padding      = 6.0f;
            style.anchorGap    = 4.0f;
            style.windowMargin = 8.0f;
            style.advance      = 6.0f; // so the width is arithmetic and not a font

            // Ten characters of ink, the character of slack the fit leaves, and
            // the padding on both sides.
            constexpr float kCardWidth  = (11.0f * 6.0f) + (6.0f * 2.0f);
            const float     kCardHeight = style.lineHeight + (style.padding * 2.0f);

            // The rule itself, as arithmetic: a panel with room to its left gets
            // the card beside it, level with the element and one gap clear of the
            // panel's own box.
            const ZHLN::GUI::Context::ElementRect adjacentPanel {.x = 400.0f, .y = 20.0f, .width = 200.0f, .height = 300.0f};
            const ZHLN::GUI::Context::ElementRect element {.x = 408.0f, .y = 28.0f, .width = 120.0f, .height = 16.0f};
            const Widget::HoverCardOffset         beside = Widget::HoverCardPlacement(kCardWidth, kCardHeight, element, &adjacentPanel, viewport, style);
            ZHLN::Test::ExpectEq(beside.x, -(kCardWidth + style.anchorGap));
            ZHLN::Test::ExpectEq(beside.y, 8.0f); // level with the element, in the panel

            // A panel against the window's edge has no room beside it, so the
            // card goes under the element instead -- inside the panel's column.
            const ZHLN::GUI::Context::ElementRect edgePanel {.x = 0.0f, .y = 20.0f, .width = 200.0f, .height = 300.0f};
            const Widget::HoverCardOffset         under = Widget::HoverCardPlacement(kCardWidth, kCardHeight, element, &edgePanel, viewport, style);
            ZHLN::Test::ExpectTrue(under.x >= 0.0f);
            ZHLN::Test::ExpectTrue(under.x < 1.0f); // floating, and inside the panel
            ZHLN::Test::ExpectEq(under.y, (element.y + element.height + style.anchorGap) - edgePanel.y);

            // No panel named: under the element, and never off the window -- a
            // card drawn at the bottom of the screen is lifted into the margin.
            const ZHLN::GUI::Context::ElementRect lowElement {.x = 40.0f, .y = 590.0f, .width = 60.0f, .height = 16.0f};
            const Widget::HoverCardOffset         floored = Widget::HoverCardPlacement(kCardWidth, kCardHeight, lowElement, nullptr, viewport, style);
            ZHLN::Test::ExpectEq(floored.x, 40.0f);
            ZHLN::Test::ExpectEq(floored.y, (600.0f - kCardHeight - style.windowMargin) - 590.0f);
            ZHLN::Test::ExpectEq(lowElement.y + floored.y, 600.0f - kCardHeight - style.windowMargin);

            // And through a real Context, drawn the way the panel draws it: the
            // offsets above and the rect the renderer ends up with have to agree.
            ZHLN::ECS::Registry registry;
            ZHLN::GUI::Context  ui(registry, viewport);
            for (const float panelX: {400.0f, 0.0f}) { // room beside the panel, then none
                const bool roomBeside = (panelX - kCardWidth - style.anchorGap) >= style.windowMargin;

                const auto drawPanel = [&](bool withCard) -> void {
                    ui.Box(
                        "widget-panel",
                        ZHLN::GUI::BoxConfig {
                            .width = {.fixed = 200.0f}, .height = {.fit = true}, .padding = 8.0f, .offsetX = panelX, .offsetY = 20.0f
                        },
                        [&]() -> void {
                            // The element the card belongs to, and the card as the
                            // panel's last child -- out of the layout flow, which
                            // is what keeps it off the panel's own rows.
                            ui.BeginBox("widget-row", ZHLN::GUI::BoxConfig {.width = {.fixed = 120.0f}, .height = {.fixed = 16.0f}});
                            ui.EndBox();
                            if (withCard) {
                                Widget::DrawHoverCard(ui, hover, viewport, style, "widget-panel");
                            }
                        }
                    );
                };

                // Frame 1: the panel and the row, so that the frame after has
                // their rectangles -- which is what a card is anchored to.
                ui.BeginFrame(0.016f);
                drawPanel(false);
                [[maybe_unused]] const ZHLN::UIDrawData panelFrame = ui.EndFrame();

                // Frame 2: the same layout, with the card.
                Widget::ResetHover(hover);
                Widget::SetHover(hover, "widget-row");
                Widget::AddHoverLine(hover, "1234567890");
                ui.BeginFrame(0.016f);
                drawPanel(true);
                // The card is a box like any other: if it did not reach the draw
                // list, there is nothing on screen to place.
                if (!ZHLN::Test::ExpectTrue(!ui.EndFrame().Empty())) {
                    return std::unexpected(WidgetTestError::HoverCardFailed);
                }

                // Frame 3: read what frame 2 laid out.
                ui.BeginFrame(0.016f);
                const auto card  = ui.GetLastFrameRect("widget-hover-card");
                const auto panel = ui.GetLastFrameRect("widget-panel");
                const auto row   = ui.GetLastFrameRect("widget-row");
                [[maybe_unused]] const ZHLN::UIDrawData readFrame = ui.EndFrame();

                if (!(ZHLN::Test::ExpectTrue(card.has_value()) && ZHLN::Test::ExpectTrue(panel.has_value()) && ZHLN::Test::ExpectTrue(row.has_value()))) {
                    return std::unexpected(WidgetTestError::HoverCardFailed);
                }

                // As wide as its longest line, whatever it is placed against.
                ZHLN::Test::ExpectEq(card->width, kCardWidth);

                // And where the rule said it would be: the offsets the widget
                // asks for, relative to the panel it was drawn inside. Both
                // branches of the rule go through this -- a panel at 400 has
                // room beside it, the one at the window's edge does not.
                const Widget::HoverCardOffset expected = Widget::HoverCardPlacement(kCardWidth, kCardHeight, *row, &*panel, viewport, style);
                ZHLN::Test::ExpectInRange(card->x, (panel->x + expected.x) - 0.01f, (panel->x + expected.x) + 0.01f);
                ZHLN::Test::ExpectInRange(card->y, (panel->y + expected.y) - 0.01f, (panel->y + expected.y) + 0.01f);
                if (roomBeside) {
                    // Which is to say: level with the row, one gap clear of the
                    // panel, out of the panel's own rows -- so the card cannot
                    // hand the hover to a row it lies over.
                    ZHLN::Test::ExpectInRange(card->y, row->y - 0.01f, row->y + 0.01f);
                    ZHLN::Test::ExpectInRange(card->x, (panel->x - kCardWidth - style.anchorGap) - 0.01f, (panel->x - kCardWidth - style.anchorGap) + 0.01f);
                } else {
                    // And with no room beside it: under the row, in the panel.
                    ZHLN::Test::ExpectInRange(card->x, panel->x, panel->x + 1.0f);
                    ZHLN::Test::ExpectInRange(card->y, (row->y + row->height + style.anchorGap) - 0.01f, (row->y + row->height + style.anchorGap) + 0.01f);
                }
            }
            return {};
        }
    };
};

auto RunWidgetsSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<WidgetTestSuite>();
}
