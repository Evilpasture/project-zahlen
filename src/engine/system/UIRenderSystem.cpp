// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UIRenderSystem.hpp"
#include "UILayoutSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ZHLN {

namespace {

struct TextBounds {
    float              minX = 0.0f;
    float              maxX = 0.0f;
    float              minY = 0.0f;
    float              maxY = 0.0f;
    [[nodiscard]] auto width() const noexcept -> float {
        return maxX - minX;
    }
    [[nodiscard]] auto height() const noexcept -> float {
        return maxY - minY;
    }
};

// Measures the exact visual ink bounding box of the rendered glyphs
auto MeasureTextBounds(const FontAtlas& font, std::string_view text, float scale) noexcept -> TextBounds {
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

// Computes the intersection [x0, y0, x1, y1] between two scissor rectangles
auto IntersectScissor(const ScissorRect& a, const ScissorRect& b) noexcept -> ScissorRect {
    int32_t x0 = std::max(a.x, b.x);
    int32_t y0 = std::max(a.y, b.y);
    int32_t x1 = std::min(a.x + static_cast<int32_t>(a.width), b.x + static_cast<int32_t>(b.width));
    int32_t y1 = std::min(a.y + static_cast<int32_t>(a.height), b.y + static_cast<int32_t>(b.height));

    if (x1 > x0 && y1 > y0) {
        return {.x = x0, .y = y0, .width = static_cast<uint32_t>(x1 - x0), .height = static_cast<uint32_t>(y1 - y0)};
    }
    return {.x = 0, .y = 0, .width = 0, .height = 0}; // Fully culled
}

} // namespace

void UIRenderSystem::Update(Engine& engine) {
    auto& reg        = engine.GetRegistry();
    auto  windowSize = engine.GetWindow().GetSize();

    if (windowSize.width == 0 || windowSize.height == 0) {
        return;
    }

    auto IsEntityOrAncestorHidden = [&](Entity ent) -> bool {
        Entity curr = ent;
        while (curr != Entity::Null() && reg.IsAlive(curr)) {
            if (auto* mesh = reg.Get<Components::MeshComponent>(curr)) {
                if ((mesh->flags & DrawFlags::Hidden) != DrawFlags::None) {
                    return true;
                }
            }
            auto* rect = reg.Get<Components::UIRectComponent>(curr);
            curr       = (rect != nullptr) ? rect->parentEntity : Entity::Null();
        }
        return false;
    };

    // 1. Resolve UI Hierarchy Layouts (Anchors, Offsets, Stacks)
    UILayoutSystem layoutSystem;
    layoutSystem.ResolveLayouts(reg, {.width = static_cast<float>(windowSize.width), .height = static_cast<float>(windowSize.height)});

    // 2. Collect ALL Unique UI Entities (Panels, Text, Rects)
    std::unordered_set<uint64_t> uniqueEntities;
    for (Entity e: reg.GetEntitiesWith<Components::UIRectComponent>()) {
        uniqueEntities.insert(e.Pack());
    }
    for (Entity e: reg.GetEntitiesWith<Components::UIPanelComponent>()) {
        uniqueEntities.insert(e.Pack());
    }
    for (Entity e: reg.GetEntitiesWith<Components::TextComponent>()) {
        uniqueEntities.insert(e.Pack());
    }

    if (uniqueEntities.empty()) {
        return;
    }

    struct SortEntry {
        Entity   entity;
        uint32_t depth;
    };
    std::vector<SortEntry> sortedEntries;
    sortedEntries.reserve(uniqueEntities.size());

    for (uint64_t packed: uniqueEntities) {
        Entity e = Entity::Unpack(packed);
        if (!reg.IsAlive(e)) {
            continue;
        }

        uint32_t depth = 0;
        if (auto* rect = reg.Get<Components::UIRectComponent>(e)) {
            depth = rect->hierarchyDepth;
        }

        sortedEntries.push_back({.entity = e, .depth = depth});
    }

    // Sort ALL UI Entities by Hierarchy Depth (Ascending: Parents & lower layers first)
    std::ranges::sort(sortedEntries, [](const auto& a, const auto& b) -> auto { return a.depth < b.depth; });

    // ========================================================================
    // 3. PRE-PASS: TOP-DOWN MULTI-ANCESTOR SCISSOR PROPAGATION & INTERSECTION
    // ========================================================================
    HashMap<uint64_t, ScissorRect> activeScissors;

    for (const auto& entry: sortedEntries) {
        Entity e = entry.entity;
        if (IsEntityOrAncestorHidden(e)) {
            continue;
        }
        auto* rect = reg.Get<Components::UIRectComponent>(e);
        if (rect == nullptr) {
            continue;
        }

        if (rect->parentEntity == Entity::Null() || !reg.IsAlive(rect->parentEntity)) {
            continue;
        }

        Entity      parent     = rect->parentEntity;
        const auto* parentRect = reg.Get<Components::UIRectComponent>(parent);
        if (parentRect == nullptr) {
            continue;
        }

        const ScissorRect* parentScissorPtr = activeScissors.Find(parent.Pack());

        if (parentRect->clipChildren) {
            ScissorRect parentClip = {
                .x      = static_cast<int32_t>(std::max(0.0f, parentRect->computedAbsMinX)),
                .y      = static_cast<int32_t>(std::max(0.0f, parentRect->computedAbsMinY)),
                .width  = static_cast<uint32_t>(std::max(0.0f, parentRect->computedAbsMaxX - parentRect->computedAbsMinX)),
                .height = static_cast<uint32_t>(std::max(0.0f, parentRect->computedAbsMaxY - parentRect->computedAbsMinY))
            };

            if (parentScissorPtr != nullptr) {
                // Intersect parent's clip box with parent's inherited scissor
                ScissorRect intersected = IntersectScissor(*parentScissorPtr, parentClip);
                activeScissors.Insert(e.Pack(), intersected);
            } else {
                activeScissors.Insert(e.Pack(), parentClip);
            }
        } else if (parentScissorPtr != nullptr) {
            ScissorRect parentScissor = *parentScissorPtr;
            activeScissors.Insert(e.Pack(), parentScissor);
        }
    }

    // CPU Staging Arrays
    ZHLN::Array<VertexPosition>   localPositions;
    ZHLN::Array<VertexAttributes> localAttributes;
    ZHLN::Array<UIBatch>          localBatches;

    localPositions.reserve(4096);
    localAttributes.reserve(4096);
    localBatches.reserve(128);

    uint32_t currentVertexOffset = 0;

    auto QueueBatch = [&](TextureHandle textureHandle, uint32_t count, bool useScissor, ScissorRect scissor, bool isSDF = false) -> void {
        if (count == 0) {
            return;
        }

        if (!localBatches.empty()) {
            auto& last = localBatches.back();
            if (last.texture == textureHandle && last.isSDF == isSDF && last.useScissor == useScissor &&
                (!useScissor || (std::memcmp(&last.scissorRect, &scissor, sizeof(ScissorRect)) == 0))) {
                last.vertexCount += count;
                return;
            }
        }

        localBatches.push_back(
            {.texture     = textureHandle,
             .vertexStart = currentVertexOffset - count,
             .vertexCount = count,
             .useScissor  = useScissor,
             .isSDF       = isSDF,
             .scissorRect = scissor}
        );
    };

    auto             uiSettingsEntities = reg.GetEntitiesWith<Components::UISettingsComponent>();
    const FontAtlas* activeFont         = nullptr;
    if (!uiSettingsEntities.empty()) {
        activeFont = &reg.Get<Components::UISettingsComponent>(uiSettingsEntities[0])->fontAtlas;
    }

    // ========================================================================
    // 4. UNIFIED INTERLEAVED PASS: PANELS AND TEXTS IN STRICT DEPTH ORDER
    // ========================================================================
    for (const auto& entry: sortedEntries) {
        Entity e = entry.entity;

        if (IsEntityOrAncestorHidden(e)) {
            continue;
        }

        bool        useScissor = false;
        ScissorRect currentScissor {};

        const ScissorRect* sc = activeScissors.Find(e.Pack());
        if (sc != nullptr) {
            if (sc->width == 0 || sc->height == 0) {
                continue; // Fully culled by multi-ancestor scissor intersection
            }
            useScissor     = true;
            currentScissor = *sc;
        }

        auto* rect = reg.Get<Components::UIRectComponent>(e);

        // A. Process Panel (if entity has UIPanelComponent)
        if (auto* panel = reg.Get<Components::UIPanelComponent>(e)) {
            if (rect != nullptr) {
                size_t startIdx = localPositions.size();
                localPositions.resize(startIdx + 54);
                localAttributes.resize(startIdx + 54);

                uint32_t written = GUI::AppendPanelVertices(&localPositions[startIdx], &localAttributes[startIdx], *rect, *panel);

                localPositions.resize(startIdx + written);
                localAttributes.resize(startIdx + written);

                currentVertexOffset += written;
                QueueBatch(panel->texture, written, useScissor, currentScissor, false);
            }
        }

        // B. Process Text (if entity has TextComponent)
        if (auto* text = reg.Get<Components::TextComponent>(e)) {
            float drawX = text->offsetX;
            float drawY = text->offsetY;

            std::string displayStr = text->text.c_str();
            // A TextInput's editable text lives on a child `_ti_text` entity;
            // walk up to find the UITextInputComponent that owns the cursor.
            const Components::UITextInputComponent* input = reg.Get<Components::UITextInputComponent>(e);
            if (input == nullptr && rect != nullptr) {
                Entity cur = rect->parentEntity;
                for (int hops = 0; hops < 4 && cur != Entity::Null() && reg.IsAlive(cur); ++hops) {
                    input = reg.Get<Components::UITextInputComponent>(cur);
                    if (input != nullptr) break;
                    if (const auto* pr = reg.Get<Components::UIRectComponent>(cur)) cur = pr->parentEntity;
                    else break;
                }
            }
            if (input != nullptr) {
                std::string_view raw = input->text;
                if (input->isFocused) {
                    displayStr = std::string(raw.substr(0, input->cursorIndex)) + "|" + std::string(raw.substr(input->cursorIndex));
                } else {
                    displayStr = std::string(raw);
                }
            }

            if (rect != nullptr) {
                float containerWidth  = rect->computedAbsMaxX - rect->computedAbsMinX;
                float containerHeight = rect->computedAbsMaxY - rect->computedAbsMinY;

                TextBounds bounds = (activeFont != nullptr) ? MeasureTextBounds(*activeFont, displayStr, text->scale) : TextBounds {};

                // 1. Horizontal Alignment (Clamped to prevent left overflow)
                if (text->align == TextAlignment::Center) {
                    float centerOffset = std::max(0.0f, (containerWidth - bounds.width()) * 0.5f);
                    drawX              = rect->computedAbsMinX + centerOffset - bounds.minX + text->offsetX;
                } else if (text->align == TextAlignment::Right) {
                    float rightOffset = std::max(0.0f, containerWidth - bounds.width());
                    drawX             = rect->computedAbsMinX + rightOffset - bounds.minX + text->offsetX;
                } else {
                    drawX = rect->computedAbsMinX - bounds.minX + text->offsetX;
                }

                // 2. Vertical Alignment (Clamped to prevent top overflow)
                if (text->verticalAlign == TextVerticalAlignment::Center) {
                    float centerOffset = std::max(0.0f, (containerHeight - bounds.height()) * 0.5f);
                    drawY              = rect->computedAbsMinY + centerOffset - bounds.minY + text->offsetY;
                } else if (text->verticalAlign == TextVerticalAlignment::Bottom) {
                    float bottomOffset = std::max(0.0f, containerHeight - bounds.height());
                    drawY              = rect->computedAbsMinY + bottomOffset - bounds.minY + text->offsetY;
                } else {
                    drawY = rect->computedAbsMinY - bounds.minY + text->offsetY;
                }
            }

            if (activeFont != nullptr && !displayStr.empty()) {
                auto   maxVertsNeeded = static_cast<uint32_t>(displayStr.length() * 6);
                size_t startIdx       = localPositions.size();

                localPositions.resize(startIdx + maxVertsNeeded);
                localAttributes.resize(startIdx + maxVertsNeeded);

                uint32_t written = GUI::AppendTextVertices(
                    &localPositions[startIdx], &localAttributes[startIdx], *activeFont, displayStr, drawX, drawY, text->scale, text->color
                );

                localPositions.resize(startIdx + written);
                localAttributes.resize(startIdx + written);

                currentVertexOffset += written;

                TextureHandle fontTexHandle = (text->fontIndex != TextureHandle::Invalid) ? text->fontIndex : activeFont->texture;
                QueueBatch(fontTexHandle, written, useScissor, currentScissor, true);
            }
        }
    }

    // ========================================================================
    // 5. SUBMIT BATCHES TO RENDER CONTEXT
    // ========================================================================
    engine.GetRenderContext().SubmitUI(
        localBatches.data(), static_cast<uint32_t>(localBatches.size()), localPositions.data(), localAttributes.data(),
        static_cast<uint32_t>(localPositions.size())
    );
}

} // namespace ZHLN
