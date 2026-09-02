// src/engine/system/UIInteractionSystem.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "UIInteractionSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/ecs/ECS.hpp>
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
        if (auto* rect = reg.Get<Components::UIRectComponent>(curr)) {
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

auto GetBounds(const Components::UIRectComponent& r) -> Bounds {
    return {r.computedAbsMinX, r.computedAbsMinY, r.computedAbsMaxX, r.computedAbsMaxY};
}

auto Inside(Bounds b, float x, float y) -> bool {
    return x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1;
}

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
            auto* rect = reg.Get<Components::UIRectComponent>(curr);
            curr       = (rect != nullptr) ? rect->parentEntity : Entity::Null();
        }
        return false;
    };

    auto entities = reg.GetEntitiesWith<Components::UIRectComponent>();
    auto rects    = reg.GetRawArray<Components::UIRectComponent>();

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

    // TODO(Evilpasture): [UI-SCROLLING] Implement container & widget scrolling using the mouse wheel:
    //
    // 1. COMPONENT (include/Zahlen/Components.hpp):
    //    - Define `Components::UIScrollComponent`:
    //      * `float scrollX = 0.0f, scrollY = 0.0f;`
    //      * `float targetScrollY = 0.0f;` (for smooth lerping via dt)
    //      * `float maxScrollX = 0.0f, maxScrollY = 0.0f;` (calculated during layout)
    //      * `float scrollSpeed = 35.0f;`
    //      * `bool smoothScroll = true;`
    //
    // 2. LAYOUT PROPAGATION (src/engine/system/UILayoutSystem.hpp):
    //    - In `ReadBackLayout`, check if the parent entity has a `UIScrollComponent`:
    //      Subtract parent `scrollX` and `scrollY` from the origin passed to child nodes:
    //      `self(self, childEnt, rect->computedAbsMinX - scrollX, rect->computedAbsMinY - scrollY);`
    //    - Post-layout pass: Measure total child bounding extent vs container height to
    //      compute `maxScrollY = std::max(0.0f, contentHeight - containerHeight)`.
    //
    // 3. INTERACTION & WHEEL DISPATCH (here in UIInteractionSystem.cpp):
    //    - If `std::abs(wheel) > 0.001f`:
    //      a) Find the innermost (deepest `hierarchyDepth`) hovered container with `UIScrollComponent`.
    //      b) Adjust `targetScrollY = std::clamp(targetScrollY - wheel * scrollSpeed, 0.0f, maxScrollY)`.
    //      c) (Optional) If hovering an active `UISliderComponent`, adjust `slider->value` by `wheel * step`.
    //    - Per-frame smooth scroll integration:
    //      `scroll->scrollY += (scroll->targetScrollY - scroll->scrollY) * std::clamp(15.0f * dt, 0.0f, 1.0f);`
    //
    // 4. CLIPPING & SCISSORING (src/engine/system/UIRenderSystem.cpp):
    //    - Scrollable containers must have `UIRectComponent.clipChildren = true`.
    //    - As child vertices shift past bounds, `IntersectScissor` automatically handles GPU clipping.
    //
    // 5. FLUENT BUILDER API (include/Zahlen/GUI.hpp):
    //    - Add `ui.ScrollBox("Name", ScrollBoxConfig { ... }, [&]() { ... })` helper in `GUI::Context`.
    (void) wheel;

    // Reset per-frame hover on every UIButtonComponent in one pass. The
    // UIButtonComponent::Hovered flag is the single source of truth for hover
    // across all widgets; compound widgets never cache their own hovered flag.
    for (auto& btn: reg.GetRawArray<Components::UIButtonComponent>()) {
        btn.Set(UIButton::Hovered, false);
        btn.Set(UIButton::Clicked, false);
    }

    // 1. Process active drag operations (window dragging + slider + splitter)
    // Window/Panel drags
    for (Entity e: reg.GetEntitiesWith<Components::UIDragComponent>()) {
        if (auto* drag = reg.Get<Components::UIDragComponent>(e)) {
            // Determine whether this drag handle belongs to a slider or splitter
            // by walking up parents.
            Entity sliderEnt   = FindAncestorWith<Components::UISliderComponent>(reg, e);
            Entity splitterEnt = FindAncestorWith<Components::UISplitterComponent>(reg, e);

            if (sliderEnt != Entity::Null()) {
                // Slider drag: adjust slider value
                if (auto* slider = reg.Get<Components::UISliderComponent>(sliderEnt)) {
                    if (!leftMouseDown) {
                        slider->isDragging = false;
                    } else if (drag->isDragging || slider->isDragging) {
                        if (!slider->isDragging) {
                            slider->isDragging = true;
                        }
                        // Find the _sl_track child to get the track's on-screen bounds
                        Bounds trackB {};
                        bool   hasBounds = false;
                        if (auto* cache = reg.Get<Components::UIChildCacheComponent>(sliderEnt)) {
                            cache->children.ForEach([&](uint64_t, const Components::UIChildCacheComponent::ChildRecord& rec) -> void {
                                if (hasBounds)
                                    return;
                                Entity child = rec.entity;
                                if (!reg.IsAlive(child))
                                    return;
                                if (auto* cname = reg.Get<Components::NameComponent>(child)) {
                                    if (std::string_view(cname->name) == "_sl_track") {
                                        if (auto* tr = reg.Get<Components::UIRectComponent>(child)) {
                                            trackB    = GetBounds(*tr);
                                            hasBounds = true;
                                        }
                                    }
                                }
                            });
                        }
                        if (!hasBounds) {
                            if (auto* srect = reg.Get<Components::UIRectComponent>(sliderEnt)) {
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
                if (auto* split = reg.Get<Components::UISplitterComponent>(splitterEnt)) {
                    if (!leftMouseDown) {
                        split->isDragging = false;
                    } else if (drag->isDragging || split->isDragging) {
                        if (!split->isDragging) {
                            split->isDragging = true;
                        }
                        if (auto* srect = reg.Get<Components::UIRectComponent>(splitterEnt)) {
                            Bounds tb         = GetBounds(*srect);
                            bool   horizontal = (split->direction == Components::UISplitterComponent::Horizontal);
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
                        if (auto* targetRect = reg.Get<Components::UIRectComponent>(drag->targetEntity)) {
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
    };
    JPH::Array<SortEntry> sortedEntries;
    sortedEntries.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        sortedEntries.push_back({.rawIndex = i, .depth = rects[i].hierarchyDepth});
    }
    std::ranges::sort(sortedEntries, [](const auto& a, const auto& b) { return a.depth > b.depth; });

    bool   clickConsumed   = false;
    bool   focusCaptured   = false;
    Entity clickedDropdown = Entity::Null();

    for (const auto& entry: sortedEntries) {
        Entity      e      = entities[entry.rawIndex];
        const auto& rect   = rects[entry.rawIndex];
        auto*       button = reg.Get<Components::UIButtonComponent>(e);

        bool hidden = IsEntityOrAncestorHidden(e);

        if (button == nullptr || hidden) {
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

        Bounds b      = GetBounds(rect);
        bool   inside = Inside(b, mouseX, mouseY);

        if (clickConsumed) {
            button->Set(UIButton::Hovered, false);
            button->Set(UIButton::Pressed, false);
            continue;
        }

        if (inside) {
            button->Set(UIButton::Hovered, true);
            if (leftMouseDown) {
                button->Set(UIButton::Pressed, true);

                // Click-to-set on sliders: start drag and jump value to click position
                if (Entity slEnt = FindAncestorWith<Components::UISliderComponent>(reg, e); slEnt != Entity::Null()) {
                    if (auto* slider = reg.Get<Components::UISliderComponent>(slEnt)) {
                        slider->isDragging = true;
                        // Find track bounds
                        Bounds trackB {};
                        bool   hasBounds = false;
                        if (auto* cache = reg.Get<Components::UIChildCacheComponent>(slEnt)) {
                            cache->children.ForEach([&](uint64_t, const Components::UIChildCacheComponent::ChildRecord& rec) -> void {
                                if (hasBounds)
                                    return;
                                Entity child = rec.entity;
                                if (!reg.IsAlive(child))
                                    return;
                                if (auto* cname = reg.Get<Components::NameComponent>(child)) {
                                    if (std::string_view(cname->name) == "_sl_track") {
                                        if (auto* tr = reg.Get<Components::UIRectComponent>(child)) {
                                            trackB    = GetBounds(*tr);
                                            hasBounds = true;
                                        }
                                    }
                                }
                            });
                        }
                        if (!hasBounds) {
                            if (auto* srect = reg.Get<Components::UIRectComponent>(slEnt)) {
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
                if (Entity spEnt = FindAncestorWith<Components::UISplitterComponent>(reg, e); spEnt != Entity::Null()) {
                    if (auto* sp = reg.Get<Components::UISplitterComponent>(spEnt)) {
                        sp->isDragging = true;
                    }
                }

                // Dropdown: clicking anywhere inside an open dropdown (but not an
                // item) shouldn't auto-close; clicking the header toggles.
                if (Entity ddEnt = FindAncestorWith<Components::UIDropdownComponent>(reg, e); ddEnt != Entity::Null()) {
                    clickedDropdown = ddEnt;
                }

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

    // Close expanded dropdowns when clicking outside any dropdown.
    if (leftMouseDown && clickedDropdown == Entity::Null()) {
        for (Entity e: reg.GetEntitiesWith<Components::UIDropdownComponent>()) {
            if (auto* dd = reg.Get<Components::UIDropdownComponent>(e)) {
                dd->expanded = false;
            }
        }
    }

    // Focus handling for text inputs (defocus when clicking away)
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
