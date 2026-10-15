// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ===========================================================================
// The card that appears when the pointer is over something.
//
// A panel is a column of lines that each cost height, and a reading often has
// more to say than its line can hold -- the window behind the number, the clock,
// the reason a series is empty, what a button writes. That detail belongs one
// hover away rather than permanently on screen, which is what this widget is:
//
//   * the panel finishes drawing, and the element under the pointer -- a line, a
//     row, a cell, a button -- calls SetHover with its id and AddHoverLine per
//     line of detail;
//   * drawing the card is the *last* thing the panel does, so it is on top of
//     everything above it. A card drawn when it is decided sits under every line
//     after it, which is the bug this ordering exists to avoid;
//   * it is placed beside the element's own rectangle, in the space to the left
//     of the panel it belongs to. Not over the panel: hovering here is a
//     rectangle test against the previous frame's layout, so a card lying across
//     the panel's rows would hand the hover to whichever row it covered the
//     moment the pointer reached it. A window with no room beside the panel gets
//     the card under the element instead, and either placement is clamped inside
//     the window;
//   * it justifies against the element's rectangle from the *previous* frame,
//     because the frame being built has no finished layout to read yet. A card
//     therefore lags its element by one frame the first time it is hovered, and
//     not after that.
//
// The state is a plain struct the panel owns -- one of them per panel, reset at
// the top of the frame and read at the bottom -- so there is no lifetime to
// manage and no allocation while nothing is hovered.
// ===========================================================================

#include <Zahlen/Common.h>  // Extent2D: the window the card is kept inside
#include <Zahlen/gui/GUI.hpp>
#include <Jolt/Math/Vec4.h>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::GUI::Widget {

struct HoverCardStyle {
    float fontSize   = 11.0f;
    float lineHeight = 14.0f; // what one line of the card takes, gap included
    float lineGap    = 3.0f;  // between two lines; lineHeight is what the card is sized with

    // The per-character advance at `fontSize`, as the panel measures it (see
    // TextFit::AdvanceAt). 0 falls back to the ratio TextFit uses for a context
    // drawing with no font asset, which is right for the fonts this tree ships
    // and is why a panel that has measured its own font should pass it.
    float advance = 0.0f;
    float padding      = 6.0f;
    float anchorGap    = 4.0f; // between the card and the element (or the panel)
    float windowMargin = 8.0f; // the card never comes closer than this to an edge
    float cornerRadius = 4.0f;
    float maxWidth     = 0.0f; // 0 leaves the width to the longest line

    JPH::Vec4 ink     = {0.10f, 0.11f, 0.14f, 0.98f};
    JPH::Vec4 textInk = {0.78f, 0.84f, 0.92f, 1.00f};
};

// What the pointer is over, if anything, while a panel is being drawn.
struct HoverState {
    // The element's id: what the card is anchored to and what the panel is
    // told about. Empty means nothing is hovered.
    std::string id;

    // The card's lines, in the order they are written. Empty means the element
    // has been hovered but has nothing to say, which draws no card.
    std::vector<std::string> lines;
};

// Forgets the frame's card. Every panel calls this before it draws anything --
// the card is one per panel, not one per element.
void ResetHover(HoverState& state) noexcept;

// Starts a card for `id`, replacing whatever was hovered before it. Last one
// wins, which is what a row that contains cells wants: the innermost element
// that recognises the pointer is the element it is over.
void SetHover(HoverState& state, std::string_view id);

// Appends a line to the card being built. Without a SetHover first, this does
// nothing: a line has no element to anchor to.
void AddHoverLine(HoverState& state, std::string_view line);

// Whether a card is waiting to be drawn.
[[nodiscard]] auto HasHover(const HoverState& state) noexcept -> bool;

// Where the card is drawn, as an offset from the box that is open when it is
// drawn -- in the panel's own coordinates, which is what a box inside the panel
// is placed by.
struct HoverCardOffset {
    float x = 0.0f;
    float y = 0.0f;
};

// The placement rule on its own, so it can be asserted without a renderer: the
// card's size, its element's rectangle and its panel's (null for a card with no
// panel to sit beside), the window it has to stay inside, and the style. Level
// with the element and one gap to the left of the panel where the window has
// room for it there; under the element where it does not; clamped into the
// window's margin either way.
[[nodiscard]] auto HoverCardPlacement(
    float                                 width,
    float                                 height,
    const GUI::Context::ElementRect&      anchor,
    const GUI::Context::ElementRect*      panel,
    const ZHLN::Extent2D&                 viewport,
    const HoverCardStyle&                 style
) noexcept -> HoverCardOffset;

// Draws the frame's card, if there is one, floating out of the layout flow.
// `besideId` is the panel the element lives in: the card goes to the left of it
// when the window has room. Pass an empty id for a card with no panel to sit
// beside -- it is then placed under its element.
//
// Call this last, after everything else in the panel, and from inside the panel
// so the card is clipped and layered with it.
void DrawHoverCard(
    GUI::Context& ui, const HoverState& state, const ZHLN::Extent2D& viewport, const HoverCardStyle& style, std::string_view besideId = {}
);

} // namespace ZHLN::GUI::Widget
