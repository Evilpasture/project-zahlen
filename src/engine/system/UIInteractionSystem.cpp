// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UIInteractionSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>

namespace ZHLN {

void UIInteractionSystem::Update(Engine& engine, float dt) {
    auto& reg   = engine.GetRegistry();
    auto& input = engine.GetInput();
    auto  mouse = input.GetMouse();

    auto IsEntityOrAncestorHidden = [&](Entity ent) -> bool {
        Entity curr = ent;
        while (curr != NullEntity && reg.IsAlive(curr)) {
            if (auto* mesh = reg.Get<Components::MeshComponent>(curr)) {
                if ((mesh->flags & DrawFlags::Hidden) != DrawFlags::None) {
                    return true;
                }
            }
            auto* rect = reg.Get<Components::UIRectComponent>(curr);
            curr       = (rect != nullptr) ? rect->parentEntity : NullEntity;
        }
        return false;
    };

    auto entities = reg.GetEntitiesWith<Components::UIRectComponent>();
    auto rects    = reg.GetRawArray<Components::UIRectComponent>();

    if (entities.empty()) {
        return;
    }

    bool leftMouseDown = input.IsMouseButtonDown(KeyCode::LButton);

    // 1. Process active dragging
    for (Entity e: reg.GetEntitiesWith<Components::UIDragComponent>()) {
        auto* drag = reg.Get<Components::UIDragComponent>(e);
        if (drag->isDragging) {
            if (!leftMouseDown) {
                drag->isDragging = false;
            } else {
                if (auto* targetRect = reg.Get<Components::UIRectComponent>(drag->targetEntity)) {
                    targetRect->x += mouse.deltaX;
                    targetRect->y += mouse.deltaY;
                }
            }
        }
    }

    // 2. Sort elements by depth descending to handle overlaps (Deeper children processed first)
    struct SortEntry {
        size_t   rawIndex;
        uint32_t depth;
    };
    JPH::Array<SortEntry> sortedEntries;
    sortedEntries.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        sortedEntries.push_back({.rawIndex = i, .depth = rects[i].hierarchyDepth});
    }
    std::ranges::sort(sortedEntries, [](const auto& a, const auto& b) { return a.depth > b.depth; });

    bool clickConsumed = false;
    bool focusCaptured = false;

    for (const auto& entry: sortedEntries) {
        Entity      e      = entities[entry.rawIndex];
        const auto& rect   = rects[entry.rawIndex];
        auto*       button = reg.Get<Components::UIButtonComponent>(e);

        if (button == nullptr || IsEntityOrAncestorHidden(e)) {
            if (button != nullptr) {
                button->Set(UIButton::Hovered, false);
                button->Set(UIButton::Pressed, false);
            }
            continue;
        }

        button->Set(UIButton::Clicked, false);

        if (button->Has(UIButton::Disabled)) {
            button->Set(UIButton::Hovered, false);
            button->Set(UIButton::Pressed, false);
            continue;
        }

        if (clickConsumed) {
            button->Set(UIButton::Hovered, false);
            button->Set(UIButton::Pressed, false);
            continue;
        }

        bool inside =
            (mouse.x >= rect.computedAbsMinX && mouse.x <= rect.computedAbsMaxX && mouse.y >= rect.computedAbsMinY && mouse.y <= rect.computedAbsMaxY);

        if (inside) {
            button->Set(UIButton::Hovered, true);
            if (leftMouseDown) {
                button->Set(UIButton::Pressed, true);

                if (auto* drag = reg.Get<Components::UIDragComponent>(e)) {
                    drag->isDragging = true;
                }

                if (reg.Get<Components::UITextInputComponent>(e) != nullptr) {
                    focusCaptured = true;
                    for (Entity other: reg.GetEntitiesWith<Components::UITextInputComponent>()) {
                        if (auto* inputComp = reg.Get<Components::UITextInputComponent>(other)) {
                            inputComp->isFocused = (other == e);
                        }
                    }
                }
            } else {
                if (button->Has(UIButton::Pressed)) {
                    button->Set(UIButton::Clicked, true);
                    clickConsumed = true;
                }
                button->Set(UIButton::Pressed, false);
            }
        } else {
            button->Set(UIButton::Hovered, false);
            if (!leftMouseDown) {
                button->Set(UIButton::Pressed, false);
            }
        }
    }

    if (leftMouseDown && !focusCaptured) {
        for (Entity e: reg.GetEntitiesWith<Components::UITextInputComponent>()) {
            if (auto* inputComp = reg.Get<Components::UITextInputComponent>(e)) {
                inputComp->isFocused = false;
            }
        }
    }

    // 3. Process State-Driven Style Transitions
    for (Entity e: reg.GetEntitiesWith<Components::UIStyleComponent>()) {
        auto* style = reg.Get<Components::UIStyleComponent>(e);
        auto* btn   = reg.Get<Components::UIButtonComponent>(e);
        if (style == nullptr) {
            continue;
        }

        JPH::Vec4 targetPanelColor = style->normalColor;
        JPH::Vec4 targetTextColor  = style->textColorNormal;

        if (btn != nullptr) {
            if (btn->Has(UIButton::Disabled)) {
                targetPanelColor = style->disabledColor;
            } else if (btn->Has(UIButton::Pressed)) {
                targetPanelColor = style->pressedColor;
                targetTextColor  = style->textColorPressed;
            } else if (btn->Has(UIButton::Hovered)) {
                targetPanelColor = style->hoverColor;
                targetTextColor  = style->textColorHover;
            }
        }

        // Animate Panel Color
        if (auto* panel = reg.Get<Components::UIPanelComponent>(e)) {
            if (style->transitionSpeed > 0.0f) {
                float factor = std::clamp(style->transitionSpeed * dt, 0.0f, 1.0f);
                panel->color = panel->color + factor * (targetPanelColor - panel->color);
            } else {
                panel->color = targetPanelColor;
            }
        }

        // Animate Text Color (on self)
        if (style->hasTextColor) {
            auto* text = reg.Get<Components::TextComponent>(e);
            if (text != nullptr) {
                if (style->transitionSpeed > 0.0f) {
                    float factor = std::clamp(style->transitionSpeed * dt, 0.0f, 1.0f);
                    text->color  = text->color + factor * (targetTextColor - text->color);
                } else {
                    text->color = targetTextColor;
                }
            }
        }
    }
}

} // namespace ZHLN
