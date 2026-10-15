// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ===========================================================================
// A span of samples drawn as a strip of bars.
//
// Three panels in this tree draw one, and each had written the same arithmetic
// with the same three questions to answer:
//
//   * what fills the height? A frame-time chart scales against the number that
//     is normal for the window (a single stall must not flatten the rest); a
//     sparkline scales against its own peak, so a row says "I am spiking" rather
//     than "I am smaller than MainPass1"; a row whose samples all sit at the
//     noise floor must not fill every bar, which is what a floor under the scale
//     is for.
//   * what colour is a bar? Below a threshold or above it (the chart's tail), or
//     by its own value on a ramp (a core cell's load), or a fixed ink.
//   * what are the bars at the left end, before the series has any samples? They
//     are an absence, not a reading, and a strip that draws them as zeros says
//     "this was idle" about a metric that has not been born yet.
//
// The answers are a style, not a rewrite: the strip lays the bars out to the
// width it is given, scales them by the rule the style names, and colours them by
// the style's ink or by a caller's policy. Everything else -- what the numbers
// mean, where the strip sits, what opens when the pointer is over it -- stays
// with the panel.
//
// It draws through GUI::Context and nothing else: no font, no clock, no state
// between frames. The same samples and the same style are the same pixels.
// ===========================================================================

#include <Zahlen/gui/GUI.hpp> // the context, its boxes, and the layout a strip is
#include <Jolt/Math/Vec4.h>    // drawn in; Jolt.h comes with it, which Vec4.h needs
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ZHLN::GUI::Widget {

// How the strip turns samples into heights.
enum class BarScale : uint8_t {
    // One value fills the height, named by BarStripStyle::sharedPeak. A chart of
    // samples that share a meaning (frame times against the window's normal).
    Shared,
    // The strip's own tallest sample fills the height, so a row reads as "these
    // are its own peaks". BarStripStyle::peakFloor is the smallest that scale may
    // be -- without a floor, a row whose samples are all noise draws every bar at
    // full height, and a row that costs nothing looks like the busiest thing on
    // the panel.
    OwnPeak,
};

// Decides one bar's colour from the bar's own number. `absent` marks the bars
// before the series started, which are handed here too so a caller can draw them
// its own way. Returning a zero-alpha colour draws no bar; the strip applies no
// alpha of its own.
using BarInkFn = JPH::Vec4 (*)(void* user, size_t bar, float value, bool absent);

struct BarStripStyle {
    // The strip's box, in pixels. Bars are laid out across `width` and stand up
    // from the bottom of `height`.
    float width  = 0.0f;
    float height = 0.0f;

    // Between bars, and the shortest a bar may be: a zero sample still leaves a
    // baseline to read the shape against.
    float gap          = 1.0f;
    float minBarHeight = 2.0f;

    BarScale scale = BarScale::OwnPeak;

    // With BarScale::OwnPeak: the smallest the scale may be, in the samples' own
    // units. 0 leaves the scale free.
    float peakFloor = 0.0f;

    // With BarScale::Shared: the value that fills the height.
    float sharedPeak = 1.0f;

    // Leading bars with no sample behind them: `samples` is shorter than the
    // strip, and these are the slots at the left that never had one. They are
    // drawn in `absentInk` (or handed to the ink policy) rather than as zeros.
    size_t absentBars = 0;

    // When set, a bar at or above this value takes `hotInk`. 0 leaves every bar
    // in `ink` unless a policy says otherwise.
    float threshold = 0.0f;

    JPH::Vec4 ink       = {0.60f, 0.70f, 0.85f, 1.00f};
    JPH::Vec4 hotInk    = {1.00f, 0.55f, 0.30f, 1.00f};
    JPH::Vec4 absentInk = {0.02f, 0.03f, 0.05f, 0.85f};

    // Wins over `ink`, `hotInk` and `threshold` when set: `inkUser` is whatever
    // the caller needs to reach its ramp or its palette (a core's load colour,
    // say), and it is passed through untouched.
    BarInkFn inkFor  = nullptr;
    void*    inkUser = nullptr;

    // The id the strip's own box is drawn with; bar `i` inside it is
    // "<idBase>-<i>". Empty draws unnamed boxes, which is what a strip with
    // nothing to hover wants.
    std::string_view idBase {};
};

// Draws `samples` (oldest first) as a strip: its own box, `style.width` by
// `style.height`, bars standing up from the bottom. `absentBars` slots at the
// left are drawn first, so the strip reads as a series that is still filling
// rather than one that starts abruptly. Draws nothing without bars at all.
void BarStrip(GUI::Context& ui, std::span<const float> samples, const BarStripStyle& style);

// The height a bar of `value` takes under a scale of `peak`, in pixels -- the
// arithmetic above, for a caller that needs the number (a hit test, a label's
// baseline) without drawing the strip.
[[nodiscard]] auto BarHeight(float value, float peak, const BarStripStyle& style) noexcept -> float;

// The scale `style` works out for `samples`: sharedPeak, or the tallest sample
// with `peakFloor` under it -- and never zero, so a caller dividing by it is
// always safe. The strip works it out once and uses it for every bar; a caller
// wanting one height asks for the peak first.
[[nodiscard]] auto BarPeak(std::span<const float> samples, const BarStripStyle& style) noexcept -> float;

} // namespace ZHLN::GUI::Widget
