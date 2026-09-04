// src/gui/UIInteractionSystem.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UIInteractionSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <algorithm>

namespace ZHLN {

namespace {

// Walk up the parent chain to find an ancestor with a given component.
template <typename Comp>
auto FindAncestorWith(ECS::Registry& reg, Entity start) -> Entity {
    Entity curr = start;
    while (curr != Entity::Null() && reg.IsAlive(curr)) {
        if (reg.Get<Comp>(curr) != nullptr) {
            return curr;
        }
        if (auto* rect = reg.Get<GUI::UIComponents::UIRectComponent>(curr)) {
            curr = rect->parentEntity;
        } else {
            break;
        }
    }
    return Entity::Null();
}

// Compute on-screen bounds for a rect (absolute min/max).
struct Bounds {
    float x0, y0, x1, y1;
};

auto GetBounds(const GUI::UIComponents::UIRectComponent& r) -> Bounds {
    return {r.computedAbsMinX, r.computedAbsMinY, r.computedAbsMaxX, r.computedAbsMaxY};
}

// Point-in-rect hit testing lives in GUI::IsPointVisible, which also walks the
// ancestor clip chain — a widget clipped away by a panel or scrolled out of a
// viewport must not swallow hover or clicks.

} // namespace

void UIInteractionSystem::Update(Engine& engine, float dt) {
    auto& reg = engine.GetRegistry();

    auto* state = reg.GetSingleton<Components::InputStateComponent>();
    if (state == nullptr) {
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

    auto entities = reg.GetEntitiesWith<GUI::UIComponents::UIRectComponent>();
    auto rects    = reg.GetRawArray<GUI::UIComponents::UIRectComponent>();

    if (entities.empty()) {
        return;
    }

    // Native ECS UI uses raw button state so ImGui capture does not block in-world panels.
    bool  leftMouseDown = state->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton));
    float mouseX        = state->mouseX;
    float mouseY        = state->mouseY;
    float deltaX        = state->mouseDeltaX;
    float deltaY        = state->mouseDeltaY;
    float wheel         = state->mouseWheel;

    // [UI-SCROLLING] Mouse wheel dispatch + smooth scroll integration.
    //
    // The wheel goes to the innermost scrollable under the pointer (deepest
    // hierarchyDepth wins, so a scroller nested in a scroller scrolls itself),
    // and every viewport eases towards its target — including the ones that
    // are not hovered, so a fling that started last frame still settles.
    //
    // Content extents (maxScrollX/Y) are measured in the layout pass, which
    // runs after this one, so the clamp here uses last frame's extent. That is
    // the same one-frame lag the scroll offset itself has, and it is why
    // ApplyScrollInput clamps against the component's stored range instead of
    // recomputing it from geometry.
    GUI::ApplyScrollInput(reg, GUI::ScrollInput {.mouseX = mouseX, .mouseY = mouseY, .wheelDelta = wheel, .deltaTime = dt});

    // Reset per-frame hover on every UIButtonComponent in one pass. The
    // UIButtonComponent::Hovered flag is the single source of truth for hover
    // across all widgets; compound widgets never cache their own hovered flag.
    for (auto& btn: reg.GetRawArray<GUI::UIComponents::UIButtonComponent>()) {
        btn.Set(GUI::UIButton::Hovered, false);
        btn.Set(GUI::UIButton::Clicked, false);
    }

    // 1. Process active drag operations (window dragging + slider + splitter)
    // Window/Panel drags
    for (Entity e: reg.GetEntitiesWith<GUI::UIComponents::UIDragComponent>()) {
        if (auto* drag = reg.Get<GUI::UIComponents::UIDragComponent>(e)) {
            // Determine whether this drag handle belongs to a slider or splitter
            // by walking up parents.
            Entity sliderEnt   = FindAncestorWith<GUI::UIComponents::UISliderComponent>(reg, e);
            Entity splitterEnt = FindAncestorWith<GUI::UIComponents::UISplitterComponent>(reg, e);

            if (sliderEnt != Entity::Null()) {
                // Slider drag: adjust slider value
                if (auto* slider = reg.Get<GUI::UIComponents::UISliderComponent>(sliderEnt)) {
                    if (!leftMouseDown) {
                        // Release BOTH latches. Forgetting the drag-component
                        // latch here made the handle stay "armed" after the
                        // first drag: the next left press anywhere on the
                        // screen re-activated the slider and the knob jumped
                        // to follow an unrelated click -- the "sticky slider".
                        slider->isDragging = false;
                        drag->isDragging   = false;
                    } else if (drag->isDragging || slider->isDragging) {
                        if (!slider->isDragging) {
                            slider->isDragging = true;
                        }
                        // Find the _sl_track child to get the track's on-screen bounds
                        Bounds trackB {};
                        bool   hasBounds = false;
                        if (auto* cache = reg.Get<GUI::UIComponents::UIChildCacheComponent>(sliderEnt)) {
                            cache->children.ForEach([&](uint64_t, const GUI::UIComponents::UIChildCacheComponent::ChildRecord& rec) -> void {
                                if (hasBounds)
                                    return;
                                Entity child = rec.entity;
                                if (!reg.IsAlive(child))
                                    return;
                                if (auto* cname = reg.Get<Components::NameComponent>(child)) {
                                    if (std::string_view(cname->name) == "_sl_track") {
                                        if (auto* tr = reg.Get<GUI::UIComponents::UIRectComponent>(child)) {
                                            trackB    = GetBounds(*tr);
                                            hasBounds = true;
                                        }
                                    }
                                }
                            });
                        }
                        if (!hasBounds) {
                            if (auto* srect = reg.Get<GUI::UIComponents::UIRectComponent>(sliderEnt)) {
                                trackB    = GetBounds(*srect);
                                hasBounds = true;
                            }
                        }
                        if (hasBounds) {
                            float width = trackB.x1 - trackB.x0;
                            if (width > 1.0f) {
                                float rel = (mouseX - trackB.x0) / width;
                                float v   = slider->minValue + rel * (slider->maxValue - slider->minValue);
                                if (slider->step > 0.0f) {
                                    v = std::round(v / slider->step) * slider->step;
                                }
                                v             = std::clamp(v, slider->minValue, slider->maxValue);
                                slider->value = v;
                            }
                        }
                        continue;
                    }
                }
            } else if (splitterEnt != Entity::Null()) {
                // Splitter drag: adjust ratio
                if (auto* split = reg.Get<GUI::UIComponents::UISplitterComponent>(splitterEnt)) {
                    if (!leftMouseDown) {
                        split->isDragging = false;
                        drag->isDragging  = false; // same latch leak as the slider
                    } else if (drag->isDragging || split->isDragging) {
                        if (!split->isDragging) {
                            split->isDragging = true;
                        }
                        if (auto* srect = reg.Get<GUI::UIComponents::UIRectComponent>(splitterEnt)) {
                            Bounds tb         = GetBounds(*srect);
                            bool   horizontal = (split->direction == GUI::UIComponents::UISplitterComponent::Horizontal);
                            float  total      = horizontal ? (tb.x1 - tb.x0) : (tb.y1 - tb.y0);
                            float  pos        = horizontal ? (mouseX - tb.x0) : (mouseY - tb.y0);
                            if (total > 1.0f) {
                                float r = pos / total;
                                // Respect minSize — clamp so each side is >= minSize / total
                                float minRatio = 0.05f;
                                float maxRatio = 0.95f;
                                r              = std::clamp(r, minRatio, maxRatio);
                                split->ratio   = r;
                            }
                        }
                        continue;
                    }
                }
            } else {
                // Regular panel drag
                if (drag->isDragging) {
                    if (!leftMouseDown) {
                        drag->isDragging = false;
                    } else {
                        if (auto* targetRect = reg.Get<GUI::UIComponents::UIRectComponent>(drag->targetEntity)) {
                            targetRect->x += deltaX;
                            targetRect->y += deltaY;
                        }
                    }
                }
            }
        }
    }

    // 2. Sort elements by depth descending to handle overlaps (Deeper children processed first)
    struct SortEntry {
        size_t   rawIndex;
        uint32_t depth;
        uint32_t order;
    };
    JPH::Array<SortEntry> sortedEntries;
    sortedEntries.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        sortedEntries.push_back({.rawIndex = i, .depth = rects[i].hierarchyDepth, .order = rects[i].layoutOrder});
    }
    // Deepest first; at equal depth the widget drawn LAST (highest layoutOrder)
    // is on top and must win the hit test -- the same order the render pass
    // draws in, so what you see on top is what you click.
    std::ranges::sort(sortedEntries, [](const auto& a, const auto& b) {
        return (a.depth != b.depth) ? (a.depth > b.depth) : (a.order > b.order);
    });

    bool   clickConsumed   = false;
    bool   focusCaptured   = false;
    Entity clickedDropdown = Entity::Null();

    for (const auto& entry: sortedEntries) {
        Entity e      = entities[entry.rawIndex];
        auto*  button = reg.Get<GUI::UIComponents::UIButtonComponent>(e);

        bool hidden = IsEntityOrAncestorHidden(e);

        if (button == nullptr || hidden) {
            if (button != nullptr) {
                button->Set(GUI::UIButton::Hovered, false);
                button->Set(GUI::UIButton::Pressed, false);
            }
            continue;
        }

        button->Set(GUI::UIButton::Clicked, false);

        if (button->Has(GUI::UIButton::Disabled)) {
            button->Set(GUI::UIButton::Hovered, false);
            button->Set(GUI::UIButton::Pressed, false);
            continue;
        }

        // Hit-test through the clip chain, not just the widget's own rect: a
        // row scrolled out of a ScrollBox viewport (or clipped by a panel with
        // clipChildren) is invisible, and an invisible widget must not swallow
        // hover or clicks. IsPointVisible walks the ancestors that clip.
        const bool inside = GUI::IsPointVisible(reg, e, mouseX, mouseY);

        if (clickConsumed) {
            button->Set(GUI::UIButton::Hovered, false);
            button->Set(GUI::UIButton::Pressed, false);
            continue;
        }

        if (inside) {
            button->Set(GUI::UIButton::Hovered, true);
            if (leftMouseDown) {
                button->Set(GUI::UIButton::Pressed, true);

                // Click-to-set on sliders: start drag and jump value to click position
                if (Entity slEnt = FindAncestorWith<GUI::UIComponents::UISliderComponent>(reg, e); slEnt != Entity::Null()) {
                    if (auto* slider = reg.Get<GUI::UIComponents::UISliderComponent>(slEnt)) {
                        slider->isDragging = true;
                        // Find track bounds
                        Bounds trackB {};
                        bool   hasBounds = false;
                        if (auto* cache = reg.Get<GUI::UIComponents::UIChildCacheComponent>(slEnt)) {
                            cache->children.ForEach([&](uint64_t, const GUI::UIComponents::UIChildCacheComponent::ChildRecord& rec) -> void {
                                if (hasBounds)
                                    return;
                                Entity child = rec.entity;
                                if (!reg.IsAlive(child))
                                    return;
                                if (auto* cname = reg.Get<Components::NameComponent>(child)) {
                                    if (std::string_view(cname->name) == "_sl_track") {
                                        if (auto* tr = reg.Get<GUI::UIComponents::UIRectComponent>(child)) {
                                            trackB    = GetBounds(*tr);
                                            hasBounds = true;
                                        }
                                    }
                                }
                            });
                        }
                        if (!hasBounds) {
                            if (auto* srect = reg.Get<GUI::UIComponents::UIRectComponent>(slEnt)) {
                                trackB    = GetBounds(*srect);
                                hasBounds = true;
                            }
                        }
                        if (hasBounds) {
                            float width = trackB.x1 - trackB.x0;
                            if (width > 1.0f) {
                                float rel = (mouseX - trackB.x0) / width;
                                float v   = slider->minValue + rel * (slider->maxValue - slider->minValue);
                                if (slider->step > 0.0f) {
                                    v = std::round(v / slider->step) * slider->step;
                                }
                                v             = std::clamp(v, slider->minValue, slider->maxValue);
                                slider->value = v;
                            }
                        }
                    }
                }

                // Splitter handle: begin drag
                if (Entity spEnt = FindAncestorWith<GUI::UIComponents::UISplitterComponent>(reg, e); spEnt != Entity::Null()) {
                    const auto* handleDrag = reg.Get<GUI::UIComponents::UIDragComponent>(e);
                    if (handleDrag != nullptr && handleDrag->targetEntity == spEnt) {
                        if (auto* sp = reg.Get<GUI::UIComponents::UISplitterComponent>(spEnt)) {
                            sp->isDragging = true;
                        }
                    }
                }

                // Dropdown: clicking anywhere inside an open dropdown (but not an
                // item) shouldn't auto-close; clicking the header toggles.
                if (Entity ddEnt = FindAncestorWith<GUI::UIComponents::UIDropdownComponent>(reg, e); ddEnt != Entity::Null()) {
                    clickedDropdown = ddEnt;
                }

                // The menu itself lives on the overlay root, so the dropdown is
                // NOT an ancestor of its own option rows. Resolve the owner
                // through UIPopupComponent; without this, clicking an option
                // reads as a click "outside every dropdown" and the menu closes
                // in the same frame the selection would have been applied.
                if (Entity popEnt = FindAncestorWith<GUI::UIComponents::UIPopupComponent>(reg, e); popEnt != Entity::Null()) {
                    if (const auto* pop = reg.Get<GUI::UIComponents::UIPopupComponent>(popEnt)) {
                        if (reg.Get<GUI::UIComponents::UIDropdownComponent>(pop->owner) != nullptr) {
                            clickedDropdown = pop->owner;
                        }
                    }
                }

                if (auto* drag = reg.Get<GUI::UIComponents::UIDragComponent>(e)) {
                    drag->isDragging = true;
                }

                if (reg.Get<GUI::UIComponents::UITextInputComponent>(e) != nullptr) {
                    focusCaptured = true;
                    for (Entity other: reg.GetEntitiesWith<GUI::UIComponents::UITextInputComponent>()) {
                        if (auto* inputComp = reg.Get<GUI::UIComponents::UITextInputComponent>(other)) {
                            const bool nowFocused = (other == e);
                            if (nowFocused && !inputComp->isFocused) {
                                // Focus gain selects the pre-focus content so
                                // typing replaces it instead of appending to it.
                                inputComp->selectAll   = true;
                                inputComp->cursorIndex = 0;
                            }
                            inputComp->isFocused = nowFocused;
                        }
                    }
                }
            } else {
                if (button->Has(GUI::UIButton::Pressed)) {
                    button->Set(GUI::UIButton::Clicked, true);
                    clickConsumed = true;
                }
                button->Set(GUI::UIButton::Pressed, false);
            }
        } else {
            button->Set(GUI::UIButton::Hovered, false);
            if (!leftMouseDown) {
                button->Set(GUI::UIButton::Pressed, false);
            }
        }
    }

    // Close expanded dropdowns when clicking outside any dropdown.
    if (leftMouseDown && clickedDropdown == Entity::Null()) {
        for (Entity e: reg.GetEntitiesWith<GUI::UIComponents::UIDropdownComponent>()) {
            if (auto* dd = reg.Get<GUI::UIComponents::UIDropdownComponent>(e)) {
                dd->expanded = false;
            }
        }
    }

    // Focus handling for text inputs (defocus when clicking away)
    if (leftMouseDown && !focusCaptured) {
        for (Entity e: reg.GetEntitiesWith<GUI::UIComponents::UITextInputComponent>()) {
            if (auto* inputComp = reg.Get<GUI::UIComponents::UITextInputComponent>(e)) {
                inputComp->isFocused = false;
            }
        }
    }

    // 3. Process State-Driven Style Transitions
    for (Entity e: reg.GetEntitiesWith<GUI::UIComponents::UIStyleComponent>()) {
        auto* style = reg.Get<GUI::UIComponents::UIStyleComponent>(e);
        auto* btn   = reg.Get<GUI::UIComponents::UIButtonComponent>(e);
        if (style == nullptr) {
            continue;
        }

        JPH::Vec4 targetPanelColor = style->normalColor;
        JPH::Vec4 targetTextColor  = style->textColorNormal;

        if (btn != nullptr) {
            if (btn->Has(GUI::UIButton::Disabled)) {
                targetPanelColor = style->disabledColor;
            } else if (btn->Has(GUI::UIButton::Pressed)) {
                targetPanelColor = style->pressedColor;
                targetTextColor  = style->textColorPressed;
            } else if (btn->Has(GUI::UIButton::Hovered)) {
                targetPanelColor = style->hoverColor;
                targetTextColor  = style->textColorHover;
            }
        }

        // Animate Panel Color
        if (auto* panel = reg.Get<GUI::UIComponents::UIPanelComponent>(e)) {
            if (style->transitionSpeed > 0.0f) {
                float factor = std::clamp(style->transitionSpeed * dt, 0.0f, 1.0f);
                panel->color = panel->color + factor * (targetPanelColor - panel->color);
            } else {
                panel->color = targetPanelColor;
            }
        }

        // Animate Text Color (on self)
        if (style->hasTextColor) {
            auto* text = reg.Get<GUI::UIComponents::TextComponent>(e);
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
