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

TextBounds MeasureWrappedTextBounds(const FontAtlas& font, std::string_view text, float scale, float maxWidth) noexcept {
    // The wrap and the measure share one buffer size so the layout pass and
    // the renderer can never disagree about which characters survived.
    ZHLN::FixedString<kWrapBufferCapacity> wrapped;
    WrapTextInto(font, text, scale, maxWidth, wrapped);
    return MeasureTextBounds(font, std::string_view(wrapped), scale);
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

namespace {

// Tile emits one quad per repeat, so its vertex count depends on the rect.
// The repeat count is capped: a 1-pixel sprite tiled across a full-screen quad
// would otherwise ask for a million quads from a typo'd sourceWidth.
constexpr int32_t kMaxTileRepeats = 64;

struct ImageQuad {
    float dx0;
    float dy0;
    float dx1;
    float dy1;
    float du0;
    float dv0;
    float du1;
    float dv1;
};

} // namespace

uint32_t CountImageVertices(const Components::UIRectComponent& rect, const Components::UIImageComponent& image) noexcept {
    const float width  = rect.computedAbsMaxX - rect.computedAbsMinX;
    const float height = rect.computedAbsMaxY - rect.computedAbsMinY;
    if (width <= 0.0f || height <= 0.0f || image.tint.GetW() <= 0.0f) {
        return 0;
    }

    if (image.mode == ImageScaleMode::Tile) {
        const float tileWidth  = (image.sourceWidth > 0.0f) ? image.sourceWidth : width;
        const float tileHeight = (image.sourceHeight > 0.0f) ? image.sourceHeight : height;
        const int32_t columns  = std::clamp(static_cast<int32_t>(std::ceil(width / tileWidth)), 1, kMaxTileRepeats);
        const int32_t rows     = std::clamp(static_cast<int32_t>(std::ceil(height / tileHeight)), 1, kMaxTileRepeats);
        return static_cast<uint32_t>(columns) * static_cast<uint32_t>(rows) * 6u;
    }

    return 6;
}

uint32_t AppendImageVertices(
    VertexPosition*                     outPos,
    VertexAttributes*                   outAttr,
    const Components::UIRectComponent&  rect,
    const Components::UIImageComponent& image
) {
    const float x0 = rect.computedAbsMinX;
    const float y0 = rect.computedAbsMinY;
    const float x1 = rect.computedAbsMaxX;
    const float y1 = rect.computedAbsMaxY;

    const float width  = x1 - x0;
    const float height = y1 - y0;
    if (width <= 0.0f || height <= 0.0f || image.tint.GetW() <= 0.0f) {
        return 0;
    }

    PackedRGBA8   c = Math::PackColor(image.tint.GetX(), image.tint.GetY(), image.tint.GetZ(), image.tint.GetW());
    Packed1010102 n = Math::PackNormal(0, 0, 1);
    Packed1010102 t = Math::PackNormal(1, 0, 0, 1);

    uint32_t writtenCount = 0;
    auto     emitVertex   = [&](float px, float py, float u, float v) -> void {
        outPos[writtenCount]    = {{px, py, 0.0f}};
        outAttr[writtenCount++] = {.normal = n, .tangent = t, .uv = Math::PackUV(u, v), .color = c};
    };
    auto emitQuad = [&](const ImageQuad& q) -> void {
        emitVertex(q.dx0, q.dy0, q.du0, q.dv0);
        emitVertex(q.dx0, q.dy1, q.du0, q.dv1);
        emitVertex(q.dx1, q.dy0, q.du1, q.dv0);
        emitVertex(q.dx1, q.dy0, q.du1, q.dv0);
        emitVertex(q.dx0, q.dy1, q.du0, q.dv1);
        emitVertex(q.dx1, q.dy1, q.du1, q.dv1);
    };

    const float su0 = image.uv0x;
    const float sv0 = image.uv0y;
    const float su1 = image.uv1x;
    const float sv1 = image.uv1y;

    const bool  hasSourceSize = (image.sourceWidth > 0.0f) && (image.sourceHeight > 0.0f);
    ImageQuad   quad {.dx0 = x0, .dy0 = y0, .dx1 = x1, .dy1 = y1, .du0 = su0, .dv0 = sv0, .du1 = su1, .dv1 = sv1};

    switch (image.mode) {
        case ImageScaleMode::FitAspect: {
            // Scale to fit INSIDE the rect: the quad shrinks and centres, the
            // UV region stays whole. This is the "icon" behaviour.
            if (hasSourceSize) {
                const float s  = std::min(width / image.sourceWidth, height / image.sourceHeight);
                const float nw = image.sourceWidth * s;
                const float nh = image.sourceHeight * s;
                const float cx = (x0 + x1) * 0.5f;
                const float cy = (y0 + y1) * 0.5f;
                quad.dx0       = cx - nw * 0.5f;
                quad.dx1       = cx + nw * 0.5f;
                quad.dy0       = cy - nh * 0.5f;
                quad.dy1       = cy + nh * 0.5f;
            }
            emitQuad(quad);
            break;
        }

        case ImageScaleMode::CropAspect: {
            // Scale to COVER the rect and crop: the quad keeps the full rect,
            // the UV window is inset so the aspect ratio survives.
            if (hasSourceSize) {
                const float s      = std::max(width / image.sourceWidth, height / image.sourceHeight);
                const float visW   = (s > 0.0f) ? (width / s) : image.sourceWidth;
                const float visH   = (s > 0.0f) ? (height / s) : image.sourceHeight;
                const float uSpan  = (su1 - su0) * std::clamp(visW / image.sourceWidth, 0.0f, 1.0f);
                const float vSpan  = (sv1 - sv0) * std::clamp(visH / image.sourceHeight, 0.0f, 1.0f);
                const float cu     = (su0 + su1) * 0.5f;
                const float cv     = (sv0 + sv1) * 0.5f;
                quad.du0           = cu - uSpan * 0.5f;
                quad.du1           = cu + uSpan * 0.5f;
                quad.dv0           = cv - vSpan * 0.5f;
                quad.dv1           = cv + vSpan * 0.5f;
            }
            emitQuad(quad);
            break;
        }

        case ImageScaleMode::Tile: {
            // Repeat the region at its native pixel size. Emitting one quad
            // per repeat keeps this correct with the renderer's clamped UI
            // sampler — no reliance on hardware UV wrap.
            const float tileWidth  = (image.sourceWidth > 0.0f) ? image.sourceWidth : width;
            const float tileHeight = (image.sourceHeight > 0.0f) ? image.sourceHeight : height;
            const int32_t columns  = std::clamp(static_cast<int32_t>(std::ceil(width / tileWidth)), 1, kMaxTileRepeats);
            const int32_t rows     = std::clamp(static_cast<int32_t>(std::ceil(height / tileHeight)), 1, kMaxTileRepeats);

            for (int32_t row = 0; row < rows; ++row) {
                for (int32_t col = 0; col < columns; ++col) {
                    const float tileX = x0 + static_cast<float>(col) * tileWidth;
                    const float tileY = y0 + static_cast<float>(row) * tileHeight;
                    // Edge tiles are cropped by the rect, and their UV window
                    // is inset by the same fraction so the sprite is not
                    // stretched into the leftover sliver.
                    const float cellW = std::min(tileWidth, x1 - tileX);
                    const float cellH = std::min(tileHeight, y1 - tileY);
                    if (cellW <= 0.0f || cellH <= 0.0f) {
                        continue;
                    }

                    ImageQuad tile {.dx0 = tileX,
                                    .dy0 = tileY,
                                    .dx1 = tileX + cellW,
                                    .dy1 = tileY + cellH,
                                    .du0 = su0,
                                    .dv0 = sv0,
                                    .du1 = su0 + (su1 - su0) * (cellW / tileWidth),
                                    .dv1 = sv0 + (sv1 - sv0) * (cellH / tileHeight)};
                    emitQuad(tile);
                }
            }
            break;
        }

        case ImageScaleMode::Stretch:
        default:
            emitQuad(quad);
            break;
    }

    return writtenCount;
}

} // namespace ZHLN::GUI
