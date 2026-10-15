// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Widget/TextFit.hpp>
#include <algorithm>
#include <format>
#include <ranges>

namespace ZHLN::GUI::Widget {

namespace {

// The widest glyph the atlas can be asked for: 'M' in a proportional font, and
// in a monospaced one any glyph at all, since they are all the same width.
constexpr uint32_t kWidestProbe = static_cast<uint32_t>('M');

} // namespace

auto AdvancePerChar(const ZHLN::FontAtlas* atlas, float fontSize) noexcept -> float {
    if (atlas != nullptr && atlas->glyphCount > 0 && atlas->fontSize > 0.0f) {
        const float advance = atlas->GlyphFor(kWidestProbe).xadvance * atlas->ScaleFor(fontSize);
        if (advance > 0.0f) {
            return advance;
        }
    }
    return kFallbackAdvanceRatio * fontSize;
}

auto AdvanceAt(float measuredAdvance, float measuredFontSize, float fontSize) noexcept -> float {
    if (measuredAdvance > 0.0f && measuredFontSize > 0.0f) {
        return measuredAdvance * (fontSize / measuredFontSize);
    }
    return kFallbackAdvanceRatio * fontSize;
}

auto FitsWidth(std::string_view text, float maxWidth, float advance) noexcept -> bool {
    if (advance <= 0.0f || maxWidth <= 0.0f) {
        return true; // nothing to measure against; draw it and let the caller see
    }
    return static_cast<float>(text.size()) <= (maxWidth / advance) - kFitSlackChars;
}

auto FitToWidth(std::string_view text, float maxWidth, float advance) -> std::string {
    if (advance <= 0.0f || maxWidth <= 0.0f) {
        return std::string(text);
    }

    const size_t fits = static_cast<size_t>(std::max(0.0f, (maxWidth / advance) - kFitSlackChars));
    if (text.size() <= fits) {
        return std::string(text);
    }
    if (fits <= 3) {
        return std::string(text.substr(0, std::min(text.size(), fits)));
    }
    return std::format("{}..", text.substr(0, fits - 2));
}

auto FitToWidthInto(std::string_view text, float maxWidth, float advance, std::span<char> buffer) noexcept -> std::string_view {
    if (buffer.empty()) {
        return {};
    }

    // The whole line, when the whole line fits -- and when the caller has no
    // measurement to make, which is the same answer.
    const size_t room = (advance <= 0.0f || maxWidth <= 0.0f) ? text.size() : static_cast<size_t>(std::max(0.0f, (maxWidth / advance) - kFitSlackChars));
    if (text.size() <= room) {
        if (text.size() > buffer.size()) {
            return {}; // no room for the line: the caller's buffer is the limit
        }
        std::ranges::copy(text, buffer.begin());
        return std::string_view {buffer.data(), text.size()};
    }

    // The cut: as much of the line as fits, then the two dots -- in place, which
    // is safe because the prefix is copied from where it already is and the dots
    // land past it.
    size_t kept = std::min(room, text.size());
    if (kept <= 3) {
        if (kept > buffer.size()) {
            return {};
        }
        std::ranges::copy(text.substr(0, kept), buffer.begin());
        return std::string_view {buffer.data(), kept};
    }
    if (kept > buffer.size()) {
        return {};
    }
    std::ranges::copy(text.substr(0, kept - 2), buffer.begin());
    buffer[kept - 2] = '.';
    buffer[kept - 1] = '.';
    return std::string_view {buffer.data(), kept};
}

} // namespace ZHLN::GUI::Widget
