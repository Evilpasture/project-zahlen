// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/String.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <string_view>

namespace ZHLN::GUI {

constexpr size_t kWrapBufferCapacity = 512;

template <size_t Capacity>
auto WrapTextInto(const FontAtlas& font, std::string_view text, float scale, float maxWidth, ZHLN::FixedString<Capacity>& out) noexcept -> uint32_t {
    out.clear();
    if (text.empty()) {
        return 0;
    }
    if (maxWidth <= 0.0f) {
        out.assign(text);
        uint32_t lines = 1;
        for (char c: text) {
            if (c == '\n') {
                ++lines;
            }
        }
        return lines;
    }

    auto WidthOf = [&](std::string_view s) -> float { return MeasureTextBounds(font, s, scale).width(); };

    uint32_t totalLines = 0;
    size_t   lineStart  = 0;
    while (lineStart < text.size()) {
        size_t           hardBreak = text.find('\n', lineStart);
        size_t           segEnd    = (hardBreak == std::string_view::npos) ? text.size() : hardBreak;
        std::string_view seg       = text.substr(lineStart, segEnd - lineStart);

        size_t wordStart        = 0;
        size_t currentLineStart = 0;

        while (wordStart < seg.size()) {
            while (wordStart < seg.size() && seg[wordStart] == ' ') {
                ++wordStart;
            }
            if (wordStart >= seg.size()) {
                break;
            }

            size_t wordEnd = seg.find(' ', wordStart);
            if (wordEnd == std::string_view::npos) {
                wordEnd = seg.size();
            }

            std::string_view candidate = seg.substr(currentLineStart, wordEnd - currentLineStart);
            if (WidthOf(candidate) <= maxWidth || currentLineStart == wordStart) {
                wordStart = wordEnd;
            } else {
                if (!out.empty()) {
                    out.append("\n");
                }
                out.append(seg.substr(currentLineStart, wordStart - currentLineStart));
                ++totalLines;
                currentLineStart = wordStart;
            }
        }

        if (currentLineStart < seg.size()) {
            if (!out.empty()) {
                out.append("\n");
            }
            out.append(seg.substr(currentLineStart));
            ++totalLines;
        }

        lineStart = (hardBreak == std::string_view::npos) ? text.size() : hardBreak + 1;
    }
    return totalLines;
}

[[nodiscard]] inline auto MeasureWrappedTextBounds(const FontAtlas& font, std::string_view text, float scale, float maxWidth) noexcept -> TextBounds {
    ZHLN::FixedString<kWrapBufferCapacity> wrapped;
    WrapTextInto(font, text, scale, maxWidth, wrapped);
    return MeasureTextBounds(font, std::string_view(wrapped), scale);
}

} // namespace ZHLN::GUI
