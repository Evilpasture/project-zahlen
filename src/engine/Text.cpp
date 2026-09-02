// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/Text.cpp
#include "Zahlen/Components.hpp"
#include <Zahlen/GUI.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <algorithm>
#include <array>
#include <cmath>
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

    const float x0 = rect.computedAbsMinX;
    const float y0 = rect.computedAbsMinY;
    const float x1 = rect.computedAbsMaxX;
    const float y1 = rect.computedAbsMaxY;

    PackedRGBA8   c = Math::PackColor(panel.color.GetX(), panel.color.GetY(), panel.color.GetZ(), panel.color.GetW());
    Packed1010102 n = Math::PackNormal(0, 0, 1);
    Packed1010102 t = Math::PackNormal(1, 0, 0, 1);

    const float width  = x1 - x0;
    const float height = y1 - y0;
    uint32_t    writtenCount = 0;

    // Radius order is top-left, top-right, bottom-right, bottom-left.  Clamp
    // and normalize the values just as a 2D rounded-rectangle primitive
    // would, so malformed configuration cannot make neighboring corners cross
    // one another.
    float radiusTL = std::max(0.0f, panel.borderRadius.GetX());
    float radiusTR = std::max(0.0f, panel.borderRadius.GetY());
    float radiusBR = std::max(0.0f, panel.borderRadius.GetZ());
    float radiusBL = std::max(0.0f, panel.borderRadius.GetW());

    if (width > 0.0f && height > 0.0f) {
        const float maxRadius = std::min(width, height) * 0.5f;
        radiusTL             = std::min(radiusTL, maxRadius);
        radiusTR             = std::min(radiusTR, maxRadius);
        radiusBR             = std::min(radiusBR, maxRadius);
        radiusBL             = std::min(radiusBL, maxRadius);

        float radiusScale = 1.0f;
        auto  constrainPair = [&](float a, float b, float limit) -> void {
            if (a + b > limit && a + b > 0.0f) {
                radiusScale = std::min(radiusScale, limit / (a + b));
            }
        };
        constrainPair(radiusTL, radiusTR, width);
        constrainPair(radiusBL, radiusBR, width);
        constrainPair(radiusTL, radiusBL, height);
        constrainPair(radiusTR, radiusBR, height);

        radiusTL *= radiusScale;
        radiusTR *= radiusScale;
        radiusBR *= radiusScale;
        radiusBL *= radiusScale;
    }

    const bool hasRoundedCorners = width > 0.0f && height > 0.0f &&
                                   (radiusTL > 0.0f || radiusTR > 0.0f || radiusBR > 0.0f || radiusBL > 0.0f);

    auto emitVertex = [&](float x, float y, float u, float v) -> void {
        outPos[writtenCount]    = {{x, y, 0.0f}};
        outAttr[writtenCount++] = {.normal = n, .tangent = t, .uv = Math::PackUV(u, v), .color = c};
    };

    if (hasRoundedCorners) {
        // A 16-sided perimeter (three segments per corner plus the four
        // straight edges) is enough for the small UI controls this function
        // serves.  A triangle fan uses 48 vertices, fitting the existing
        // renderer scratch allocation of 54 vertices per panel while giving
        // circles such as slider knobs a genuinely rounded silhouette.
        struct Point {
            float x;
            float y;
        };
        constexpr int   kCornerSegments = 3;
        constexpr float kPi             = 3.14159265358979323846f;
        constexpr float kQuarterStep    = (kPi * 0.5f) / static_cast<float>(kCornerSegments);

        std::array<Point, 16> perimeter {};
        uint32_t              perimeterCount = 0;
        auto appendPoint = [&](float x, float y) -> void {
            if (perimeterCount < perimeter.size()) {
                perimeter[perimeterCount++] = {.x = x, .y = y};
            }
        };
        auto appendArc = [&](float cx, float cy, float radius, float startAngle, int first, int last) -> void {
            for (int segment = first; segment <= last; ++segment) {
                const float angle = startAngle + kQuarterStep * static_cast<float>(segment);
                appendPoint(cx + std::cos(angle) * radius, cy + std::sin(angle) * radius);
            }
        };

        // Walk clockwise in screen coordinates: top-left, top-right,
        // bottom-right, bottom-left.  Straight-edge endpoints are explicit so
        // each rounded corner is joined by a real edge rather than a diagonal.
        appendPoint(x0 + radiusTL, y0);
        appendPoint(x1 - radiusTR, y0);
        appendArc(x1 - radiusTR, y0 + radiusTR, radiusTR, -kPi * 0.5f, 1, kCornerSegments);
        appendPoint(x1, y1 - radiusBR);
        appendArc(x1 - radiusBR, y1 - radiusBR, radiusBR, 0.0f, 1, kCornerSegments);
        appendPoint(x0 + radiusBL, y1);
        appendArc(x0 + radiusBL, y1 - radiusBL, radiusBL, kPi * 0.5f, 1, kCornerSegments);
        appendPoint(x0, y0 + radiusTL);
        // The final point of this arc is the first point of the perimeter;
        // omit it and let the fan close across the top edge.
        appendArc(x0 + radiusTL, y0 + radiusTL, radiusTL, kPi, 1, kCornerSegments - 1);

        const float centerX = (x0 + x1) * 0.5f;
        const float centerY = (y0 + y1) * 0.5f;
        const float invWidth  = 1.0f / width;
        const float invHeight = 1.0f / height;
        for (uint32_t i = 0; i < perimeterCount; ++i) {
            const Point& a = perimeter[i];
            const Point& b = perimeter[(i + 1) % perimeterCount];

            // Match the winding of the original UI quads.  UI coordinates use
            // a top-left origin, so the visually clockwise perimeter is the
            // renderer's front-facing order when emitted as (center, next,
            // current).
            emitVertex(centerX, centerY, (centerX - x0) * invWidth, (centerY - y0) * invHeight);
            emitVertex(b.x, b.y, (b.x - x0) * invWidth, (b.y - y0) * invHeight);
            emitVertex(a.x, a.y, (a.x - x0) * invWidth, (a.y - y0) * invHeight);
        }
    } else if (panel.edgeWidth > 0.0f && width > 0.0f && height > 0.0f) {
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

                emitVertex(qx0, qy0, qu0, qv0);
                emitVertex(qx0, qy1, qu0, qv1);
                emitVertex(qx1, qy0, qu1, qv0);
                emitVertex(qx1, qy0, qu1, qv0);
                emitVertex(qx0, qy1, qu0, qv1);
                emitVertex(qx1, qy1, qu1, qv1);
            }
        }
    } else {
        emitVertex(x0, y0, 0.0f, 0.0f);
        emitVertex(x0, y1, 0.0f, 1.0f);
        emitVertex(x1, y0, 1.0f, 0.0f);
        emitVertex(x1, y0, 1.0f, 0.0f);
        emitVertex(x0, y1, 0.0f, 1.0f);
        emitVertex(x1, y1, 1.0f, 1.0f);
    }
    return writtenCount;
}

} // namespace ZHLN::GUI
