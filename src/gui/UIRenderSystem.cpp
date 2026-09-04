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
#include <Zahlen/gui/UIComponents.hpp>
#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ZHLN {

namespace {

// Text measurement is deliberately NOT re-implemented here: GUI::TextBounds /
// GUI::MeasureTextBounds (src/gui/Text.cpp) are the single source of truth
// shared with the Yoga measure callback and the word wrapper. A local copy is
// how a label ends up measured as one line and drawn as a paragraph.
using TextBounds = GUI::TextBounds;

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
            auto* rect = reg.Get<GUI::UIComponents::UIRectComponent>(curr);
            curr       = (rect != nullptr) ? rect->parentEntity : Entity::Null();
        }
        return false;
    };

    // 1. Resolve UI Hierarchy Layouts (Anchors, Offsets, Stacks)
    UILayoutSystem layoutSystem;
    layoutSystem.ResolveLayouts(reg, {.width = static_cast<float>(windowSize.width), .height = static_cast<float>(windowSize.height)});

    // 2. Collect ALL Unique UI Entities (Panels, Text, Rects)
    std::unordered_set<uint64_t> uniqueEntities;
    for (Entity e: reg.GetEntitiesWith<GUI::UIComponents::UIRectComponent>()) {
        uniqueEntities.insert(e.Pack());
    }
    for (Entity e: reg.GetEntitiesWith<GUI::UIComponents::UIPanelComponent>()) {
        uniqueEntities.insert(e.Pack());
    }
    for (Entity e: reg.GetEntitiesWith<GUI::UIComponents::TextComponent>()) {
        uniqueEntities.insert(e.Pack());
    }

    if (uniqueEntities.empty()) {
        return;
    }

    struct SortEntry {
        Entity   entity;
        uint32_t depth;
        uint32_t order;
    };
    std::vector<SortEntry> sortedEntries;
    sortedEntries.reserve(uniqueEntities.size());

    for (uint64_t packed: uniqueEntities) {
        Entity e = Entity::Unpack(packed);
        if (!reg.IsAlive(e)) {
            continue;
        }

        uint32_t depth = 0;
        uint32_t order = 0;
        if (auto* rect = reg.Get<GUI::UIComponents::UIRectComponent>(e)) {
            depth = rect->hierarchyDepth;
            order = rect->layoutOrder;
        }

        sortedEntries.push_back({.entity = e, .depth = depth, .order = order});
    }

    // Sort ALL UI Entities by Hierarchy Depth (Ascending: parents and lower
    // layers first). Entries come out of an unordered_set, so same-depth ties
    // MUST break on layoutOrder or the draw order of overlapping windows is
    // nondeterministic from frame to frame -- panels and text of two windows
    // interleaving differently every run.
    std::ranges::sort(sortedEntries, [](const auto& a, const auto& b) -> auto {
        return (a.depth != b.depth) ? (a.depth < b.depth) : (a.order < b.order);
    });

    // ========================================================================
    // 3. PRE-PASS: TOP-DOWN MULTI-ANCESTOR SCISSOR PROPAGATION & INTERSECTION
    // ========================================================================
    HashMap<uint64_t, ScissorRect> activeScissors;

    for (const auto& entry: sortedEntries) {
        Entity e = entry.entity;
        if (IsEntityOrAncestorHidden(e)) {
            continue;
        }
        auto* rect = reg.Get<GUI::UIComponents::UIRectComponent>(e);
        if (rect == nullptr) {
            continue;
        }

        if (rect->parentEntity == Entity::Null() || !reg.IsAlive(rect->parentEntity)) {
            continue;
        }

        Entity      parent     = rect->parentEntity;
        const auto* parentRect = reg.Get<GUI::UIComponents::UIRectComponent>(parent);
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

    auto             uiSettingsEntities = reg.GetEntitiesWith<GUI::UIComponents::UISettingsComponent>();
    const FontAtlas* activeFont         = nullptr;
    if (!uiSettingsEntities.empty()) {
        activeFont = &reg.Get<GUI::UIComponents::UISettingsComponent>(uiSettingsEntities[0])->fontAtlas;
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

        auto* rect = reg.Get<GUI::UIComponents::UIRectComponent>(e);

        // A. Process Panel (if entity has UIPanelComponent)
        if (auto* panel = reg.Get<GUI::UIComponents::UIPanelComponent>(e)) {
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

        // A2. Process Image (if entity has UIImageComponent)
        // A dedicated primitive rather than a panel background: the scale mode
        // and the sub-UV region decide the quad's shape, and Tile needs one
        // quad per repeat, which the 54-vertex panel budget cannot express.
        if (auto* image = reg.Get<GUI::UIComponents::UIImageComponent>(e)) {
            if (rect != nullptr) {
                const uint32_t needed = GUI::CountImageVertices(*rect, *image);
                if (needed > 0) {
                    size_t startIdx = localPositions.size();
                    localPositions.resize(startIdx + needed);
                    localAttributes.resize(startIdx + needed);

                    uint32_t written = GUI::AppendImageVertices(&localPositions[startIdx], &localAttributes[startIdx], *rect, *image);

                    localPositions.resize(startIdx + written);
                    localAttributes.resize(startIdx + written);

                    currentVertexOffset += written;
                    QueueBatch(image->texture, written, useScissor, currentScissor, false);
                }
            }
        }

        // A3. Process Gradient (if entity has UIGradientComponent)
        // Drawn before the panel would hide it, and after it so a gradient
        // child sits on top of its parent's background: the colour picker's
        // saturation/value plane is a gradient child of a plain Box.
        if (auto* gradient = reg.Get<GUI::UIComponents::UIGradientComponent>(e)) {
            if (rect != nullptr) {
                const uint32_t needed = GUI::CountGradientVertices(*rect, *gradient);
                if (needed > 0) {
                    size_t startIdx = localPositions.size();
                    localPositions.resize(startIdx + needed);
                    localAttributes.resize(startIdx + needed);

                    uint32_t written = GUI::AppendGradientVertices(&localPositions[startIdx], &localAttributes[startIdx], *rect, *gradient);

                    localPositions.resize(startIdx + written);
                    localAttributes.resize(startIdx + written);

                    currentVertexOffset += written;
                    QueueBatch(TextureHandle::Invalid, written, useScissor, currentScissor, false);
                }
            }
        }

        // A4. Process Plot (if entity has UIPlotComponent)
        // A plot is pure geometry with no textured background, so it batches
        // with the other untextured UI (panels, gradients) rather than forcing
        // a texture bind.
        if (auto* plot = reg.Get<GUI::UIComponents::UIPlotComponent>(e)) {
            if (rect != nullptr) {
                const uint32_t needed = GUI::CountPlotVertices(*plot);
                if (needed > 0) {
                    size_t startIdx = localPositions.size();
                    localPositions.resize(startIdx + needed);
                    localAttributes.resize(startIdx + needed);

                    uint32_t written = GUI::AppendPlotVertices(&localPositions[startIdx], &localAttributes[startIdx], *rect, *plot);

                    localPositions.resize(startIdx + written);
                    localAttributes.resize(startIdx + written);

                    currentVertexOffset += written;
                    QueueBatch(TextureHandle::Invalid, written, useScissor, currentScissor, false);
                }
            }
        }

        // B. Process Text (if entity has TextComponent)
        if (auto* text = reg.Get<GUI::UIComponents::TextComponent>(e)) {
            float drawX = text->offsetX;
            float drawY = text->offsetY;

            std::string displayStr = text->text.c_str();

            // A TextInput's editable text lives on the dedicated `_ti_text`
            // leaf.  The label is a sibling of that leaf and must keep the
            // text stored in its own TextComponent (for example, "Profile")
            // instead of being replaced with the input value (for example,
            // "Default").
            bool isTextInputLeaf = false;
            if (const auto* name = reg.Get<Components::NameComponent>(e)) {
                isTextInputLeaf = std::string_view(name->name) == "_ti_text";
            }

            const GUI::UIComponents::UITextInputComponent* input = nullptr;
            if (isTextInputLeaf) {
                // The component is owned by the TextInput root.  Walk up the
                // small UI hierarchy to find it without affecting unrelated
                // text children that happen to be below the same root.
                input = reg.Get<GUI::UIComponents::UITextInputComponent>(e);
                if (input == nullptr && rect != nullptr) {
                    Entity cur = rect->parentEntity;
                    for (int hops = 0; hops < 4 && cur != Entity::Null() && reg.IsAlive(cur); ++hops) {
                        input = reg.Get<GUI::UIComponents::UITextInputComponent>(cur);
                        if (input != nullptr) {
                            break;
                        }
                        if (const auto* pr = reg.Get<GUI::UIComponents::UIRectComponent>(cur)) {
                            cur = pr->parentEntity;
                        } else {
                            break;
                        }
                    }
                }
            }

            if (isTextInputLeaf && input != nullptr) {
                std::string_view raw    = input->text;
                size_t           cursor = std::min<size_t>(input->cursorIndex, raw.size());
                if (input->isFocused) {
                    displayStr = std::string(raw.substr(0, cursor)) + "|" + std::string(raw.substr(cursor));
                } else {
                    displayStr = std::string(raw);
                }
            }

            float containerWidth  = 0.0f;
            float containerHeight = 0.0f;
            if (rect != nullptr) {
                containerWidth  = rect->computedAbsMaxX - rect->computedAbsMinX;
                containerHeight = rect->computedAbsMaxY - rect->computedAbsMinY;
            }

            // Wrap when the widget asks for it. The breaks come from the same
            // shaper the Yoga measure callback uses (GUI::WrapTextInto), so a
            // label is drawn exactly as tall and as wide as it was laid out.
            ZHLN::FixedString<GUI::kWrapBufferCapacity> wrappedText;
            std::string_view                            drawStr = displayStr;
            if (text->wrapText && activeFont != nullptr) {
                float wrapAt = text->wrapWidth;
                if (wrapAt <= 0.0f) {
                    wrapAt = containerWidth;
                }
                if (wrapAt > 0.0f) {
                    GUI::WrapTextInto(*activeFont, displayStr, text->scale, wrapAt, wrappedText);
                    drawStr = std::string_view(wrappedText);
                }
            }

            if (rect != nullptr) {
                TextBounds bounds = (activeFont != nullptr) ? GUI::MeasureTextBounds(*activeFont, drawStr, text->scale) : TextBounds {};

                // 1. Horizontal Alignment (Clamped to prevent left overflow)
                if (text->align == GUI::TextAlignment::Center) {
                    float centerOffset = std::max(0.0f, (containerWidth - bounds.width()) * 0.5f);
                    drawX              = rect->computedAbsMinX + centerOffset - bounds.minX + text->offsetX;
                } else if (text->align == GUI::TextAlignment::Right) {
                    float rightOffset = std::max(0.0f, containerWidth - bounds.width());
                    drawX             = rect->computedAbsMinX + rightOffset - bounds.minX + text->offsetX;
                } else {
                    drawX = rect->computedAbsMinX - bounds.minX + text->offsetX;
                }

                // 2. Vertical Alignment (Clamped to prevent top overflow)
                if (text->verticalAlign == GUI::TextVerticalAlignment::Center) {
                    float centerOffset = std::max(0.0f, (containerHeight - bounds.height()) * 0.5f);
                    drawY              = rect->computedAbsMinY + centerOffset - bounds.minY + text->offsetY;
                } else if (text->verticalAlign == GUI::TextVerticalAlignment::Bottom) {
                    float bottomOffset = std::max(0.0f, containerHeight - bounds.height());
                    drawY              = rect->computedAbsMinY + bottomOffset - bounds.minY + text->offsetY;
                } else {
                    drawY = rect->computedAbsMinY - bounds.minY + text->offsetY;
                }
            }

            if (activeFont != nullptr && !drawStr.empty()) {
                const auto   maxVertsNeeded = static_cast<uint32_t>(drawStr.size() * 6);
                const size_t startIdx       = localPositions.size();

                localPositions.resize(startIdx + maxVertsNeeded);
                localAttributes.resize(startIdx + maxVertsNeeded);

                // AppendTextVertices resets its pen to the block's left edge on
                // every '\n', which left-aligns each line inside the block. A
                // centred or right-aligned paragraph therefore has to be
                // emitted one line at a time, each with its own x.
                const bool multiLine   = drawStr.find('\n') != std::string_view::npos;
                const bool perLineDraw = multiLine && (text->align != GUI::TextAlignment::Left);

                uint32_t written = 0;
                if (!perLineDraw) {
                    written = GUI::AppendTextVertices(
                        &localPositions[startIdx], &localAttributes[startIdx], *activeFont, std::string(drawStr), drawX, drawY, text->scale, text->color
                    );
                } else {
                    const float lineHeight = GUI::TextLineHeight(text->scale);
                    float       lineY      = drawY;
                    size_t      pos        = 0;

                    while (pos <= drawStr.size()) {
                        const size_t     newline = drawStr.find('\n', pos);
                        const size_t     lineEnd = (newline == std::string_view::npos) ? drawStr.size() : newline;
                        std::string_view line    = drawStr.substr(pos, lineEnd - pos);

                        const TextBounds lineBounds = GUI::MeasureTextBounds(*activeFont, line, text->scale);
                        float            lineX      = drawX;
                        if (rect != nullptr) {
                            if (text->align == GUI::TextAlignment::Center) {
                                lineX = rect->computedAbsMinX + std::max(0.0f, (containerWidth - lineBounds.width()) * 0.5f) - lineBounds.minX + text->offsetX;
                            } else {
                                lineX = rect->computedAbsMinX + std::max(0.0f, containerWidth - lineBounds.width()) - lineBounds.minX + text->offsetX;
                            }
                        }

                        written += GUI::AppendTextVertices(
                            &localPositions[startIdx + written], &localAttributes[startIdx + written], *activeFont, std::string(line), lineX, lineY,
                            text->scale, text->color
                        );

                        if (newline == std::string_view::npos) {
                            break;
                        }
                        lineY += lineHeight;
                        pos = newline + 1;
                    }
                }

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
