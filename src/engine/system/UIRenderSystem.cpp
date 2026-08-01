// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UIRenderSystem.hpp"
#include "UILayoutSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Window.hpp>
#include <algorithm>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

namespace {
struct UIPanelCacheEntry {
    Mesh  posMesh {};
    float lastX = -1e30f;
    float lastY = -1e30f;
};

struct UITextCacheEntry {
    Mesh        posMesh {};
    float       lastX = -1e30f;
    float       lastY = -1e30f;
    std::string textCache;
};

static ZHLN::HashMap<uint64_t, UIPanelCacheEntry> s_UIPanelMeshes;
static ZHLN::HashMap<uint64_t, UITextCacheEntry>  s_UITextMeshes;
} // namespace

void UIRenderSystem::Update(Engine& engine) {
    auto& reg        = engine.GetRegistry();
    auto& rc         = engine.GetRenderContext();
    auto  windowSize = engine.GetWindow().GetSize();

    if (windowSize.width == 0 || windowSize.height == 0) {
        return;
    }

    UILayoutSystem layoutSystem;
    layoutSystem.ResolveLayouts(reg, {.width = (float) windowSize.width, .height = (float) windowSize.height});

    auto entities = reg.GetEntitiesWith<Components::UIRectComponent>();
    auto rects    = reg.GetRawArray<Components::UIRectComponent>();

    struct SortEntry {
        size_t   rawIndex;
        uint32_t depth;
    };
    JPH::Array<SortEntry> sortedEntries;
    sortedEntries.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        sortedEntries.push_back({.rawIndex = i, .depth = rects[i].hierarchyDepth});
    }
    std::ranges::sort(sortedEntries, [](const auto& a, const auto& b) { return a.depth < b.depth; });

    HashMap<uint64_t, ScissorRect> activeScissors;

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
            const auto* cache = s_UIPanelMeshes.Find(e.Pack());
            Mesh        panelMesh {};

            if (!cache || cache->posMesh.posBuffer == BufferHandle::Invalid || cache->lastX != rect.computedAbsMinX || cache->lastY != rect.computedAbsMinY) {
                if (cache && cache->posMesh.posBuffer != BufferHandle::Invalid) {
                    rc.DestroyBuffer(cache->posMesh.posBuffer);
                    rc.DestroyBuffer(cache->posMesh.attrBuffer);
                }
                panelMesh = GUI::CreatePanelMesh(rc, rect, *panel);
                s_UIPanelMeshes.Insert(e.Pack(), UIPanelCacheEntry {.posMesh = panelMesh, .lastX = rect.computedAbsMinX, .lastY = rect.computedAbsMinY});
            } else {
                panelMesh = cache->posMesh;
            }
            Renderer::DrawUI(rc, panelMesh, panel->textureIndex, useScissor, currentScissor);
        }
    }

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

        const auto* cache = s_UITextMeshes.Find(e.Pack());
        Mesh        textMesh {};

        if (!cache || cache->posMesh.posBuffer == BufferHandle::Invalid || cache->lastX != drawX || cache->lastY != drawY || cache->textCache != displayStr) {
            if (cache && cache->posMesh.posBuffer != BufferHandle::Invalid) {
                rc.DestroyBuffer(cache->posMesh.posBuffer);
                rc.DestroyBuffer(cache->posMesh.attrBuffer);
            }
            if (activeFont != nullptr && !displayStr.empty()) {
                textMesh = GUI::CreateTextMesh(rc, *activeFont, displayStr, drawX, drawY, text->scale, text->color);
                s_UITextMeshes.Insert(e.Pack(), UITextCacheEntry {.posMesh = textMesh, .lastX = drawX, .lastY = drawY, .textCache = displayStr});
            }
        } else {
            textMesh = cache->posMesh;
        }

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

        if (textMesh.posBuffer != BufferHandle::Invalid) {
            Renderer::DrawUI(rc, textMesh, text->fontIndex, useScissor, currentScissor);
        }
    }
}

} // namespace ZHLN
