// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/gui/Text.cpp
#include "Text.hpp"
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Types.hpp>
#include <algorithm>
#include <string>

namespace ZHLN::GUI {

TextBounds MeasureTextBounds(const FontAtlas& font, std::string_view text, float scale) noexcept {
    if (text.empty()) {
        return {};
    }

    TextBounds bounds;
    bounds.minX = 1e9f;
    bounds.maxX = -1e9f;
    bounds.minY = 1e9f;
    bounds.maxY = -1e9f;

    // Line breaks are part of the measurement, not something the renderer gets
    // to discover on its own: AppendTextVertices advances by TextLineHeight on
    // '\n' and resets the pen to the line's left edge, so the bounds of a
    // multi-line string are the union of its lines rather than the width of
    // every glyph run together. Without this, a wrapped label measures as one
    // line wide and one line tall while drawing as a paragraph.
    float currentX  = 0.0f;
    float lineTop   = 0.0f;
    bool  hasGlyphs = false;

    for (char c: text) {
        if (c == '\n') {
            currentX = 0.0f;
            lineTop += TextLineHeight(scale);
            continue;
        }
        if (c == '\r') {
            continue;
        }
        uint32_t glyphCode = static_cast<uint8_t>(c);
        if (glyphCode < 32 || glyphCode > 127) {
            glyphCode = '?';
        }

        const auto& g = font.glyphs[glyphCode - 32];

        float x0 = currentX + g.xoff * scale;
        float x1 = x0 + (g.x1 - g.x0) * scale;
        float y0 = lineTop + (g.yoff + 28.0f) * scale;
        float y1 = y0 + (g.y1 - g.y0) * scale;

        bounds.minX = std::min(bounds.minX, x0);
        bounds.maxX = std::max(bounds.maxX, x1);
        bounds.minY = std::min(bounds.minY, y0);
        bounds.maxY = std::max(bounds.maxY, y1);

        currentX += g.xadvance * scale;
        hasGlyphs = true;
    }

    if (!hasGlyphs) {
        return {};
    }

    return bounds;
}

uint32_t AppendTextVertices(
    VertexPosition*    outPos,
    VertexAttributes*  outAttr,
    const FontAtlas&   font,
    const std::string& text,
    float              x,
    float              y,
    float              scale,
    const JPH::Vec4&   color
) {
    if (text.empty()) {
        return 0;
    }

    float         currentX     = x;
    float         currentY     = y;
    float         lineHeight   = TextLineHeight(scale); // Line height step for newlines
    PackedRGBA8   packedColor  = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());
    Packed1010102 dummyNormal  = Math::PackNormal(0, 1, 0);
    Packed1010102 dummyTangent = Math::PackNormal(1, 0, 0, 1);

    uint32_t writtenCount = 0;

    for (char c: text) {
        if (c == '\n') {
            currentX = x;
            currentY += lineHeight;
            continue;
        }
        if (c == '\r') {
            continue;
        }

        uint32_t glyphCode = static_cast<uint8_t>(c);
        if (glyphCode < 32 || glyphCode > 127) {
            glyphCode = '?';
        }

        const auto& g  = font.glyphs[glyphCode - 32];
        float       u0 = g.x0 / 1024.0f; // Fixed: 1024.0f matches 1024x1024 atlas
        float       v0 = g.y0 / 1024.0f;
        float       u1 = g.x1 / 1024.0f;
        float       v1 = g.y1 / 1024.0f;

        float x0 = currentX + g.xoff * scale;
        // Offset by +28.0f to convert STB TTF baseline yoff to top-left bounding box coordinates
        float y0 = currentY + (g.yoff + 28.0f) * scale;
        float x1 = x0 + (g.x1 - g.x0) * scale;
        float y1 = y0 + (g.y1 - g.y0) * scale;

        outPos[writtenCount]    = {{x0, y0, 0.0f}};
        outAttr[writtenCount++] = {.normal = dummyNormal, .tangent = dummyTangent, .uv = Math::PackUV(u0, v0), .color = packedColor};
        outPos[writtenCount]    = {{x0, y1, 0.0f}};
        outAttr[writtenCount++] = {.normal = dummyNormal, .tangent = dummyTangent, .uv = Math::PackUV(u0, v1), .color = packedColor};
        outPos[writtenCount]    = {{x1, y0, 0.0f}};
        outAttr[writtenCount++] = {.normal = dummyNormal, .tangent = dummyTangent, .uv = Math::PackUV(u1, v0), .color = packedColor};

        outPos[writtenCount]    = {{x1, y0, 0.0f}};
        outAttr[writtenCount++] = {.normal = dummyNormal, .tangent = dummyTangent, .uv = Math::PackUV(u1, v0), .color = packedColor};
        outPos[writtenCount]    = {{x0, y1, 0.0f}};
        outAttr[writtenCount++] = {.normal = dummyNormal, .tangent = dummyTangent, .uv = Math::PackUV(u0, v1), .color = packedColor};
        outPos[writtenCount]    = {{x1, y1, 0.0f}};
        outAttr[writtenCount++] = {.normal = dummyNormal, .tangent = dummyTangent, .uv = Math::PackUV(u1, v1), .color = packedColor};

        currentX += g.xadvance * scale;
    }
    return writtenCount;
}
} // namespace ZHLN::GUI
