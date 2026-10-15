// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Widget/BarStrip.hpp>
#include <algorithm>
#include <array>
#include <format>

namespace ZHLN::GUI::Widget {

namespace {

// A bar's id: the strip's base and the bar's index. Formatted into a fixed
// buffer per bar rather than a string, so drawing a strip allocates nothing --
// a panel draws several of these a frame.
constexpr size_t kBarIdCapacity = 96;

auto BarId(std::array<char, kBarIdCapacity>& buffer, std::string_view idBase, size_t bar) noexcept -> std::string_view {
    if (idBase.empty()) {
        return {};
    }
    char* end = std::format_to(buffer.data(), "{}-{}", idBase, bar);
    return std::string_view {buffer.data(), static_cast<size_t>(end - buffer.data())};
}

} // namespace

auto BarPeak(std::span<const float> samples, const BarStripStyle& style) noexcept -> float {
    if (style.scale == BarScale::Shared) {
        return std::max(style.sharedPeak, 1.0e-6f); // never a division by zero
    }

    float peak = style.peakFloor;
    for (const float sample: samples) {
        peak = std::max(peak, sample);
    }
    return std::max(peak, 1.0e-6f);
}

auto BarHeight(float value, float peak, const BarStripStyle& style) noexcept -> float {
    const float safe = std::max(peak, 1.0e-6f);
    const float fill = std::clamp(value / safe, 0.0f, 1.0f);
    return std::max(style.minBarHeight, fill * style.height);
}

void BarStrip(GUI::Context& ui, std::span<const float> samples, const BarStripStyle& style) {
    const size_t bars = samples.size() + style.absentBars;
    if (bars == 0 || style.width <= 0.0f || style.height <= 0.0f) {
        return;
    }

    const float slots   = static_cast<float>(bars);
    const float barWide = (style.width - ((slots - 1.0f) * style.gap)) / slots;
    if (barWide <= 0.0f) {
        return; // the gap has eaten the strip; drawing bars would overlap them
    }

    const float peak = BarPeak(samples, style);

    std::array<char, kBarIdCapacity> buffer {};

    // The strip's own box: as wide and tall as the style says, bars laid out
    // left to right and standing on its bottom edge. A caller that wants a
    // background behind them draws it outside this call.
    ui.BeginBox(
        style.idBase, BoxConfig {
                          .width      = {.fixed = style.width},
                          .height     = {.fixed = style.height},
                          .gap        = style.gap,
                          .direction  = Direction::Row,
                          .alignCross = Alignment::End,
                      }
    );
    for (size_t bar = 0; bar < bars; ++bar) {
        const bool  absent = bar < style.absentBars;
        const float value  = absent ? 0.0f : samples[bar - style.absentBars];

        const float height = absent ? style.minBarHeight : BarHeight(value, peak, style);

        JPH::Vec4 ink = style.ink;
        if (style.inkFor != nullptr) {
            ink = style.inkFor(style.inkUser, bar, value, absent);
        } else if (absent) {
            ink = style.absentInk;
        } else if (style.threshold > 0.0f && value >= style.threshold) {
            ink = style.hotInk;
        }

        ui.BeginBox(BarId(buffer, style.idBase, bar), BoxConfig {.width = {.fixed = barWide}, .height = {.fixed = height}, .color = ink});
        ui.EndBox();
    }
    ui.EndBox();
}

} // namespace ZHLN::GUI::Widget
