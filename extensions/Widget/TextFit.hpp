// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ===========================================================================
// Measuring a line of text against the box it goes in.
//
// A text element wider than its box does not clip: Clay draws past the edge. So
// a column too narrow for its content is a name drawn over the value beside it,
// and the two ways a panel handles that both need the same number first -- the
// width of one character at the size the line will be drawn.
//
// That number is not the font's own advance. The atlas is rasterized at one size
// (FontAtlas::fontSize) and a panel draws at several, so the advance at a size is
// the atlas advance scaled by the ratio between them. And when the engine has no
// font asset at all it draws with an embedded fallback, whose advance is a
// constant ratio of the size -- the larger of the two ratios this tree knows, so
// a layout measured without the asset is never narrower than the text.
//
// FitToWidth leaves a character of slack. The advance is the atlas's average,
// and a line of digits and capitals is wider than its average -- so a line fitted
// to its last character can still overflow, and a text that overflows wraps,
// which puts the ".." on a line of its own and reads as a glitch rather than as
// the cut it is.
//
// The measure is one advance times the character count, not a walk of the
// string: the engine's own GUI::MeasureTextBounds is the exact one, and it is
// what GUIContext uses to lay a text element out, but a panel measuring a column
// every frame asks for the cheap answer and pays for it with the slack -- which
// is why the slack is here and not at the call sites.
// ===========================================================================

#include <Zahlen/GUI/Font.hpp>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ZHLN::GUI::Widget {

// Per-character advance assumed when the engine has no font asset and is drawing
// with its embedded fallback. That fallback is 18/32 = 0.5625 em per glyph, and
// JetBrains Mono -- the font this tree vendors -- is 0.6 em, which is also what
// panels here were measured against on screen: a 30-character 16 px title is
// 288 px of ink.
inline constexpr float kFallbackAdvanceRatio = 0.6f;

// The slack every fit leaves, in characters. One is enough to cover the
// difference between the average advance and a line of wide glyphs.
inline constexpr float kFitSlackChars = 1.0f;

// The advance of one character at `fontSize`, straight from the atlas the engine
// is actually drawing with: every font this tree ships is monospaced, so one
// advance is all of them, and where a proportional one would differ, 'M' is the
// widest -- the safe choice either way. A null or empty atlas falls back to the
// ratio above.
[[nodiscard]] auto AdvancePerChar(const ZHLN::FontAtlas* atlas, float fontSize) noexcept -> float;

// The same advance at another size, from one already measured: what a panel that
// measured its table body once needs for the three sizes it draws.
[[nodiscard]] auto AdvanceAt(float measuredAdvance, float measuredFontSize, float fontSize) noexcept -> float;

// Whether a line fits, asked before it is cut -- so a caller can lay a long line
// out differently (a second line, a shorter label) instead of printing the end of
// it as "..".
[[nodiscard]] auto FitsWidth(std::string_view text, float maxWidth, float advance) noexcept -> bool;

// `text` cut to the longest prefix whose ink fits `maxWidth`, with a two-dot
// ellipsis when anything was dropped. Shorter than the limit by kFitSlackChars,
// so the cut is deliberate rather than a wrap.
[[nodiscard]] auto FitToWidth(std::string_view text, float maxWidth, float advance) -> std::string;

// The same cut, written into caller storage, for a caller drawing rows every
// frame: a row that owns a buffer measures, cuts and draws without allocating
// once. `text` may point into `buffer` -- the cut only ever writes at or before
// the character it has just read. Returns the fitted view, empty when the buffer
// has no room for it, and valid while `buffer` is.
[[nodiscard]] auto FitToWidthInto(std::string_view text, float maxWidth, float advance, std::span<char> buffer) noexcept -> std::string_view;

} // namespace ZHLN::GUI::Widget
