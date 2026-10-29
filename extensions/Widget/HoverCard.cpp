// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Widget/HoverCard.hpp>
#include <Widget/TextFit.hpp>
#include <algorithm>
#include <format>
#include <optional>

namespace ZHLN::GUI::Widget {

namespace {

// The card's own box. One card per frame, so one id: a name would collide with a
// panel's element ids for no reason.
constexpr const char* kCardId = "widget-hover-card";

// A hair off zero, because the layout is what decides whether a box floats: any
// non-zero offset does, and a card that is meant to be in flow is not a card.
constexpr float kFloatingEpsilon = 0.5f;

// One line's advance: what the caller measured, or the ratio TextFit falls back
// to when nobody has.
auto CardAdvance(const HoverCardStyle& style) noexcept -> float {
    return style.advance > 0.0f ? style.advance : AdvancePerChar(nullptr, style.fontSize);
}

} // namespace

void ResetHover(HoverState& state) noexcept {
    state.id.clear();
    state.lines.clear();
}

void SetHover(HoverState& state, std::string_view id) {
    if (id.empty()) {
        ResetHover(state);
        return;
    }
    if (state.id != id) {
        state.lines.clear(); // a different element is a different card
    }
    state.id.assign(id);
}

void AddHoverLine(HoverState& state, std::string_view line) {
    if (state.id.empty()) {
        return; // nothing to anchor a line to
    }
    state.lines.emplace_back(line);
}

auto HasHover(const HoverState& state) noexcept -> bool {
    return !state.id.empty() && !state.lines.empty();
}

auto HoverCardPlacement(
    float width, float height, const GUI::ElementRect& anchor, const GUI::ElementRect* panel, const ZHLN::Extent2D& viewport,
    const HoverCardStyle& style
) noexcept -> HoverCardOffset {
    const float windowWidth   = static_cast<float>(viewport.width);
    const float windowHeight  = static_cast<float>(viewport.height);
    HoverCardOffset where {};

    if (panel != nullptr) {
        // The panel the element lives in decides which side of it the card goes:
        // level with the element, in the space beside that panel. Both offsets
        // are relative to the panel's own box, which is what the card hangs from.
        where.x = panel->x - width - style.anchorGap - panel->x; // to the panel's left
        where.y = anchor.y - panel->y;

        if ((panel->x + where.x) < style.windowMargin) {
            where.x = kFloatingEpsilon; // no room beside: under the element, in the panel
            where.y = (anchor.y + anchor.height + style.anchorGap) - panel->y;
        }

        // Neither placement may leave the window: the card is clamped into the
        // margin, top and bottom, whichever side it ended up on.
        where.y = std::clamp(where.y, style.windowMargin - panel->y, (windowHeight - height - style.windowMargin) - panel->y);
        return where;
    }

    // No panel named: under the element, clamped into the window's margins.
    where.x = std::clamp(anchor.x, style.windowMargin - anchor.x, (windowWidth - width - style.windowMargin) - anchor.x);
    where.y = std::clamp(
        anchor.y + anchor.height + style.anchorGap, style.windowMargin - anchor.y, (windowHeight - height - style.windowMargin) - anchor.y
    );
    where.x = std::max(where.x, kFloatingEpsilon); // non-zero: it floats
    return where;
}

void DrawHoverCard(
    GUI::Frame& ui, const HoverState& state, const ZHLN::Extent2D& viewport, const HoverCardStyle& style, std::string_view besideId
) {
    if (!HasHover(state)) {
        return;
    }

    const std::optional<GUI::ElementRect> anchor = ui.GetLastFrameRect(state.id);
    if (!anchor) {
        return; // the element is not in the last frame's tree yet
    }

    // As wide as the longest line needs -- with the same slack FitToWidth leaves,
    // or the card would be measured to the character and then cut the very line
    // it was sized for -- and never wider than the caller allows (a panel's
    // content width) or than the window itself.
    const float advance = CardAdvance(style);
    float       longest = 0.0f;
    for (const std::string& line: state.lines) {
        longest = std::max(longest, static_cast<float>(line.size()));
    }

    const float windowMax = static_cast<float>(viewport.width) - (style.windowMargin * 2.0f);
    float       width     = ((longest + kFitSlackChars) * advance) + (style.padding * 2.0f);
    if (style.maxWidth > 0.0f) {
        width = std::min(width, style.maxWidth);
    }
    width = std::max(std::min(width, windowMax), 0.0f);

    const float textWidth = std::max(width - (style.padding * 2.0f), 0.0f);
    const float height    = (static_cast<float>(state.lines.size()) * style.lineHeight) + (style.padding * 2.0f);

    // Where it goes is the widget's own rule, in one place, and the rendering is
    // the last thing that happens to it (see HoverCardPlacement).
    const std::optional<GUI::ElementRect> panel = besideId.empty() ? std::nullopt : ui.GetLastFrameRect(besideId);
    const HoverCardOffset where = HoverCardPlacement(width, height, *anchor, panel ? &*panel : nullptr, viewport, style);

    ui.Box(
        kCardId,
        BoxConfig {
            .width        = {.fixed = width},
            .color        = style.ink,
            .cornerRadius = {style.cornerRadius, style.cornerRadius, style.cornerRadius, style.cornerRadius},
            .padding      = style.padding,
            .gap          = style.lineGap,
            .direction    = Direction::Column,
            .offsetX      = where.x,
            .offsetY      = where.y,
        },
        [&]() -> void {
            for (const std::string& line: state.lines) {
                ui.Text(FitToWidth(line, textWidth, advance), style.fontSize, style.textInk);
            }
        }
    );
}

} // namespace ZHLN::GUI::Widget
