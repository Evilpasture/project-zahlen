// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/Text.cpp
#include "Zahlen/Components.hpp"
#include <Zahlen/GUI.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
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

    float currentX  = 0.0f;
    bool  hasGlyphs = false;

    for (char c: text) {
        if (c == '\n' || c == '\r') {
            continue;
        }
        uint32_t glyphCode = static_cast<uint8_t>(c);
        if (glyphCode < 32 || glyphCode > 127) {
            glyphCode = '?';
        }

        const auto& g = font.glyphs[glyphCode - 32];

        float x0 = currentX + g.xoff * scale;
        float x1 = x0 + (g.x1 - g.x0) * scale;
        float y0 = (g.yoff + 28.0f) * scale;
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
    float         lineHeight   = 36.0f * scale; // Line height step for newlines
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

uint32_t
    AppendPanelVertices(VertexPosition* outPos, VertexAttributes* outAttr, const Components::UIRectComponent& rect, const Components::UIPanelComponent& panel) {
    // Skip generating quad geometry for completely transparent/invisible layout boxes
    if (panel.color.GetW() <= 0.0f && panel.edgeWidth <= 0.0f) {
        return 0;
    }

    float x0 = rect.computedAbsMinX;
    float y0 = rect.computedAbsMinY;
    float x1 = rect.computedAbsMaxX;
    float y1 = rect.computedAbsMaxY;

    PackedRGBA8   c = Math::PackColor(panel.color.GetX(), panel.color.GetY(), panel.color.GetZ(), panel.color.GetW());
    Packed1010102 n = Math::PackNormal(0, 0, 1);
    Packed1010102 t = Math::PackNormal(1, 0, 0, 1);

    float    width        = x1 - x0;
    float    height       = y1 - y0;
    uint32_t writtenCount = 0;

    if (panel.edgeWidth > 0.0f && width > 0.0f && height > 0.0f) {
        float borderX = std::min(panel.edgeWidth, width * 0.5f);
        float borderY = std::min(panel.edgeWidth, height * 0.5f);

        float xs[4] = {x0, x0 + borderX, x1 - borderX, x1};
        float ys[4] = {y0, y0 + borderY, y1 - borderY, y1};
        float us[4] = {0.0f, panel.uvLeft, 1.0f - panel.uvRight, 1.0f};
        float vs[4] = {0.0f, panel.uvTop, 1.0f - panel.uvBottom, 1.0f};

        for (int r = 0; r < 3; ++r) {
            for (int col = 0; col < 3; ++col) {
                float qx0 = xs[col];
                float qx1 = xs[col + 1];
                float qy0 = ys[r];
                float qy1 = ys[r + 1];
                float qu0 = us[col];
                float qu1 = us[col + 1];
                float qv0 = vs[r];
                float qv1 = vs[r + 1];

                outPos[writtenCount]    = {{qx0, qy0, 0.0f}};
                outAttr[writtenCount++] = {.normal = n, .tangent = t, .uv = Math::PackUV(qu0, qv0), .color = c};
                outPos[writtenCount]    = {{qx0, qy1, 0.0f}};
                outAttr[writtenCount++] = {.normal = n, .tangent = t, .uv = Math::PackUV(qu0, qv1), .color = c};
                outPos[writtenCount]    = {{qx1, qy0, 0.0f}};
                outAttr[writtenCount++] = {.normal = n, .tangent = t, .uv = Math::PackUV(qu1, qv0), .color = c};

                outPos[writtenCount]    = {{qx1, qy0, 0.0f}};
                outAttr[writtenCount++] = {.normal = n, .tangent = t, .uv = Math::PackUV(qu1, qv0), .color = c};
                outPos[writtenCount]    = {{qx0, qy1, 0.0f}};
                outAttr[writtenCount++] = {.normal = n, .tangent = t, .uv = Math::PackUV(qu0, qv1), .color = c};
                outPos[writtenCount]    = {{qx1, qy1, 0.0f}};
                outAttr[writtenCount++] = {.normal = n, .tangent = t, .uv = Math::PackUV(qu1, qv1), .color = c};
            }
        }
    } else {
        outPos[0]    = {{x0, y0, 0.0f}};
        outAttr[0]   = {.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 0.0f), .color = c};
        outPos[1]    = {{x0, y1, 0.0f}};
        outAttr[1]   = {.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 1.0f), .color = c};
        outPos[2]    = {{x1, y0, 0.0f}};
        outAttr[2]   = {.normal = n, .tangent = t, .uv = Math::PackUV(1.0f, 0.0f), .color = c};
        outPos[3]    = {{x1, y0, 0.0f}};
        outAttr[3]   = {.normal = n, .tangent = t, .uv = Math::PackUV(1.0f, 0.0f), .color = c};
        outPos[4]    = {{x0, y1, 0.0f}};
        outAttr[4]   = {.normal = n, .tangent = t, .uv = Math::PackUV(0.0f, 1.0f), .color = c};
        outPos[5]    = {{x1, y1, 0.0f}};
        outAttr[5]   = {.normal = n, .tangent = t, .uv = Math::PackUV(1.0f, 1.0f), .color = c};
        writtenCount = 6;
    }
    return writtenCount;
}

} // namespace ZHLN::GUI
