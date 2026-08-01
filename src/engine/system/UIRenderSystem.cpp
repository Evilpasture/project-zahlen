#include "UIRenderSystem.hpp"
#include "UILayoutSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cstring>

namespace ZHLN {

void UIRenderSystem::Update(Engine& engine) {
    auto& reg        = engine.GetRegistry();
    auto  windowSize = engine.GetWindow().GetSize();

    if (windowSize.width == 0 || windowSize.height == 0) {
        return;
    }

    UILayoutSystem layoutSystem;
    layoutSystem.ResolveLayouts(reg, {.width = (float) windowSize.width, .height = (float) windowSize.height});

    auto entities = reg.GetEntitiesWith<Components::UIRectComponent>();
    auto rects    = reg.GetRawArray<Components::UIRectComponent>();

    if (entities.empty()) {
        return;
    }

    struct SortEntry {
        size_t   rawIndex;
        uint32_t depth;
    };
    std::vector<SortEntry> sortedEntries;
    sortedEntries.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        sortedEntries.push_back({.rawIndex = i, .depth = rects[i].hierarchyDepth});
    }
    std::ranges::sort(sortedEntries, [](const auto& a, const auto& b) { return a.depth < b.depth; });

    // CPU-side staging arrays (Zero allocation overhead after warming up)
    ZHLN::Array<VertexPosition>   localPositions;
    ZHLN::Array<VertexAttributes> localAttributes;
    ZHLN::Array<UIBatch>          localBatches;

    localPositions.reserve(4096);
    localAttributes.reserve(4096);
    localBatches.reserve(128);

    uint32_t                       currentVertexOffset = 0;
    HashMap<uint64_t, ScissorRect> activeScissors;

    auto QueueBatch = [&](uint32_t textureIdx, uint32_t count, bool useScissor, ScissorRect scissor, bool isSDF = false) {
        if (count == 0) {
            return;
        }

        if (!localBatches.empty()) {
            auto& last = localBatches.back();
            if (last.textureIndex == textureIdx && last.isSDF == isSDF && last.useScissor == useScissor &&
                (!useScissor || (std::memcmp(&last.scissorRect, &scissor, sizeof(ScissorRect)) == 0))) {
                last.vertexCount += count;
                return;
            }
        }

        localBatches.push_back(
            {.textureIndex = textureIdx,
             .vertexStart  = currentVertexOffset - count,
             .vertexCount  = count,
             .useScissor   = useScissor,
             .isSDF        = isSDF,
             .scissorRect  = scissor}
        );
    };

    // --- Generate Panel Vertices ---
    for (const auto& entry: sortedEntries) {
        Entity      e    = entities[entry.rawIndex];
        const auto& rect = rects[entry.rawIndex];

        bool        parentHasScissor = false;
        ScissorRect parentScissor {};

        if (rect.parentEntity != NullEntity && reg.IsAlive(rect.parentEntity)) {
            const ScissorRect* parentScissorPtr = activeScissors.Find(rect.parentEntity.Pack());
            if (parentScissorPtr != nullptr) {
                parentScissor    = *parentScissorPtr;
                parentHasScissor = true;
            }
        }

        bool        useScissor     = parentHasScissor;
        ScissorRect currentScissor = parentScissor;

        if (rect.parentEntity != NullEntity && reg.IsAlive(rect.parentEntity)) {
            if (auto* parentRect = reg.Get<Components::UIRectComponent>(rect.parentEntity)) {
                if (parentRect->clipChildren) {
                    ScissorRect parentClip = {
                        .x      = (int32_t) std::max(0.0f, parentRect->computedAbsMinX),
                        .y      = (int32_t) std::max(0.0f, parentRect->computedAbsMinY),
                        .width  = (uint32_t) std::max(0.0f, parentRect->computedAbsMaxX - parentRect->computedAbsMinX),
                        .height = (uint32_t) std::max(0.0f, parentRect->computedAbsMaxY - parentRect->computedAbsMinY)
                    };

                    if (!useScissor) {
                        currentScissor = parentClip;
                        useScissor     = true;
                    } else {
                        int32_t x0 = std::max(currentScissor.x, parentClip.x);
                        int32_t y0 = std::max(currentScissor.y, parentClip.y);
                        int32_t x1 = std::min(currentScissor.x + (int32_t) currentScissor.width, parentClip.x + (int32_t) parentClip.width);
                        int32_t y1 = std::min(currentScissor.y + (int32_t) currentScissor.height, parentClip.y + (int32_t) parentClip.height);

                        currentScissor.x      = x0;
                        currentScissor.y      = y0;
                        currentScissor.width  = std::max(0, x1 - x0);
                        currentScissor.height = std::max(0, y1 - y0);
                    }
                }
            }
        }

        if (useScissor) {
            activeScissors.Insert(e.Pack(), currentScissor);
        }

        if (auto* panel = reg.Get<Components::UIPanelComponent>(e)) {
            uint32_t maxNeeded = 54; // Max vertices for 9-slice panel
            if (currentVertexOffset + maxNeeded >= 100000) {
                break;
            }

            size_t startIdx = localPositions.size();
            localPositions.resize(startIdx + maxNeeded);
            localAttributes.resize(startIdx + maxNeeded);

            uint32_t written = GUI::AppendPanelVertices(&localPositions[startIdx], &localAttributes[startIdx], rect, *panel);

            localPositions.resize(startIdx + written);
            localAttributes.resize(startIdx + written);

            currentVertexOffset += written;
            QueueBatch(panel->textureIndex, written, useScissor, currentScissor);
        }
    }

    // --- Generate Text Vertices ---
    auto             uiSettingsEntities = reg.GetEntitiesWith<Components::UISettingsComponent>();
    const FontAtlas* activeFont         = nullptr;
    if (!uiSettingsEntities.empty()) {
        activeFont = &reg.Get<Components::UISettingsComponent>(uiSettingsEntities[0])->fontAtlas;
    }

    for (Entity e: reg.GetEntitiesWith<Components::TextComponent>()) {
        auto* text    = reg.Get<Components::TextComponent>(e);
        float drawX   = text->x;
        float drawY   = text->y;
        bool  hasRect = false;

        auto* rect = reg.Get<Components::UIRectComponent>(e);
        if (rect != nullptr) {
            drawX   = rect->computedAbsMinX + text->x;
            drawY   = rect->computedAbsMinY + text->y;
            hasRect = true;
        }

        std::string displayStr = text->text.c_str();
        if (auto* input = reg.Get<Components::UITextInputComponent>(e)) {
            std::string_view raw = input->text;
            if (input->isFocused) {
                displayStr = std::string(raw.substr(0, input->cursorIndex)) + "|" + std::string(raw.substr(input->cursorIndex));
            } else {
                displayStr = std::string(raw);
            }
        }

        if (activeFont != nullptr && !displayStr.empty()) {
            auto maxVertsNeeded = static_cast<uint32_t>(displayStr.length() * 6);
            if (currentVertexOffset + maxVertsNeeded >= 100000) {
                break;
            }

            size_t startIdx = localPositions.size();
            localPositions.resize(startIdx + maxVertsNeeded);
            localAttributes.resize(startIdx + maxVertsNeeded);

            uint32_t written =
                GUI::AppendTextVertices(&localPositions[startIdx], &localAttributes[startIdx], *activeFont, displayStr, drawX, drawY, text->scale, text->color);

            localPositions.resize(startIdx + written);
            localAttributes.resize(startIdx + written);

            currentVertexOffset += written;

            bool        useScissor = false;
            ScissorRect currentScissor {};

            if (hasRect) {
                const ScissorRect* parentScissorPtr = activeScissors.Find(rect->parentEntity.Pack());
                if (parentScissorPtr != nullptr) {
                    currentScissor = *parentScissorPtr;
                    useScissor     = true;
                } else if (rect->parentEntity != NullEntity && reg.IsAlive(rect->parentEntity)) {
                    if (auto* parentRect = reg.Get<Components::UIRectComponent>(rect->parentEntity)) {
                        if (parentRect->clipChildren) {
                            currentScissor = {
                                .x      = (int32_t) std::max(0.0f, parentRect->computedAbsMinX),
                                .y      = (int32_t) std::max(0.0f, parentRect->computedAbsMinY),
                                .width  = (uint32_t) std::max(0.0f, parentRect->computedAbsMaxX - parentRect->computedAbsMinX),
                                .height = (uint32_t) std::max(0.0f, parentRect->computedAbsMaxY - parentRect->computedAbsMinY)
                            };
                            useScissor = true;
                        }
                    }
                }
            }

            QueueBatch(text->fontIndex, written, useScissor, currentScissor, true);
        }
    }

    // Submit the data cleanly through the public boundary API
    engine.GetRenderContext().SubmitUI(
        localBatches.data(), static_cast<uint32_t>(localBatches.size()), localPositions.data(), localAttributes.data(),
        static_cast<uint32_t>(localPositions.size())
    );
}

} // namespace ZHLN
