// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// src/gui/GUIContext.cpp
//
// Out-of-line definitions of GUI::Context's non-template members, extracted
// from include/Zahlen/GUI.hpp so the header carries declarations and the
// template/closure layer only. Signatures, semantics and the RAII/closure
// contract are unchanged; this is a linkage split, not a behaviour change.
// Template members (closure forms, callback overloads) stay in the header by
// necessity.

#include <Zahlen/GUI.hpp>
#include <Zahlen/gui/UIComponents.hpp>

namespace ZHLN::GUI {

[[nodiscard]] auto Context::GetRegistry() const noexcept -> ECS::Registry& {
        return *m_reg;
    }

[[nodiscard]] auto Context::GetCurrentParent() const noexcept -> Entity {
        return (m_stackTop > 0) ? m_stack[m_stackTop - 1].entity : Entity::Null();
    }

[[nodiscard]] auto Context::GetCurrentDepth() const noexcept -> uint32_t {
        return (m_stackTop > 0) ? m_stack[m_stackTop - 1].depth + 1 : 1;
    }

[[nodiscard]] auto Context::Status() const noexcept -> std::expected<void, Error> {
        if (m_error) {
            return std::unexpected(m_error);
        }
        return {};
    }

void Context::ClearStatus() noexcept {
        m_error = Error {};
    }

[[nodiscard]] auto Context::DestroyUIEntity(Entity ent) noexcept -> std::expected<void, Error> {
        if (!m_reg->IsAlive(ent)) {
            return std::unexpected(Error(GUIError::EntityNotAlive));
        }

        ZHLN::Log<LogChannel::StdErr, LogLevel::Verbose>(
            "[GUI::Context] DestroyUIEntity: destroying UI entity ({}:{}) and its subtree.", ent.index, ent.generation
        );

        // First failure wins and propagates; a liveness-verified child that
        // fails to die is a real failure, not a discarded temporary.
        std::expected<void, Error> firstFailure {};
        if (const auto* cache = m_reg->Get<UIComponents::UIChildCacheComponent>(ent)) {
            cache->children.ForEach([&](uint64_t, const UIComponents::UIChildCacheComponent::ChildRecord& rec) -> void {
                if (firstFailure.has_value() && m_reg->IsAlive(rec.entity)) {
                    firstFailure = DestroyUIEntity(rec.entity);
                }
            });
        }
        if (!firstFailure) {
            return firstFailure;
        }

        m_reg->Destroy(ent);
        return {};
    }

void Context::SweepStaleChildren(Entity parentEntity) {
        Entity cacheEntity = (parentEntity != Entity::Null()) ? parentEntity : GetRootCacheEntity();
        if (cacheEntity == Entity::Null() || !m_reg->IsAlive(cacheEntity)) {
            RecordError(Error(GUIError::ParentNotAlive));
            return;
        }

        auto* cache = m_reg->Get<UIComponents::UIChildCacheComponent>(cacheEntity);
        if (cache == nullptr) {
            return;
        }

        // A record is stale when its widget was not rebuilt this frame, OR when
        // the entity died outside the GUI (orphaned record pointing at a dead
        // entity, e.g. after a direct Registry::Destroy).
        ZHLN::Array<uint64_t> staleKeys;
        cache->children.ForEach([&](uint64_t key, const UIComponents::UIChildCacheComponent::ChildRecord& rec) -> void {
            if (!m_reg->IsAlive(rec.entity) || rec.lastVisitedFrame < m_currentFrame) {
                staleKeys.push_back(key);
            }
        });

        // Counters feed the verbose traces below; the API itself carries no
        // success payload.
        uint32_t destroyedSubtrees = 0;
        uint32_t purgedRecords     = 0;
        for (uint64_t key: staleKeys) {
            bool wasAlive = false;
            if (const auto* rec = cache->children.Find(key)) {
                wasAlive = m_reg->IsAlive(rec->entity);
                if (wasAlive) {
                    if (const auto res = DestroyUIEntity(rec->entity); !res) {
                        // Latch, never discard: a liveness-verified entity
                        // failing to die is a real failure. Abort the sweep
                        // here, exactly as the old propagation did.
                        RecordError(res.error());
                        return;
                    }
                }
            }
            // Erase the record as well as the entity: keeping it leaks one record
            // per dynamic widget ever created (re-keyed tree rows, changing
            // labels, ...), and every sweep then walks all of them every frame -
            // which is the runtime lag this GC is supposed to prevent.
            cache->children.Erase(key);
            if (wasAlive) {
                ++destroyedSubtrees;
            } else {
                ++purgedRecords;
            }
        }

        if (destroyedSubtrees > 0) {
            ZHLN::Log<LogChannel::StdErr, LogLevel::Verbose>(
                "[GUI::Context] Swept {} stale UI subtree(s) under parent ({}:{}): widget(s) were not rebuilt this frame. Remaining records: {}.",
                destroyedSubtrees, cacheEntity.index, cacheEntity.generation, cache->children.Size()
            );
        }
        if (purgedRecords > 0) {
            ZHLN::Log<LogChannel::StdErr, LogLevel::Verbose>(
                "[GUI::Context] Purged {} orphaned UI cache record(s) under parent ({}:{}) pointing at entities destroyed outside the GUI.", purgedRecords,
                cacheEntity.index, cacheEntity.generation
            );
        }
    }

[[nodiscard]] auto Context::Panel(std::string_view name, const PanelConfig& cfg ) -> UIScope {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        uint64_t key = HashCombine(parent.Pack(), HashStringView(name));

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(name)},
                UIComponents::UIRectComponent {
                    .parentEntity   = parent,
                    .x              = cfg.x,
                    .y              = cfg.y,
                    .width          = cfg.width,
                    .height         = cfg.height,
                    .anchorMinX     = cfg.anchorMinX,
                    .anchorMinY     = cfg.anchorMinY,
                    .anchorMaxX     = cfg.anchorMaxX,
                    .anchorMaxY     = cfg.anchorMaxY,
                    .hierarchyDepth = depth,
                    .clipChildren   = cfg.clipChildren
                },
                UIComponents::UIPanelComponent {.color = cfg.color, .borderRadius = cfg.borderRadius, .edgeWidth = cfg.edgeWidth},
                UIComponents::UIFlexComponent {
                    .direction     = cfg.direction,
                    .justify       = cfg.justify,
                    .alignItems    = cfg.alignItems,
                    .paddingLeft   = cfg.padding,
                    .paddingTop    = cfg.padding,
                    .paddingRight  = cfg.padding,
                    .paddingBottom = cfg.padding,
                    .gapX          = cfg.gap,
                    .gapY          = cfg.gap
                },
                Components::MeshComponent {}
            );
        });

        m_reg->Patch<UIComponents::UIRectComponent>(e, [&](auto& rect) -> auto {
            rect.x              = cfg.x;
            rect.y              = cfg.y;
            rect.width          = cfg.width;
            rect.height         = cfg.height;
            rect.anchorMinX     = cfg.anchorMinX;
            rect.anchorMinY     = cfg.anchorMinY;
            rect.anchorMaxX     = cfg.anchorMaxX;
            rect.anchorMaxY     = cfg.anchorMaxY;
            rect.parentEntity   = parent;
            rect.hierarchyDepth = depth;
        });

        return PushScope(e, depth);
    }

[[nodiscard]] auto Context::Box(std::string_view name, const BoxConfig& cfg ) -> UIScope {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        uint64_t key = HashCombine(parent.Pack(), HashStringView(name));

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(name)},
                UIComponents::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                UIComponents::UIPanelComponent {.color = cfg.color, .edgeWidth = cfg.edgeWidth},
                UIComponents::UIFlexComponent {
                    .direction     = cfg.direction,
                    .justify       = cfg.justify,
                    .alignItems    = cfg.alignItems,
                    .flexShrink    = cfg.flexShrink,
                    .paddingLeft   = cfg.padding,
                    .paddingTop    = cfg.padding,
                    .paddingRight  = cfg.padding,
                    .paddingBottom = cfg.padding,
                    .marginLeft    = cfg.margin,
                    .marginTop     = cfg.margin,
                    .marginRight   = cfg.margin,
                    .marginBottom  = cfg.margin,
                    .gapX          = cfg.gap,
                    .gapY          = cfg.gap
                }
            );
        });

        return PushScope(e, depth);
    }

auto Context::Label(std::string_view text, const LabelConfig& cfg ) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        std::array<char, 128> labelBuf {};
        std::string_view      labelName = FormatTo(labelBuf, "Lbl_{}", text);
        uint64_t              key       = HashCombine(parent.Pack(), HashStringView(labelName));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(labelName)},
                UIComponents::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = depth},
                UIComponents::TextComponent {
                    .text          = String256(text),
                    .scale         = cfg.scale,
                    .color         = cfg.color,
                    .align         = cfg.align,
                    .verticalAlign = cfg.verticalAlign,
                    .fontIndex     = fontHandle,
                    .wrapText      = cfg.wrap,
                    .wrapWidth     = cfg.maxWidth
                }
            );
        });

        m_reg->Patch<UIComponents::TextComponent>(e, [&](auto& textComp) -> auto {
            textComp.text.assign(text);
            textComp.color     = cfg.color;
            textComp.scale     = cfg.scale;
            textComp.align     = cfg.align;
            textComp.wrapText  = cfg.wrap;
            textComp.wrapWidth = cfg.maxWidth;
        });

        return e;
    }

auto Context::Checkbox(std::string_view id, std::string_view label, bool& value, const CheckboxConfig& cfg) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                UIComponents::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = depth},
                UIComponents::UIPanelComponent {.color = {0.0f, 0.0f, 0.0f, 0.0f}},
                UIComponents::UIFlexComponent {
                    .direction     = FlexDirection::Row,
                    .alignItems    = FlexAlign::Center,
                    .paddingLeft   = 4.0f,
                    .paddingRight  = 4.0f,
                    .gapX          = cfg.gap,
                    .gapY          = cfg.gap
                },
                UIComponents::UIButtonComponent {},
                UIComponents::UICheckboxComponent {.checked = value, .previousValue = value},
                // Inner checkbox box
                // Will be child-created below as a sibling to a label
                Components::MeshComponent {}
            );
        });

        // Sync: on first/respawn the entity has no children yet — create the
        // box+check+label inside. On subsequent frames ensure visual state
        // matches the component.
        auto* cb = m_reg->Get<UIComponents::UICheckboxComponent>(e);

        // Toggle on click (ConsumeClick both tests and clears the flag).
        if (ConsumeClick(e)) {
            cb->checked = !cb->checked;
        }
        // Accept external programmatic change when the caller flipped the
        // value without a click.
        else if (value != cb->checked) {
            cb->checked = value;
        }

        // Create / patch inner children (box + check mark + label)
        EnsureCheckboxChildren(e, label, cfg, fontHandle, cb->checked);

        // Apply hover visual directly from the UIButtonComponent (single
        // source of truth — no duplicated cb->hovered flag).
        PatchCheckboxVisuals(e, cfg, cb->checked);

        // Push the (possibly new) ECS value back to the caller's reference
        value = cb->checked;
        cb->previousValue = cb->checked;

        return e;
    }

auto Context::Checkbox(std::string_view id, std::string_view label, bool& value) -> Entity {
        return Checkbox(id, label, value, CheckboxConfig {});
    }

auto Context::Checkbox(std::string_view label, bool& value, const CheckboxConfig& cfg ) -> Entity {
        return Checkbox(label, label, value, cfg);
    }

auto Context::DragFloat(std::string_view id, std::string_view label, float& value, float minVal, float maxVal, float step, const SliderConfig& cfg) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                UIComponents::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                UIComponents::UIPanelComponent {.color = {0.0f, 0.0f, 0.0f, 0.0f}},
                UIComponents::UIFlexComponent {
                    .direction  = FlexDirection::Row,
                    .alignItems = FlexAlign::Center,
                    .gapX       = 8.0f,
                    .gapY       = 8.0f
                },
                UIComponents::UIButtonComponent {},
                UIComponents::UISliderComponent {
                    .value         = std::clamp(value, minVal, maxVal),
                    .minValue      = minVal,
                    .maxValue      = maxVal,
                    .step          = step,
                    .previousValue = value
                }
            );
        });

        auto* slider = m_reg->Get<UIComponents::UISliderComponent>(e);

        // Write back to caller reference (ECS is authoritative)
        value = std::clamp(value, slider->minValue, slider->maxValue);
        if (value != slider->previousValue && !slider->isDragging) {
            // Caller changed value externally — reflect it into ECS
            slider->value         = value;
            slider->previousValue = value;
        } else {
            value = slider->value;
        }
        slider->previousValue = slider->value;

        // Ensure label + track + knob children exist
        EnsureSliderChildren(e, label, cfg, fontHandle, slider->value, slider->minValue, slider->maxValue);

        return e;
    }

auto Context::DragFloat(std::string_view label, float& value, float minVal, float maxVal, float step , const SliderConfig& cfg ) -> Entity {
        return DragFloat(label, label, value, minVal, maxVal, step, cfg);
    }

auto Context::DragFloat(std::string_view id, std::string_view label, float& value, float minVal, float maxVal, float step) -> Entity {
        return DragFloat(id, label, value, minVal, maxVal, step, SliderConfig {});
    }

auto Context::DragFloat(std::string_view id, std::string_view label, float& value, float minVal, float maxVal) -> Entity {
        return DragFloat(id, label, value, minVal, maxVal, 0.0f, SliderConfig {});
    }

auto Context::Slider(std::string_view label, float& value, float minVal, float maxVal, float step , const SliderConfig& cfg ) -> Entity {
        // Slider is a cosmetic alias for DragFloat (click-to-jump + drag semantics)
        return DragFloat(label, label, value, minVal, maxVal, step, cfg);
    }

auto Context::Slider(std::string_view id, std::string_view label, float& value, float minVal, float maxVal, float step , const SliderConfig& cfg ) -> Entity {
        return DragFloat(id, label, value, minVal, maxVal, step, cfg);
    }

auto Context::Dropdown(std::string_view id, std::string_view label, int& selectedIdx, std::span<const std::string_view> options, const DropdownConfig& cfg) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            auto ent = m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                UIComponents::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                UIComponents::UIPanelComponent {.color = cfg.bgColor, .borderRadius = cfg.borderRadius},
                UIComponents::UIFlexComponent {
                    .direction     = FlexDirection::Row,
                    .alignItems    = FlexAlign::Center,
                    .paddingLeft   = cfg.padding,
                    .paddingRight  = cfg.padding
                },
                UIComponents::UIButtonComponent {},
                UIComponents::UIDropdownComponent {
                    .selectedIdx = selectedIdx,
                    .previousIdx = selectedIdx,
                    .expanded    = false,
                    .options     = {},
                }
            );
            return ent;
        });

        auto* dd = m_reg->Get<UIComponents::UIDropdownComponent>(e);

        // The header (selected text + arrow) is a single row. An older build
        // created it as a column, stacking the arrow under the text where it
        // hung out of the 32px header and into the widget below.
        m_reg->Patch<UIComponents::UIFlexComponent>(e, [&](auto& f) -> auto {
            f.direction  = FlexDirection::Row;
            f.alignItems = FlexAlign::Center;
        });

        // Store options into the component so they survive between calls
        dd->options.clear();
        for (const auto& opt: options) {
            dd->options.push_back(String128(opt));
        }

        // Click on header toggles expansion (ConsumeClick reads + clears the flag).
        if (ConsumeClick(e)) {
            dd->expanded = !dd->expanded;
        }

        // Update header panel color to reflect hover
        m_reg->Patch<UIComponents::UIPanelComponent>(e, [&](auto& pc) -> auto {
            pc.color = IsHovered(e) ? cfg.hoverColor : cfg.bgColor;
            pc.borderRadius = cfg.borderRadius;
        });

        // Header shows the currently selected option (or placeholder label)
        std::string_view displayText = label;
        if (dd->selectedIdx >= 0 && static_cast<size_t>(dd->selectedIdx) < dd->options.size()) {
            displayText = std::string_view(dd->options[dd->selectedIdx]);
        }

        // Ensure header child (display text + arrow) exists
        EnsureDropdownHeader(e, displayText, cfg, fontHandle);

        // When expanded, build the item list on the OVERLAY layer instead of
        // under the dropdown entity. Parenting it here would let any ancestor
        // with clipChildren (a panel, a scrolled viewport) cut the menu off at
        // the parent's edge; the overlay root has no parent, so the renderer's
        // scissor propagation never reaches it and the menu floats on top.
        if (dd->expanded) {
            const OwnerAnchor anchor = GetOwnerAnchor(e);

            PopupConfig popCfg;
            popCfg.width        = (cfg.width > 0.0f) ? cfg.width : std::max(120.0f, anchor.width);
            popCfg.bgColor      = {0.07f, 0.10f, 0.16f, 0.98f};
            popCfg.borderRadius = cfg.borderRadius;
            popCfg.gap          = 0.0f;
            popCfg.padding      = 4.0f;

            auto popupScope = BeginPopup(e, popCfg);

            // A menu taller than maxMenuHeight gets its own scroll viewport,
            // so a long option list stays reachable instead of running off the
            // bottom of the screen.
            const float menuHeight =
                static_cast<float>(dd->options.size()) * cfg.itemHeight + std::max(0.0f, static_cast<float>(dd->options.size()) - 1.0f) * 2.0f + 8.0f;
            const bool needsScroll = (cfg.maxMenuHeight > 0.0f) && (menuHeight > cfg.maxMenuHeight);

            std::array<char, 64> menuNameBuf {};
            std::string_view     menuName = FormatTo(menuNameBuf, "{}_menu", id);

            UIScope scrollScope {};
            if (needsScroll) {
                ScrollBoxConfig sbCfg;
                sbCfg.height         = cfg.maxMenuHeight;
                sbCfg.padding        = 0.0f;
                sbCfg.gap            = 2.0f;
                sbCfg.bgColor        = {0.0f, 0.0f, 0.0f, 0.0f};
                sbCfg.scrollbarWidth = 6.0f;
                scrollScope          = BeginScrollBox(menuName, sbCfg);
            }

            for (int i = 0; i < static_cast<int>(dd->options.size()); ++i) {
                std::array<char, 64> itemKeyBuf {};
                std::string_view     itemName = FormatTo(itemKeyBuf, "{}_opt{}", id, i);
                uint64_t itemKey = HashCombine(GetCurrentParent().Pack(), HashStringView(itemName));

                Entity menuParent = GetCurrentParent();
                uint32_t menuDepth = GetCurrentDepth();
                Entity itemEnt = GetOrCreateEntity(itemKey, [&]() -> Entity {
                    return m_reg->Create(
                        Components::NameComponent {.name = String64(itemName)},
                        UIComponents::UIRectComponent {.parentEntity = menuParent, .height = cfg.itemHeight, .hierarchyDepth = menuDepth},
                        UIComponents::UIPanelComponent {.color = (i == dd->selectedIdx) ? cfg.selectedColor : cfg.bgColor},
                        UIComponents::UIFlexComponent {
                            .direction     = FlexDirection::Row,
                            .alignItems    = FlexAlign::Center,
                            .flexGrow      = 1.0f,
                            .paddingLeft   = cfg.padding,
                            .paddingRight  = cfg.padding
                        },
                        UIComponents::UIButtonComponent {},
                        UIComponents::TextComponent {
                            .text          = String256(std::string_view(dd->options[i])),
                            .scale         = cfg.scale,
                            .color         = cfg.textColor,
                            .align         = TextAlignment::Left,
                            .verticalAlign = TextVerticalAlignment::Center,
                            .fontIndex     = fontHandle
                        }
                    );
                });

                // Ensure parent/depth/sizing stays correct if the menu box moved
                m_reg->Patch<UIComponents::UIRectComponent>(itemEnt, [&](auto& r) -> auto {
                    r.parentEntity   = menuParent;
                    r.hierarchyDepth = menuDepth;
                    r.height         = cfg.itemHeight;
                });
                if (auto* iflex = m_reg->Get<UIComponents::UIFlexComponent>(itemEnt)) {
                    iflex->flexGrow = 1.0f;
                }

                // Update text/color
                m_reg->Patch<UIComponents::TextComponent>(itemEnt, [&](auto& tc) -> auto {
                    tc.text.assign(std::string_view(dd->options[i]));
                });
                bool isSelected = (i == dd->selectedIdx);
                bool isItemHover = IsHovered(itemEnt);
                m_reg->Patch<UIComponents::UIPanelComponent>(itemEnt, [&](auto& pc) -> auto {
                    pc.color = isSelected ? cfg.selectedColor : (isItemHover ? cfg.hoverColor : cfg.bgColor);
                });

                // Handle item click (consumes the click flag)
                if (ConsumeClick(itemEnt)) {
                    dd->selectedIdx = i;
                    dd->expanded    = false;
                }
            }
        }

        // Sync selected index back to caller
        if (dd->selectedIdx != dd->previousIdx) {
            selectedIdx     = dd->selectedIdx;
            dd->previousIdx = dd->selectedIdx;
        } else {
            // Accept external value change
            if (selectedIdx != dd->selectedIdx && selectedIdx >= 0 &&
                static_cast<size_t>(selectedIdx) < dd->options.size()) {
                dd->selectedIdx = selectedIdx;
            }
            selectedIdx = dd->selectedIdx;
        }

        return e;
    }

auto Context::Dropdown(std::string_view label, int& selectedIdx, std::span<const std::string_view> options, const DropdownConfig& cfg ) -> Entity {
        return Dropdown(label, label, selectedIdx, options, cfg);
    }

auto Context::Dropdown(std::string_view id, std::string_view label, int& selectedIdx, std::span<const std::string_view> options) -> Entity {
        return Dropdown(id, label, selectedIdx, options, DropdownConfig {});
    }

[[nodiscard]] auto Context::BeginCollapsingHeader(std::string_view id, std::string_view label, bool defaultOpen, const CollapsingHeaderConfig& cfg ) -> UIScope {
        uint32_t depth = 0;
        Entity   e     = PrepareCollapsingHeader(id, label, defaultOpen, cfg, depth);

        auto* hdr = m_reg->Get<UIComponents::UICollapsingHeaderComponent>(e);
        if (hdr->isOpen) {
            // Build the content box directly under the header (same key as the
            // closure form's Box, so switching forms never dupes the subtree),
            // then push ONLY that box. Widgets the caller adds after this call
            // therefore parent inside the box, matching the closure form.
            std::array<char, 64> boxNameBuf {};
            std::string_view     contentBoxName = FormatTo(boxNameBuf, "{}_content", id);
            const uint64_t       boxKey         = HashCombine(e.Pack(), HashStringView(contentBoxName));

            Entity contentBox = GetOrCreateChild(e, boxKey, [&]() -> Entity {
                return m_reg->Create(
                    Components::NameComponent {.name = String64(contentBoxName)},
                    UIComponents::UIRectComponent {.parentEntity = e, .width = 0.0f, .height = 0.0f, .hierarchyDepth = depth + 1},
                    UIComponents::UIPanelComponent {.color = {0.0f, 0.0f, 0.0f, 0.0f}},
                    UIComponents::UIFlexComponent {
                        .direction     = FlexDirection::Column,
                        .paddingLeft   = cfg.indent,
                        .paddingTop    = cfg.indent,
                        .paddingRight  = cfg.indent,
                        .paddingBottom = cfg.indent,
                        .gapX          = 2.0f,
                        .gapY          = 2.0f
                    }
                );
            });
            // Keep parent/depth correct for the persistent entity each frame.
            m_reg->Patch<UIComponents::UIRectComponent>(contentBox, [&](auto& r) -> auto {
                r.parentEntity   = e;
                r.hierarchyDepth = depth + 1;
            });

            // If the push overflowed the cap, the guard disengages and the
            // caller sees IsPushed() == false; either way the guard owns the pop.
            return PushScope(contentBox, depth + 1);
        }

        // Collapsed: nothing was pushed; the caller's content would leak into
        // the current parent, so return a disengaged guard (check IsPushed()).
        SweepStaleChildren(e);
        return {};
    }

[[nodiscard]] auto Context::BeginCollapsingHeader(std::string_view label, bool defaultOpen, const CollapsingHeaderConfig& cfg ) -> UIScope {
        return BeginCollapsingHeader(label, label, defaultOpen, cfg);
    }

[[nodiscard]] auto Context::BeginScrollBox(std::string_view id, const ScrollBoxConfig& cfg ) -> UIScope {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const float viewportHeight = (cfg.height > 0.0f) ? cfg.height : 240.0f;

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                UIComponents::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = viewportHeight, .hierarchyDepth = depth},
                UIComponents::UIPanelComponent {.color = cfg.bgColor},
                UIComponents::UIFlexComponent {
                    .direction  = FlexDirection::Row,
                    .alignItems = FlexAlign::Stretch,
                    .flexGrow   = cfg.flexGrow,
                    // A scroll box has exactly one honest way to give space
                    // back: a shorter viewport, and it is the only widget in a
                    // panel that can take one, because its content scrolls.
                    // Yoga's default shrink-to-fit, however, also applied when
                    // the caller asked for a FIXED height -- squeezing a 300px
                    // viewport to 37px and scrolling nothing. So shrink only
                    // while the box is joining in the parent's free-space
                    // distribution (flexGrow > 0); a fixed height stays
                    // authoritative and lets the panel's overflow be the
                    // caller's problem, where it is visible.
                    .flexShrink = (cfg.flexGrow > 0.0f) ? 1.0f : 0.0f,
                    .flexBasis  = -1.0f
                }
            );
        });

        m_reg->Patch<UIComponents::UIRectComponent>(e, [&](auto& r) -> auto {
            r.parentEntity   = parent;
            r.width          = cfg.width;
            r.height         = viewportHeight;
            r.hierarchyDepth = depth;
            r.clipChildren   = false;
        });
        // flexGrow decides whether `height` is authoritative (0) or a base the
        // parent's free space is added to (1). It is patched every frame so a
        // cached box follows a config change instead of keeping last frame's.
        m_reg->Patch<UIComponents::UIFlexComponent>(e, [&](auto& f) -> auto {
            f.direction  = FlexDirection::Row;
            f.alignItems = FlexAlign::Stretch;
            f.flexGrow   = cfg.flexGrow;
            f.flexShrink = (cfg.flexGrow > 0.0f) ? 1.0f : 0.0f; // see the create path
            f.flexBasis  = -1.0f;
        });

        // Viewport: the clipping, scrolling surface the content lives in.
        Entity viewport = GetOrCreateChild(e, HashStringView("_sb_viewport"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_sb_viewport")},
                UIComponents::UIRectComponent {.parentEntity = e, .width = 0.0f, .height = 0.0f, .hierarchyDepth = depth + 1, .clipChildren = true},
                UIComponents::UIPanelComponent {.color = {0.0f, 0.0f, 0.0f, 0.0f}},
                UIComponents::UIFlexComponent {
                    .direction     = FlexDirection::Column,
                    .alignItems    = FlexAlign::Stretch,
                    .flexGrow      = 1.0f,
                    .flexShrink    = 1.0f,
                    .flexBasis     = 0.0f,
                    .paddingLeft   = cfg.padding,
                    .paddingTop    = cfg.padding,
                    .paddingRight  = cfg.padding,
                    .paddingBottom = cfg.padding,
                    .gapX          = cfg.gap,
                    .gapY          = cfg.gap
                },
                UIComponents::UIScrollComponent {
                    .scrollSpeed     = cfg.scrollSpeed,
                    .smoothSpeed     = cfg.smoothSpeed,
                    .smoothScroll    = cfg.smoothScroll,
                    .allowHorizontal = cfg.allowHorizontal
                }
            );
        });
        m_reg->Patch<UIComponents::UIRectComponent>(viewport, [&](auto& r) -> auto {
            r.parentEntity   = e;
            r.hierarchyDepth = depth + 1;
            r.clipChildren   = true;
        });
        m_reg->Patch<UIComponents::UIFlexComponent>(viewport, [&](auto& f) -> auto {
            f.direction     = FlexDirection::Column;
            f.alignItems    = FlexAlign::Stretch;
            f.flexGrow      = 1.0f;
            f.flexShrink    = 1.0f;
            f.flexBasis     = 0.0f;
            f.paddingLeft   = cfg.padding;
            f.paddingTop    = cfg.padding;
            f.paddingRight  = cfg.padding;
            f.paddingBottom = cfg.padding;
            f.gapX          = cfg.gap;
            f.gapY          = cfg.gap;
        });
        m_reg->Patch<UIComponents::UIScrollComponent>(viewport, [&](auto& s) -> auto {
            s.scrollSpeed     = cfg.scrollSpeed;
            s.smoothSpeed     = cfg.smoothSpeed;
            s.smoothScroll    = cfg.smoothScroll;
            s.allowHorizontal = cfg.allowHorizontal;
        });

        UpdateScrollbar(e, viewport, cfg, depth);

        // The viewport's children are the caller's widgets; the root's own
        // chrome (viewport + track) is visited every frame above, so this
        // sweep only ever reclaims chrome that stopped being built (e.g. the
        // track after scrollbarWidth was set to 0).
        SweepStaleChildren(e);

        return PushScope(viewport, depth + 1);
    }

auto Context::Image(std::string_view id, TextureHandle texture, const ImageConfig& cfg ) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                UIComponents::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                // A button makes the sprite a hover target: UIButtonComponent
                // is the single source of truth for hover, and TooltipFor reads
                // IsHovered(owner) -- without a button an icon could never fire
                // its tooltip. It carries no visual of its own.
                UIComponents::UIButtonComponent {},
                UIComponents::UIImageComponent {
                    .texture      = texture,
                    .mode         = cfg.mode,
                    .tint         = cfg.tint,
                    .uv0x         = cfg.uv0x,
                    .uv0y         = cfg.uv0y,
                    .uv1x         = cfg.uv1x,
                    .uv1y         = cfg.uv1y,
                    .sourceWidth  = cfg.sourceWidth,
                    .sourceHeight = cfg.sourceHeight
                }
            );
        });

        // Cached entities outlive the config that created them, so every field
        // is re-applied each frame (an icon swapped to another atlas slice must
        // not keep last frame's UVs).
        m_reg->Patch<UIComponents::UIRectComponent>(e, [&](auto& r) -> auto {
            r.parentEntity   = parent;
            r.width          = cfg.width;
            r.height         = cfg.height;
            r.hierarchyDepth = depth;
        });
        m_reg->Patch<UIComponents::UIImageComponent>(e, [&](auto& img) -> auto {
            img.texture      = texture;
            img.mode         = cfg.mode;
            img.tint         = cfg.tint;
            img.uv0x         = cfg.uv0x;
            img.uv0y         = cfg.uv0y;
            img.uv1x         = cfg.uv1x;
            img.uv1y         = cfg.uv1y;
            img.sourceWidth  = cfg.sourceWidth;
            img.sourceHeight = cfg.sourceHeight;
        });

        return e;
    }

auto Context::Icon(std::string_view id, TextureHandle texture, float size , ImageScaleMode mode ) -> Entity {
        ImageConfig cfg;
        cfg.width        = size;
        cfg.height       = size;
        cfg.mode         = mode;
        cfg.sourceWidth  = size;
        cfg.sourceHeight = size;
        return Image(id, texture, cfg);
    }

auto Context::Gradient(std::string_view id, const GradientConfig& cfg ) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                UIComponents::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                UIComponents::UIGradientComponent {}
            );
        });

        m_reg->Patch<UIComponents::UIRectComponent>(e, [&](auto& r) -> auto {
            r.parentEntity   = parent;
            r.width          = cfg.width;
            r.height         = cfg.height;
            r.hierarchyDepth = depth;
        });

        // The stops are rewritten every frame: a gradient is a live visual
        // (the hue strip of a picker tracks the selected colour), and the
        // inline array is cheaper to overwrite than to diff.
        m_reg->Patch<UIComponents::UIGradientComponent>(e, [&](auto& g) -> auto {
            g.axis      = cfg.axis;
            g.stopCount = std::min(static_cast<uint32_t>(cfg.stops.size()), UIComponents::UIGradientComponent::kMaxStops);
            for (uint32_t i = 0; i < g.stopCount; ++i) {
                g.stops[i] = cfg.stops[i];
            }
        });

        return e;
    }

auto Context::Plot(std::string_view id, std::span<const float> values, const PlotConfig& cfg ) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                UIComponents::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                UIComponents::UIPlotComponent {}
            );
        });

        m_reg->Patch<UIComponents::UIRectComponent>(e, [&](auto& r) -> auto {
            r.parentEntity   = parent;
            r.width          = cfg.width;
            r.height         = cfg.height;
            r.hierarchyDepth = depth;
        });

        m_reg->Patch<UIComponents::UIPlotComponent>(e, [&](auto& p) -> auto {
            // Resize only when the sample count changes. resize() preserves
            // the elements it keeps, so this is about avoiding a reallocation
            // on the hot path: the common case is a ring buffer handed over at
            // a fixed size every frame, and the widget's copy already
            // overwrites every sample.
            const uint32_t n = std::min(static_cast<uint32_t>(values.size()), UIComponents::UIPlotComponent::kMaxPoints);
            if (p.values.size() != n) {
                p.values.resize(n);
            }
            for (uint32_t i = 0; i < n; ++i) {
                p.values[i] = values[i];
            }

            p.kind      = cfg.kind;
            p.minValue  = cfg.minValue;
            p.maxValue  = cfg.maxValue;
            p.lineColor = cfg.lineColor;
            p.fillColor = cfg.fillColor;
            p.lineWidth = cfg.lineWidth;
            p.barGap    = cfg.barGap;
        });

        return e;
    }

auto Context::PlotLines(std::string_view id, std::span<const float> values, const PlotConfig& cfg ) -> Entity {
        PlotConfig c = cfg;
        c.kind       = UIPlotKind::Lines;
        return Plot(id, values, c);
    }

auto Context::Histogram(std::string_view id, std::span<const float> values, const PlotConfig& cfg ) -> Entity {
        PlotConfig c = cfg;
        c.kind       = UIPlotKind::Histogram;
        return Plot(id, values, c);
    }

auto Context::Reference(
    std::string_view id, std::string_view label, uint64_t& value, std::span<const ReferenceOption> options, const ReferenceConfig& cfg
) -> Entity {
        // Cap the label array below, which lives on the stack. The cap is a UI
        // decision, not a memory one — a four-thousand-entry menu is unusable
        // long before it is expensive — but it is also what keeps one call to
        // ~16KB of stack, which matters on a fiber even at the 512KB minimum.
        constexpr uint32_t kHardMaxOptions = 1024;

        const uint32_t cap       = std::min(cfg.maxOptions, kHardMaxOptions);
        const uint32_t optCount  = std::min(static_cast<uint32_t>(options.size()), cap);
        const uint32_t noneSlots = cfg.allowNone ? 1u : 0u;

        std::array<std::string_view, kHardMaxOptions + 2> labels {};
        uint32_t                                          labelCount = 0;

        if (cfg.allowNone) {
            labels[labelCount++] = cfg.noneLabel;
        }

        // Re-derived from the id every frame, never carried across as an index:
        // the caller's list is rebuilt each frame too (entities come and go),
        // so a remembered index would re-point the field at whatever happened
        // to land in that slot.
        int32_t selectedIdx = cfg.allowNone ? 0 : -1;
        for (uint32_t i = 0; i < optCount; ++i) {
            const uint32_t slot = labelCount;
            labels[labelCount++] = options[i].label;
            if (options[i].id == value) {
                selectedIdx = static_cast<int32_t>(slot);
            }
        }

        // A value that matches nothing is shown rather than hidden, with the
        // raw id so the reader can tell a dangling handle from a cleared field.
        std::array<char, 48> danglingBuf {};
        uint32_t             danglingSlot = 0xFFFFFFFFu;
        bool                 hasDangling  = false;
        if (value != 0 && selectedIdx == (cfg.allowNone ? 0 : -1)) {
            danglingSlot    = labelCount;
            hasDangling     = true;
            // Select it, or the row would read "None" for a handle that is
            // very much set — and the write-back below would then clear the
            // field, laundering a dangling handle into an empty one.
            selectedIdx     = static_cast<int32_t>(danglingSlot);
            labels[labelCount++] = FormatTo(danglingBuf, "{}{:#X}>", cfg.danglingPrefix, value);
        }

        if (selectedIdx < 0) {
            selectedIdx = 0;
        }

        DropdownConfig ddCfg;
        ddCfg.height        = cfg.height;
        ddCfg.scale         = cfg.scale;
        ddCfg.itemHeight    = cfg.itemHeight;
        ddCfg.bgColor       = cfg.bgColor;
        ddCfg.hoverColor    = cfg.hoverColor;
        ddCfg.selectedColor = cfg.selectedColor;
        ddCfg.textColor     = cfg.textColor;
        ddCfg.arrowColor    = cfg.arrowColor;
        ddCfg.borderRadius  = cfg.borderRadius;
        ddCfg.maxMenuHeight = cfg.maxMenuHeight;
        ddCfg.padding       = cfg.padding;

        int    idx = selectedIdx;
        Entity e   = Dropdown(id, label, idx, std::span<const std::string_view>(labels.data(), labelCount), ddCfg);

        // Selecting the dangling entry is deliberately a no-op: the value is
        // not in the option list, so there is nothing to map it to, and
        // writing options[0] would launder a broken handle into a plausible
        // one. The entry exists so the state is visible and can be cleared
        // through "None" — on purpose, not by accident.
        if (hasDangling && idx == static_cast<int32_t>(danglingSlot)) {
            return e;
        }

        if (cfg.allowNone && idx == 0) {
            value = 0;
            return e;
        }

        const int32_t optIdx = idx - static_cast<int32_t>(noneSlots);
        if (optIdx >= 0 && static_cast<uint32_t>(optIdx) < optCount) {
            value = options[static_cast<uint32_t>(optIdx)].id;
        }
        return e;
    }

// JPH::Vec4's operator== is component-wise and returns a mask, not a bool, so
// "did this colour change" has to be asked one channel at a time.
namespace {
[[nodiscard]] auto SameColor(const JPH::Vec4& a, const JPH::Vec4& b) noexcept -> bool {
    return a.GetX() == b.GetX() && a.GetY() == b.GetY() && a.GetZ() == b.GetZ() && a.GetW() == b.GetW();
}

[[nodiscard]] auto Clamp01(float v) noexcept -> float {
    if (!(v > 0.0f)) { // also catches NaN
        return 0.0f;
    }
    return (v > 1.0f) ? 1.0f : v;
}

// The seven stops of a full hue loop, used by both the hue strip and the
// saturation/value plane (which needs only the colour at the current hue, but
// builds it from the same table so the two can never disagree).
[[nodiscard]] auto HueStop(float hueDegrees) noexcept -> JPH::Vec4 {
    return HsvToRgb(hueDegrees, 1.0f, 1.0f);
}
} // namespace

void Context::EnsureColorEditChildren(Entity ceEntity, std::string_view label, const ColorEditConfig& cfg, TextureHandle font, const JPH::Vec4& color) {
    uint32_t parentDepth = 0;
    if (const auto* r = m_reg->Get<UIComponents::UIRectComponent>(ceEntity)) {
        parentDepth = r->hierarchyDepth;
    }

    if (!label.empty()) {
        Entity lblEnt = GetOrCreateChild(ceEntity, HashStringView("_ce_label"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ce_label")},
                UIComponents::UIRectComponent {.parentEntity = ceEntity, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                UIComponents::TextComponent {
                    .text          = String256(label),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                },
                UIComponents::UIFlexComponent {.flexGrow = 1.0f}
            );
        });
        m_reg->Patch<UIComponents::TextComponent>(lblEnt, [&](auto& tc) -> auto {
            tc.text.assign(label);
            tc.color = cfg.textColor;
        });
    }

    Entity swatchEnt = GetOrCreateChild(ceEntity, HashStringView("_ce_swatch"), [&]() -> Entity {
        return m_reg->Create(
            Components::NameComponent {.name = String64("_ce_swatch")},
            UIComponents::UIRectComponent {
                .parentEntity = ceEntity, .width = cfg.swatchWidth, .height = cfg.height - 4.0f, .hierarchyDepth = parentDepth + 1},
            UIComponents::UIPanelComponent {
                .color = color, .borderRadius = {cfg.borderRadius, cfg.borderRadius, cfg.borderRadius, cfg.borderRadius}, .edgeWidth = 1.0f},
            UIComponents::UIButtonComponent {}
        );
    });

    const bool hovered = IsHovered(ceEntity) || IsHovered(swatchEnt);
    m_reg->Patch<UIComponents::UIRectComponent>(swatchEnt, [&](auto& r) -> auto {
        r.parentEntity   = ceEntity;
        r.width          = cfg.swatchWidth;
        r.height         = cfg.height - 4.0f;
        r.hierarchyDepth = parentDepth + 1;
    });
    m_reg->Patch<UIComponents::UIPanelComponent>(swatchEnt, [&](auto& pc) -> auto {
        pc.color          = color;
        pc.borderRadius   = {cfg.borderRadius, cfg.borderRadius, cfg.borderRadius, cfg.borderRadius};
        // A dark colour on a dark panel has no visible edge of its own; the
        // border is what keeps a near-black swatch from disappearing.
        pc.edgeWidth      = 1.0f;
        pc.uvLeft = pc.uvRight = pc.uvTop = pc.uvBottom = hovered ? 0.0f : 0.0f;
    });
    // Hover feedback lives on the row, not the swatch: the swatch is the
    // colour, and tinting it would lie about the value being edited.
    m_reg->Patch<UIComponents::UIPanelComponent>(ceEntity, [&](auto& pc) -> auto {
        pc.color = hovered ? cfg.hoverColor : JPH::Vec4 {0.0f, 0.0f, 0.0f, 0.0f};
    });
}

auto Context::ColorEdit(std::string_view id, std::string_view label, float* col, int componentCount, const ColorEditConfig& cfg ) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();
        const bool          hasAlpha   = (componentCount >= 4);

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                UIComponents::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = depth},
                UIComponents::UIPanelComponent {.color = {0.0f, 0.0f, 0.0f, 0.0f}},
                UIComponents::UIFlexComponent {
                    .direction  = FlexDirection::Row,
                    .alignItems = FlexAlign::Center,
                    .gapX       = cfg.gap,
                    .gapY       = cfg.gap
                },
                UIComponents::UIButtonComponent {},
                UIComponents::UIColorEditComponent {}
            );
        });

        m_reg->Patch<UIComponents::UIRectComponent>(e, [&](auto& r) -> auto {
            r.parentEntity   = parent;
            r.height         = cfg.height;
            r.hierarchyDepth = depth;
        });
        m_reg->Patch<UIComponents::UIFlexComponent>(e, [&](auto& f) -> auto {
            f.direction  = FlexDirection::Row;
            f.alignItems = FlexAlign::Center;
            f.gapX = f.gapY = cfg.gap;
        });

        auto* ce = m_reg->Get<UIComponents::UIColorEditComponent>(e);
        if (ce == nullptr) {
            return e;
        }
        ce->componentCount = hasAlpha ? 4 : 3;

        // --- 1. Sync in ---
        //
        // The caller's array is authoritative only when it CHANGED: the picker
        // writes to it at the end of every frame, so treating it as
        // authoritative unconditionally would fight the user's own edits (drag
        // the hue strip, get last frame's value back). previousValue is what
        // tells an outside edit (a preset, an undo, another panel on the same
        // material) apart from that echo.
        const JPH::Vec4 incoming {
            Clamp01(col[0]), Clamp01(col[1]), Clamp01(col[2]), hasAlpha ? Clamp01(col[3]) : ce->value.GetW()};
        if (!SameColor(incoming, ce->previousValue)) {
            ce->value         = incoming;
            ce->previousValue = incoming;
            ce->hsvStale      = true;
        }
        if (ce->hsvStale) {
            RgbToHsv(ce->value, ce->hue, ce->sat, ce->val);
            ce->hsvStale = false;
        }

        // --- 2. The row: label + swatch ---
        EnsureColorEditChildren(e, label, cfg, fontHandle, ce->value);

        // --- 3. Open / close the picker ---
        const Entity swatchEnt = FindChildByKey(e, HashStringView("_ce_swatch"));
        if (ConsumeClick(e) || ((swatchEnt != Entity::Null()) && ConsumeClick(swatchEnt))) {
            ce->expanded = !ce->expanded;
        }

        // --- 4. The picker popup ---
        if (ce->expanded) {
            const OwnerAnchor anchor = GetOwnerAnchor(e);

            PopupConfig popCfg;
            popCfg.width   = (cfg.pickerWidth > 0.0f) ? cfg.pickerWidth : std::max(160.0f, anchor.width);
            popCfg.padding = cfg.padding;
            popCfg.gap     = 6.0f;
            // PopupConfig keeps one radius per corner; the colour field's
            // config carries a single number, which is the common case.
            popCfg.borderRadius = {cfg.borderRadius, cfg.borderRadius, cfg.borderRadius, cfg.borderRadius};

            Popup(e, popCfg, [&]() -> void {
                // --- 4a. Saturation / value plane ---
                //
                // The pad is a plain box (no UIFlexComponent) so its children
                // are positioned by ANCHORS and can overlap: the two gradient
                // layers and the knob all cover the same rect. Under flex they
                // would stack, which is not a colour picker.
                const uint32_t padDepth = GetCurrentDepth();
                Entity svEnt = GetOrCreateEntity(HashCombine(GetCurrentParent().Pack(), HashStringView("_ce_sv")), [&]() -> Entity {
                    return m_reg->Create(
                        Components::NameComponent {.name = String64("_ce_sv")},
                        UIComponents::UIRectComponent {
                            .parentEntity = GetCurrentParent(), .width = cfg.svSize, .height = cfg.svSize, .hierarchyDepth = padDepth},
                        UIComponents::UIButtonComponent {},
                        UIComponents::UIDragComponent {.targetEntity = Entity::Null(), .isDragging = false},
                        UIComponents::UIColorSVComponent {}
                    );
                });

                m_reg->Patch<UIComponents::UIRectComponent>(svEnt, [&](auto& r) -> auto {
                    r.parentEntity   = GetCurrentParent();
                    r.width          = cfg.svSize;
                    r.height         = cfg.svSize;
                    r.hierarchyDepth = padDepth;
                });

                auto* sv = m_reg->Get<UIComponents::UIColorSVComponent>(svEnt);
                if (sv != nullptr) {
                    sv->hue = ce->hue;

                    // The pad is the one widget here the interaction pass does
                    // not own a 1D control for, so its drag state is read back
                    // into the editor every frame. Only while dragging: the
                    // authoritative store is the ColorEdit component, and a
                    // stale pad (one whose entity outlived a collapse) must not
                    // overwrite a hue the user just picked from the strip.
                    if (sv->isDragging) {
                        ce->sat = std::clamp(sv->sat, 0.0f, 1.0f);
                        ce->val = std::clamp(sv->val, 0.0f, 1.0f);
                    } else {
                        sv->sat = ce->sat;
                        sv->val = ce->val;
                    }

                    const JPH::Vec4 hueColor = HueStop(ce->hue);

                    // Layer 1: white -> pure hue, left to right (saturation).
                    const JPH::Vec4 svStops[2] = {JPH::Vec4(1.0f, 1.0f, 1.0f, 1.0f), hueColor};
                    Gradient("_ce_sv_sat", GradientConfig {
                        .width = 0.0f, .height = 0.0f, .axis = UIGradientAxis::Horizontal,
                        .stops = std::span<const JPH::Vec4>(svStops, 2)});

                    // Layer 2: transparent -> black, top to bottom (value).
                    // Compositing black over the hue ramp is the whole SV
                    // plane in two quads: it is exactly the sRGB-space
                    // definition, and cheap enough to rebuild every frame.
                    const JPH::Vec4 valStops[2] = {JPH::Vec4(0.0f, 0.0f, 0.0f, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)};
                    Gradient("_ce_sv_val", GradientConfig {
                        .width = 0.0f, .height = 0.0f, .axis = UIGradientAxis::Vertical,
                        .stops = std::span<const JPH::Vec4>(valStops, 2)});

                    // Knob: anchored at (sat, 1-val) and pulled back by half
                    // its size so its CENTRE sits on the picked point.
                    const uint32_t knobDepth = padDepth + 1;
                    const float    knobHalf  = cfg.knobSize * 0.5f;
                    Entity knobEnt = GetOrCreateEntity(HashCombine(svEnt.Pack(), HashStringView("_ce_sv_knob")), [&]() -> Entity {
                        return m_reg->Create(
                            Components::NameComponent {.name = String64("_ce_sv_knob")},
                            UIComponents::UIRectComponent {
                                .parentEntity = svEnt, .width = cfg.knobSize, .height = cfg.knobSize, .hierarchyDepth = knobDepth},
                            UIComponents::UIPanelComponent {
                                .color = {1.0f, 1.0f, 1.0f, 1.0f},
                                .borderRadius = {knobHalf, knobHalf, knobHalf, knobHalf},
                                .edgeWidth = 1.0f
                            }
                        );
                    });
                    m_reg->Patch<UIComponents::UIRectComponent>(knobEnt, [&](auto& r) -> auto {
                        r.parentEntity   = svEnt;
                        r.width          = cfg.knobSize;
                        r.height         = cfg.knobSize;
                        r.hierarchyDepth = knobDepth;
                        r.anchorMinX = r.anchorMaxX = std::clamp(sv->sat, 0.0f, 1.0f);
                        r.anchorMinY = r.anchorMaxY = std::clamp(1.0f - sv->val, 0.0f, 1.0f);
                        // Anchor positioning puts the widget's top-left on the
                        // pivot; the negative offset re-centres it.
                        r.x            = -knobHalf;
                        r.y            = -knobHalf;
                    });
                    m_reg->Patch<UIComponents::UIPanelComponent>(knobEnt, [&](auto& pc) -> auto {
                        pc.color        = {1.0f, 1.0f, 1.0f, 1.0f};
                        pc.borderRadius = {knobHalf, knobHalf, knobHalf, knobHalf};
                        pc.edgeWidth    = 1.0f;
                    });
                }

                // --- 4b. Hue strip ---
                //
                // A stock Slider 0..360 with a hue ramp painted over its
                // track: the drag, the click-to-jump and the knob all come
                // from the existing slider, and only the fill is new.
                SliderConfig hueCfg;
                hueCfg.height      = cfg.stripHeight;
                hueCfg.trackHeight = cfg.stripHeight;
                hueCfg.knobSize    = cfg.knobSize;
                hueCfg.showValue   = false;
                hueCfg.trackColor  = {0.0f, 0.0f, 0.0f, 0.0f};
                Entity hueEnt = Slider("_ce_hue", "H", ce->hue, 0.0f, 360.0f, 1.0f, hueCfg);
                if (Entity track = FindChildByKey(hueEnt, HashStringView("_sl_track")); track != Entity::Null()) {
                    const JPH::Vec4 hueStops[7] = {
                        HueStop(0.0f), HueStop(60.0f), HueStop(120.0f), HueStop(180.0f), HueStop(240.0f), HueStop(300.0f), HueStop(360.0f)};
                    if (m_reg->Get<UIComponents::UIGradientComponent>(track) == nullptr) {
                        m_reg->Add<UIComponents::UIGradientComponent>(track);
                    }
                    m_reg->Patch<UIComponents::UIGradientComponent>(track, [&](auto& g) -> auto {
                        g.axis      = UIGradientAxis::Horizontal;
                        g.stopCount = 7;
                        for (uint32_t i = 0; i < 7; ++i) {
                            g.stops[i] = hueStops[i];
                        }
                    });
                }

                // --- 4c. Alpha strip ---
                if (hasAlpha) {
                    SliderConfig alphaCfg;
                    alphaCfg.height      = cfg.stripHeight;
                    alphaCfg.trackHeight = cfg.stripHeight;
                    alphaCfg.knobSize    = cfg.knobSize;
                    alphaCfg.showValue   = false;
                    alphaCfg.trackColor  = {0.0f, 0.0f, 0.0f, 0.0f};
                    float alpha = ce->value.GetW();
                    Entity alphaEnt = Slider("_ce_alpha", "A", alpha, 0.0f, 1.0f, 0.0f, alphaCfg);
                    ce->value.SetW(std::clamp(alpha, 0.0f, 1.0f));
                    if (Entity track = FindChildByKey(alphaEnt, HashStringView("_sl_track")); track != Entity::Null()) {
                        // Transparent -> opaque, over a checkerboard the
                        // track's own panel cannot draw. Two stops on the
                        // colour is enough: alpha is the gradient, the RGB is
                        // constant along the strip.
                        const JPH::Vec4 a0 = JPH::Vec4(ce->value.GetX(), ce->value.GetY(), ce->value.GetZ(), 0.0f);
                        const JPH::Vec4 a1 = JPH::Vec4(ce->value.GetX(), ce->value.GetY(), ce->value.GetZ(), 1.0f);
                        const JPH::Vec4 alphaStops[2] = {a0, a1};
                        if (m_reg->Get<UIComponents::UIGradientComponent>(track) == nullptr) {
                            m_reg->Add<UIComponents::UIGradientComponent>(track);
                        }
                        m_reg->Patch<UIComponents::UIGradientComponent>(track, [&](auto& g) -> auto {
                            g.axis      = UIGradientAxis::Horizontal;
                            g.stopCount = 2;
                            g.stops[0]  = alphaStops[0];
                            g.stops[1]  = alphaStops[1];
                        });
                    }
                }

                // --- 4d. Per-channel sliders ---
                if (cfg.showRgbSliders) {
                    SliderConfig chanCfg;
                    chanCfg.height    = 22.0f;
                    chanCfg.showValue = true;
                    float rgba[4] = {ce->value.GetX(), ce->value.GetY(), ce->value.GetZ(), ce->value.GetW()};
                    static constexpr std::string_view kChannelNames[4] = {"R", "G", "B", "A"};
                    const int channelCount = hasAlpha ? 4 : 3;
                    for (int c = 0; c < channelCount; ++c) {
                        std::array<char, 32> chanIdBuf {};
                        const std::string_view chanId = FormatTo(chanIdBuf, "_ce_chan{}", c);
                        Slider(chanId, kChannelNames[c], rgba[c], 0.0f, 1.0f, 0.0f, chanCfg);
                    }
                    // A channel edit moves the colour without going through
                    // HSV, so the pad's position has to be re-derived —
                    // otherwise dragging R to 1 leaves the knob sitting where
                    // it was.
                    ce->value = JPH::Vec4(Clamp01(rgba[0]), Clamp01(rgba[1]), Clamp01(rgba[2]), Clamp01(rgba[3]));
                    ce->hsvStale = true;
                }

                // --- 4e. Hex readout ---
                if (cfg.showHex) {
                    std::array<char, 16> hexBuf {};
                    const std::string_view hexText = FormatTo(
                        hexBuf, "#{:02X}{:02X}{:02X}{:02X}",
                        static_cast<int>(std::lround(ce->value.GetX() * 255.0f)),
                        static_cast<int>(std::lround(ce->value.GetY() * 255.0f)),
                        static_cast<int>(std::lround(ce->value.GetZ() * 255.0f)),
                        static_cast<int>(std::lround(ce->value.GetW() * 255.0f))
                    );
                    LabelConfig lblCfg;
                    lblCfg.height = 20.0f;
                    lblCfg.scale  = 0.75f;
                    lblCfg.align  = TextAlignment::Center;
                    Label(hexText, lblCfg);
                }
            });

            // The channel sliders can set hsvStale; re-derive here so the pad
            // and the swatch agree with the sliders in the SAME frame rather
            // than lagging one behind.
            if (ce->hsvStale) {
                RgbToHsv(ce->value, ce->hue, ce->sat, ce->val);
                ce->hsvStale = false;
            }
        }

        // --- 5. Apply HSV -> RGB and sync out ---
        //
        // Runs whenever the plane was touched this frame, which is exactly
        // when the RGB has not already been written by a channel slider.
        if (!ce->hsvStale) {
            const JPH::Vec4 rgb = HsvToRgb(ce->hue, ce->sat, ce->val);
            ce->value           = JPH::Vec4(rgb.GetX(), rgb.GetY(), rgb.GetZ(), ce->value.GetW());
        }

        ce->previousValue = ce->value;
        col[0]            = ce->value.GetX();
        col[1]            = ce->value.GetY();
        col[2]            = ce->value.GetZ();
        if (hasAlpha) {
            col[3] = ce->value.GetW();
        }

        return e;
    }

auto Context::ColorEdit3(std::string_view id, std::string_view label, float col[3], const ColorEditConfig& cfg ) -> Entity {
        return ColorEdit(id, label, col, 3, cfg);
    }

auto Context::ColorEdit3(std::string_view label, float col[3], const ColorEditConfig& cfg ) -> Entity {
        return ColorEdit(label, label, col, 3, cfg);
    }

auto Context::ColorEdit4(std::string_view id, std::string_view label, float col[4], const ColorEditConfig& cfg ) -> Entity {
        return ColorEdit(id, label, col, 4, cfg);
    }

auto Context::ColorEdit4(std::string_view label, float col[4], const ColorEditConfig& cfg ) -> Entity {
        return ColorEdit(label, label, col, 4, cfg);
    }

auto Context::ColorEdit4(std::string_view id, std::string_view label, JPH::Vec4& col, const ColorEditConfig& cfg ) -> Entity {
        float v[4] = {col.GetX(), col.GetY(), col.GetZ(), col.GetW()};
        Entity e   = ColorEdit(id, label, v, 4, cfg);
        col        = JPH::Vec4(v[0], v[1], v[2], v[3]);
        return e;
    }

auto Context::ColorEdit4(std::string_view label, JPH::Vec4& col, const ColorEditConfig& cfg ) -> Entity {
        return ColorEdit4(label, label, col, cfg);
    }

auto Context::Selectable(std::string_view id, std::string_view label, bool& selected, const SelectableConfig& cfg) -> Entity {
        uint32_t depth = 0;
        SelectableClickInfo info = PrepareSelectable(id, label, selected, cfg, std::string_view {}, false, depth);
        selected                 = info.selected;
        return info.entity;
    }

auto Context::Selectable(std::string_view id, std::string_view label, bool& selected) -> Entity {
        return Selectable(id, label, selected, SelectableConfig {});
    }

auto Context::Tooltip(std::string_view text, const TooltipConfig& cfg ) -> Entity {
        return TooltipFor(m_lastItem, text, cfg);
    }

auto Context::TooltipFor(Entity owner, std::string_view text, const TooltipConfig& cfg) -> Entity {
        return TooltipForImpl(owner, text, cfg);
    }

auto Context::TooltipFor(Entity owner, std::string_view text) -> Entity {
        return TooltipFor(owner, text, TooltipConfig {});
    }

[[nodiscard]] auto Context::IsHovered(Entity e) const noexcept -> bool {
        if (const auto* btn = m_reg->Get<UIComponents::UIButtonComponent>(e)) {
            return btn->Has(UIButton::Hovered);
        }
        return false;
    }

[[nodiscard]] auto Context::IsItemHovered() const noexcept -> bool {
        return IsHovered(m_lastItem);
    }

[[nodiscard]] auto Context::GetLastItem() const noexcept -> Entity {
        return m_lastItem;
    }

[[nodiscard]] auto Context::BeginPopup(Entity owner, const PopupConfig& cfg ) -> UIScope {
        return BeginPopupImpl(owner, cfg);
    }

auto Context::NextLayoutOrder() -> uint32_t {
        return m_reg->Get<UIComponents::UISettingsComponent>(GetRootCacheEntity())->nextLayoutOrder++;
    }

auto Context::GetRootCacheEntity() -> Entity {
        if (m_rootCacheEntity != Entity::Null() && m_reg->IsAlive(m_rootCacheEntity)) {
            return m_rootCacheEntity;
        }

        auto uiSettings = m_reg->GetEntitiesWith<UIComponents::UISettingsComponent>();
        if (!uiSettings.empty()) {
            m_rootCacheEntity = uiSettings[0];
            return m_rootCacheEntity;
        }

        // Fallback: create UISettingsComponent entity if missing
        m_rootCacheEntity = m_reg->Create(UIComponents::UISettingsComponent {});
        return m_rootCacheEntity;
    }

[[nodiscard]] auto Context::InternalPush(Entity entity, uint32_t depth) noexcept -> bool {
        if (m_stackTop < MAX_UI_STACK_DEPTH) {
            m_stack[m_stackTop++] = {.entity = entity, .depth = depth};
            return true;
        }
        RecordError(Error(GUIError::HierarchyTooDeep)); // surfaced through Status()
        return false;
    }

void Context::InternalPop(bool wasPushed, Entity entity) noexcept {
        if (wasPushed && m_stackTop > 0) {
            --m_stackTop;
        }
        SweepStaleChildren(entity); // a failure latches into Status()
    }

auto Context::PushScope(Entity e, uint32_t depth) noexcept -> UIScope {
        return {this, e, InternalPush(e, depth)};
    }

[[nodiscard]] auto Context::ConsumeClick(Entity e) noexcept -> bool {
        bool clicked = false;
        if (auto* btn = m_reg->Get<UIComponents::UIButtonComponent>(e)) {
            if (btn->Has(UIButton::Clicked)) {
                clicked = true;
                btn->Set(UIButton::Clicked, false);
            }
        }
        return clicked;
    }

[[nodiscard]] auto Context::FindChildByKey(Entity parent, uint64_t childKey) const -> Entity {
        if (const auto* cache = m_reg->Get<UIComponents::UIChildCacheComponent>(parent)) {
            if (const auto* rec = cache->children.Find(childKey)) {
                if (m_reg->IsAlive(rec->entity)) {
                    return rec->entity;
                }
            }
        }
        return Entity::Null();
    }

void Context::EnsureCheckboxChildren(Entity cbEntity, std::string_view label, const CheckboxConfig& cfg, TextureHandle font, bool checked) {
        Entity   parent = cbEntity;
        uint32_t parentDepth = 0;
        if (const auto* r = m_reg->Get<UIComponents::UIRectComponent>(cbEntity)) {
            parentDepth = r->hierarchyDepth;
        }

        // Checkbox box (flex column so the mark can be centered inside)
        Entity boxEnt = GetOrCreateChild(parent, HashStringView("_cb_box"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_cb_box")},
                UIComponents::UIRectComponent {.parentEntity = parent, .width = cfg.boxSize, .height = cfg.boxSize, .hierarchyDepth = parentDepth + 1},
                UIComponents::UIPanelComponent {.color = cfg.boxColor, .borderRadius = cfg.borderRadius, .edgeWidth = 1.0f},
                UIComponents::UIFlexComponent {
                    .direction  = FlexDirection::Column,
                    .justify    = FlexJustify::Center,
                    .alignItems = FlexAlign::Center
                }
            );
        });

        // Check mark (child of box; centered via flex)
        float inset = cfg.boxSize * 0.30f;
        Entity markEnt = GetOrCreateChild(boxEnt, HashStringView("_cb_mark"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_cb_mark")},
                UIComponents::UIRectComponent {.parentEntity = boxEnt, .width = cfg.boxSize - inset, .height = cfg.boxSize - inset, .hierarchyDepth = parentDepth + 2},
                UIComponents::UIPanelComponent {.color = checked ? cfg.checkColor : JPH::Vec4 {0, 0, 0, 0}, .borderRadius = {2.0f, 2.0f, 2.0f, 2.0f}}
            );
        });
        m_reg->Patch<UIComponents::UIPanelComponent>(markEnt, [&](auto& pc) -> auto {
            pc.color = checked ? cfg.checkColor : JPH::Vec4 {0, 0, 0, 0};
        });

        // Label
        Entity lblEnt = GetOrCreateChild(parent, HashStringView("_cb_label"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_cb_label")},
                UIComponents::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                UIComponents::TextComponent {
                    .text          = String256(label),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                },
                UIComponents::UIFlexComponent {.flexGrow = 1.0f}
            );
        });
        m_reg->Patch<UIComponents::TextComponent>(lblEnt, [&](auto& tc) -> auto {
            tc.text.assign(label);
            tc.color = cfg.textColor;
        });
    }

void Context::PatchCheckboxVisuals(Entity e, const CheckboxConfig& cfg, bool checked) {
        bool isHovered = IsHovered(e);
        Entity boxEnt  = FindChildByKey(e, HashStringView("_cb_box"));
        if (boxEnt != Entity::Null()) {
            m_reg->Patch<UIComponents::UIPanelComponent>(boxEnt, [&](auto& panel) -> auto {
                panel.color        = isHovered ? cfg.hoverColor : cfg.boxColor;
                panel.edgeWidth    = 1.0f;
                panel.borderRadius = cfg.borderRadius;
            });
        }
        // Update check-mark visibility (mark is a child of boxEnt)
        if (boxEnt != Entity::Null()) {
            Entity markEnt = FindChildByKey(boxEnt, HashStringView("_cb_mark"));
            if (markEnt != Entity::Null()) {
                m_reg->Patch<UIComponents::UIPanelComponent>(markEnt, [&](auto& mc) -> auto {
                    mc.color = checked ? cfg.checkColor : JPH::Vec4 {0, 0, 0, 0};
                });
            }
        }
    }

void Context::EnsureSliderChildren(Entity sliderEntity, std::string_view label, const SliderConfig& cfg, TextureHandle font, float value, float minVal, float maxVal) {
        Entity   parent = sliderEntity;
        uint32_t parentDepth = 0;
        if (const auto* r = m_reg->Get<UIComponents::UIRectComponent>(sliderEntity)) {
            parentDepth = r->hierarchyDepth;
        }

        // Label
        if (!label.empty()) {
            Entity lblEnt = GetOrCreateChild(parent, HashStringView("_sl_label"), [&]() -> Entity {
                return m_reg->Create(
                    Components::NameComponent {.name = String64("_sl_label")},
                    UIComponents::UIRectComponent {.parentEntity = parent, .width = cfg.labelWidth, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                    UIComponents::TextComponent {
                        .text          = String256(label),
                        .scale         = cfg.scale,
                        .color         = cfg.textColor,
                        .align         = TextAlignment::Left,
                        .verticalAlign = TextVerticalAlignment::Center,
                        .fontIndex     = font
                    }
                );
            });
            m_reg->Patch<UIComponents::UIRectComponent>(lblEnt, [&](auto& r) -> auto { r.width = cfg.labelWidth; });
            m_reg->Patch<UIComponents::TextComponent>(lblEnt, [&](auto& tc) -> auto { tc.text.assign(label); });
        }

        // Track (container for filled region and knob)
        Entity trackEnt = GetOrCreateChild(parent, HashStringView("_sl_track"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_sl_track")},
                UIComponents::UIRectComponent {.parentEntity = parent, .width = 0.0f, .height = cfg.trackHeight, .hierarchyDepth = parentDepth + 1},
                UIComponents::UIPanelComponent {
                    .color        = cfg.trackColor,
                    .borderRadius = {cfg.trackHeight * 0.5f, cfg.trackHeight * 0.5f, cfg.trackHeight * 0.5f, cfg.trackHeight * 0.5f}
                },
                UIComponents::UIFlexComponent {
                    .direction  = FlexDirection::Row,
                    .alignItems = FlexAlign::Center,
                    .flexGrow   = 1.0f,
                    .flexShrink = 1.0f,
                    .flexBasis  = -1.0f
                },
                UIComponents::UIButtonComponent {}
            );
        });

        // The track is a thin visual bar, not another full-height row.  Keep
        // its layout properties in sync for cached widgets as well as newly
        // created ones.
        m_reg->Patch<UIComponents::UIRectComponent>(trackEnt, [&](auto& tr) -> auto {
            tr.parentEntity   = parent;
            tr.width          = 0.0f;
            tr.height         = std::max(0.0f, cfg.trackHeight);
            tr.hierarchyDepth = parentDepth + 1;
        });
        m_reg->Patch<UIComponents::UIFlexComponent>(trackEnt, [&](auto& tf) -> auto {
            tf.direction  = FlexDirection::Row;
            tf.alignItems = FlexAlign::Center;
            tf.flexGrow   = 1.0f;
            tf.flexShrink = 1.0f;
            tf.flexBasis  = -1.0f;
            // The knob's travel is measured across the complete track.  It
            // should not be inset by padding, otherwise the endpoint values
            // never quite reach the ends of the bar.
            tf.paddingLeft = tf.paddingRight = 0.0f;
            tf.paddingTop = tf.paddingBottom = 0.0f;
        });
        m_reg->Patch<UIComponents::UIPanelComponent>(trackEnt, [&](auto& tp) -> auto {
            tp.color        = cfg.trackColor;
            const float r   = std::max(0.0f, cfg.trackHeight) * 0.5f;
            tp.borderRadius = {r, r, r, r};
        });

        // Value text
        if (cfg.showValue) {
            const FixedString<32> valStr = FormatSliderValue(value);
            Entity valEnt = GetOrCreateChild(parent, HashStringView("_sl_value"), [&]() -> Entity {
                return m_reg->Create(
                    Components::NameComponent {.name = String64("_sl_value")},
                    UIComponents::UIRectComponent {.parentEntity = parent, .width = cfg.valueWidth, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                    UIComponents::TextComponent {
                        .text          = String256(std::string_view(valStr)),
                        .scale         = cfg.scale,
                        .color         = cfg.textColor,
                        .align         = TextAlignment::Right,
                        .verticalAlign = TextVerticalAlignment::Center,
                        .fontIndex     = font
                    }
                );
            });
            m_reg->Patch<UIComponents::UIRectComponent>(valEnt, [&](auto& r) -> auto { r.width = cfg.valueWidth; });
            m_reg->Patch<UIComponents::TextComponent>(valEnt, [&](auto& tc) -> auto {
                tc.text.assign(std::string_view(valStr));
            });
        }

        // Knob visual child on the track.  The margin is part of the Yoga
        // layout, so changing the value moves the knob instead of leaving a
        // marker permanently at the track's flex-start edge.
        float range = maxVal - minVal;
        float t     = (range > 0.0f) ? ((value - minVal) / range) : 0.0f;
        t           = std::clamp(t, 0.0f, 1.0f);

        Entity knobEnt = GetOrCreateChild(trackEnt, HashStringView("_sl_knob"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_sl_knob")},
                UIComponents::UIRectComponent {
                    .parentEntity   = trackEnt,
                    .width          = std::max(0.0f, cfg.knobSize),
                    .height         = std::max(0.0f, cfg.knobSize),
                    .hierarchyDepth = parentDepth + 2
                },
                UIComponents::UIPanelComponent {
                    .color        = cfg.knobColor,
                    .borderRadius = {cfg.knobSize * 0.5f, cfg.knobSize * 0.5f, cfg.knobSize * 0.5f, cfg.knobSize * 0.5f}
                },
                UIComponents::UIFlexComponent {},
                UIComponents::UIButtonComponent {},
                UIComponents::UIDragComponent {.targetEntity = sliderEntity, .isDragging = false}
            );
        });

        // `cfg.width` is the best estimate available before Yoga has laid out
        // a fill-width slider.  Once a cached track has a computed width, use
        // that width so the first frame after a resize also converges to the
        // true travel distance.  The fallback keeps the value-dependent
        // position useful on the very first frame of a fill-width widget.
        float trackWidth = 0.0f;
        if (const auto* tr = m_reg->Get<UIComponents::UIRectComponent>(trackEnt)) {
            trackWidth = tr->computedAbsMaxX - tr->computedAbsMinX;
        }
        if (trackWidth <= 0.0f) {
            trackWidth = cfg.width;
        }
        if (trackWidth <= 0.0f) {
            trackWidth = 100.0f;
        }
        const float knobSize   = std::max(0.0f, cfg.knobSize);
        const float knobTravel = std::max(0.0f, trackWidth - knobSize);

        m_reg->Patch<UIComponents::UIRectComponent>(knobEnt, [&](auto& kr) -> auto {
            kr.parentEntity   = trackEnt;
            kr.width          = knobSize;
            kr.height         = knobSize;
            kr.hierarchyDepth = parentDepth + 2;
        });
        // GetOrCreateChild also supports widgets created by an older build;
        // make sure those cached knobs acquire the flex component needed for
        // marginLeft before patching it.
        if (m_reg->Get<UIComponents::UIFlexComponent>(knobEnt) == nullptr) {
            m_reg->Add<UIComponents::UIFlexComponent>(knobEnt);
        }
        m_reg->Patch<UIComponents::UIFlexComponent>(knobEnt, [&](auto& kf) -> auto {
            kf.flexGrow   = 0.0f;
            kf.flexShrink = 0.0f;
            kf.flexBasis  = -1.0f;
            kf.marginLeft = t * knobTravel;
            kf.marginTop = kf.marginRight = kf.marginBottom = 0.0f;
        });
        m_reg->Patch<UIComponents::UIPanelComponent>(knobEnt, [&](auto& pc) -> auto {
            // Hover on either the root slider entity, the track, or the knob itself
            bool hover  = IsHovered(sliderEntity) || IsHovered(trackEnt) || IsHovered(knobEnt);
            auto* s     = m_reg->Get<UIComponents::UISliderComponent>(sliderEntity);
            bool active = (s != nullptr && s->isDragging) || hover;
            pc.color = active ? cfg.hoverColor : cfg.knobColor;
            pc.borderRadius = {knobSize * 0.5f, knobSize * 0.5f, knobSize * 0.5f, knobSize * 0.5f};
        });
    }

void Context::EnsureTextInputLabel(Entity inputEnt, std::string_view label, const TextInputConfig& cfg, TextureHandle font) {
        uint32_t parentDepth = 0;
        if (const auto* r = m_reg->Get<UIComponents::UIRectComponent>(inputEnt)) {
            parentDepth = r->hierarchyDepth;
        }
        Entity lblEnt = GetOrCreateChild(inputEnt, HashStringView("_ti_label"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ti_label")},
                UIComponents::UIRectComponent {.parentEntity = inputEnt, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                UIComponents::TextComponent {
                    .text          = String256(label),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                }
            );
        });
        m_reg->Patch<UIComponents::TextComponent>(lblEnt, [&](auto& tc) -> auto { tc.text.assign(label); });
    }

void Context::PatchTextInputVisuals(Entity e, const TextInputConfig& cfg, bool focused, TextureHandle font) {
        m_reg->Patch<UIComponents::UIPanelComponent>(e, [&](auto& pc) -> auto {
            pc.color        = focused ? cfg.focusedColor : cfg.bgColor;
            pc.edgeWidth    = 1.0f;
            pc.borderRadius = cfg.borderRadius;
        });

        // Ensure the leaf text child exists (leaf — no children, so Yoga may
        // safely attach a measure function to it). Sync it from the
        // UITextInputComponent which is the authoritative text store.
        if (auto* input = m_reg->Get<UIComponents::UITextInputComponent>(e)) {
            uint32_t parentDepth = 0;
            if (const auto* r = m_reg->Get<UIComponents::UIRectComponent>(e)) {
                parentDepth = r->hierarchyDepth;
            }
            Entity textEnt = GetOrCreateChild(e, HashStringView("_ti_text"), [&]() -> Entity {
                return m_reg->Create(
                    Components::NameComponent {.name = String64("_ti_text")},
                    UIComponents::UIRectComponent {.parentEntity = e, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                    UIComponents::UIFlexComponent {.flexGrow = 1.0f},
                    UIComponents::TextComponent {
                        .text          = input->text,
                        .scale         = cfg.scale,
                        .color         = cfg.textColor,
                        .align         = TextAlignment::Left,
                        .verticalAlign = TextVerticalAlignment::Center,
                        .fontIndex     = font
                    }
                );
            });
            // Keep parent/depth/sizing correct each frame
            m_reg->Patch<UIComponents::UIRectComponent>(textEnt, [&](auto& tr) -> auto {
                tr.parentEntity   = e;
                tr.hierarchyDepth = parentDepth + 1;
                tr.height         = cfg.height;
            });
            // Sync displayed text
            m_reg->Patch<UIComponents::TextComponent>(textEnt, [&](auto& tc) -> auto {
                tc.text      = input->text;
                tc.color     = focused ? JPH::Vec4 {1.0f, 1.0f, 1.0f, 1.0f} : cfg.textColor;
                tc.scale     = cfg.scale;
                tc.fontIndex = font;
            });
            if (auto* tflex = m_reg->Get<UIComponents::UIFlexComponent>(textEnt)) {
                tflex->flexGrow = 1.0f;
            }
        }
    }

void Context::EnsureDropdownHeader(Entity ddEnt, std::string_view displayText, const DropdownConfig& cfg, TextureHandle font) {
        Entity   parent = ddEnt;
        uint32_t parentDepth = 0;
        if (const auto* r = m_reg->Get<UIComponents::UIRectComponent>(ddEnt)) {
            parentDepth = r->hierarchyDepth;
        }

        Entity txtEnt = GetOrCreateChild(parent, HashStringView("_dd_text"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_dd_text")},
                UIComponents::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                UIComponents::TextComponent {
                    .text          = String256(displayText),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                },
                UIComponents::UIFlexComponent {.flexGrow = 1.0f}
            );
        });
        m_reg->Patch<UIComponents::TextComponent>(txtEnt, [&](auto& tc) -> auto { tc.text.assign(displayText); });

        // Arrow
        Entity arrowEnt = GetOrCreateChild(parent, HashStringView("_dd_arrow"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_dd_arrow")},
                UIComponents::UIRectComponent {.parentEntity = parent, .width = 16.0f, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                UIComponents::TextComponent {
                    .text          = String256("v"),
                    .scale         = cfg.scale,
                    .color         = cfg.arrowColor,
                    .align         = TextAlignment::Right,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                }
            );
        });
        m_reg->Patch<UIComponents::TextComponent>(arrowEnt, [&](auto& tc) -> auto {
            auto* dd = m_reg->Get<UIComponents::UIDropdownComponent>(ddEnt);
            tc.text.assign((dd != nullptr && dd->expanded) ? "^" : "v");
        });
    }

auto Context::GetOrCreateOverlayRoot() -> Entity {
        if (m_overlayRoot != Entity::Null() && m_reg->IsAlive(m_overlayRoot)) {
            return m_overlayRoot;
        }

        static constexpr std::string_view kOverlayName = "__ui_overlay_root__";
        Entity cache = GetRootCacheEntity();
        m_overlayRoot = GetOrCreateChild(cache, HashStringView(kOverlayName), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(kOverlayName)},
                UIComponents::UIRectComponent {
                    .parentEntity   = Entity::Null(),
                    .width          = 0.0f,
                    .height         = 0.0f,
                    .hierarchyDepth = UI_OVERLAY_DEPTH,
                    .clipChildren   = false
                }
            );
        });
        m_reg->Patch<UIComponents::UIRectComponent>(m_overlayRoot, [&](auto& r) -> auto {
            r.parentEntity   = Entity::Null();
            r.hierarchyDepth = UI_OVERLAY_DEPTH;
            r.clipChildren   = false;
        });
        return m_overlayRoot;
    }

[[nodiscard]] auto Context::GetOwnerAnchor(Entity owner) const noexcept -> OwnerAnchor {
        OwnerAnchor a {};
        if (const auto* r = m_reg->Get<UIComponents::UIRectComponent>(owner)) {
            a.x      = r->computedAbsMinX;
            a.y      = r->computedAbsMinY;
            a.width  = r->computedAbsMaxX - r->computedAbsMinX;
            a.height = r->computedAbsMaxY - r->computedAbsMinY;
            a.valid  = (a.width > 0.0f) || (a.height > 0.0f);
        }
        return a;
    }

[[nodiscard]] auto Context::GetPointerPosition() const noexcept -> PointerPosition {
        if (const auto* input = m_reg->GetSingleton<Components::InputStateComponent>()) {
            return {.x = input->mouseX, .y = input->mouseY, .present = true};
        }
        return {};
    }

auto Context::PrepareCollapsingHeader(std::string_view id, std::string_view label, bool defaultOpen, const CollapsingHeaderConfig& cfg, uint32_t& outDepth) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                UIComponents::UIRectComponent {.parentEntity = parent, .height = 0.0f, .hierarchyDepth = depth},
                UIComponents::UIPanelComponent {.color = defaultOpen ? cfg.openColor : cfg.bgColor},
                UIComponents::UIFlexComponent {
                    .direction     = FlexDirection::Column,
                    .paddingLeft   = 0.0f,
                    .paddingTop    = 0.0f,
                    .paddingRight  = 0.0f,
                    .paddingBottom = 0.0f,
                    .gapX          = 0.0f,
                    .gapY          = 0.0f
                },
                UIComponents::UICollapsingHeaderComponent {.isOpen = defaultOpen, .defaultOpen = defaultOpen}
            );
        });

        auto* hdr = m_reg->Get<UIComponents::UICollapsingHeaderComponent>(e);

        // Ensure the clickable header button child exists. The title child
        // carries the UIButtonComponent so its hover flag is the source of
        // truth for hover visuals.
        EnsureCollapsingHeaderTitle(e, label, cfg, fontHandle, hdr->isOpen);

        // The clickable _title child carries the UIButtonComponent; consume
        // a click there to toggle open/closed.
        bool   titleClicked = false;
        Entity titleEnt     = FindChildByKey(e, HashStringView("_title"));
        if (titleEnt != Entity::Null()) {
            titleClicked = ConsumeClick(titleEnt);
        }
        if (titleClicked) {
            hdr->isOpen = !hdr->isOpen;
        }

        // Update panel colour to reflect open/hover state. Hover is read from
        // the title child's UIButtonComponent (single source of truth).
        bool titleHover = (titleEnt != Entity::Null()) && IsHovered(titleEnt);
        m_reg->Patch<UIComponents::UIPanelComponent>(e, [&](auto& pc) -> auto {
            pc.color = titleHover ? cfg.hoverColor : (hdr->isOpen ? cfg.openColor : cfg.bgColor);
        });

        outDepth = depth;
        return e;
    }

void Context::EnsureCollapsingHeaderTitle(Entity hdrEntity, std::string_view label, const CollapsingHeaderConfig& cfg, TextureHandle font, bool isOpen) {
        Entity   parent = hdrEntity;
        uint32_t parentDepth = 0;
        if (const auto* r = m_reg->Get<UIComponents::UIRectComponent>(hdrEntity)) {
            parentDepth = r->hierarchyDepth;
        }

        Entity titleEnt = GetOrCreateChild(parent, HashStringView("_title"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_title")},
                UIComponents::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                UIComponents::UIPanelComponent {.color = {0, 0, 0, 0}},
                UIComponents::UIFlexComponent {
                    .direction     = FlexDirection::Row,
                    .alignItems    = FlexAlign::Center,
                    .paddingLeft   = cfg.padding,
                    .paddingRight  = cfg.padding
                },
                UIComponents::UIButtonComponent {}
            );
        });

        // Arrow + label inside the title
        Entity arrowEnt = GetOrCreateChild(titleEnt, HashStringView("_ch_arrow"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ch_arrow")},
                UIComponents::UIRectComponent {.parentEntity = titleEnt, .width = 16.0f, .height = cfg.height, .hierarchyDepth = parentDepth + 2},
                UIComponents::TextComponent {
                    .text          = String256(isOpen ? "v" : ">"),
                    .scale         = cfg.scale,
                    .color         = cfg.arrowColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                }
            );
        });
        m_reg->Patch<UIComponents::TextComponent>(arrowEnt, [&](auto& tc) -> auto {
            tc.text.assign(isOpen ? "v" : ">");
            tc.color = cfg.arrowColor;
        });

        Entity lblEnt = GetOrCreateChild(titleEnt, HashStringView("_ch_label"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ch_label")},
                UIComponents::UIRectComponent {.parentEntity = titleEnt, .height = cfg.height, .hierarchyDepth = parentDepth + 2},
                UIComponents::TextComponent {
                    .text          = String256(label),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                },
                UIComponents::UIFlexComponent {.flexGrow = 1.0f}
            );
        });
        m_reg->Patch<UIComponents::TextComponent>(lblEnt, [&](auto& tc) -> auto {
            tc.text.assign(label);
            tc.color = cfg.textColor;
        });
    }

void Context::ApplySelectableVisual(Entity row, const SelectableConfig& cfg, bool selected) {
        const bool hovered = IsHovered(row);
        const bool active  = selected && hovered;
        m_reg->Patch<UIComponents::UIPanelComponent>(row, [&](auto& pc) -> auto {
            pc.color        = active ? cfg.activeColor : (selected ? cfg.selectedColor : (hovered ? cfg.hoverColor : cfg.normalColor));
            pc.borderRadius = cfg.borderRadius;
        });
        if (Entity lbl = FindChildByKey(row, HashStringView("_sel_label")); lbl != Entity::Null()) {
            m_reg->Patch<UIComponents::TextComponent>(lbl, [&](auto& tc) -> auto { tc.color = selected ? cfg.selectedTextColor : cfg.textColor; });
        }
    }

void Context::ApplySelectableSelection(Entity row, Entity stateEnt, const SelectableConfig& cfg, bool selected) {
        if (auto* sel = m_reg->Get<UIComponents::UISelectableComponent>(stateEnt)) {
            sel->selected = selected;
        }
        ApplySelectableVisual(row, cfg, selected);
    }

void Context::PatchSelectableArrow(Entity rowEntity, std::string_view glyph, JPH::Vec4 color) {
        Entity arrowEnt = FindChildByKey(rowEntity, HashStringView("_sel_arrow"));
        if (arrowEnt == Entity::Null()) {
            return;
        }
        m_reg->Patch<UIComponents::TextComponent>(arrowEnt, [&](auto& tc) -> auto {
            tc.text.assign(glyph);
            tc.color = color;
        });
    }

void Context::UpdateScrollbar(Entity root, Entity viewport, const ScrollBoxConfig& cfg, uint32_t depth) {
        if (cfg.scrollbarWidth <= 0.0f) {
            return;
        }

        Entity track = GetOrCreateChild(root, HashStringView("_sb_track"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_sb_track")},
                UIComponents::UIRectComponent {.parentEntity = root, .width = cfg.scrollbarWidth, .height = 0.0f, .hierarchyDepth = depth + 1},
                UIComponents::UIPanelComponent {.color = cfg.trackColor},
                UIComponents::UIFlexComponent {.direction = FlexDirection::Column, .flexGrow = 0.0f, .flexShrink = 0.0f, .flexBasis = cfg.scrollbarWidth}
            );
        });
        m_reg->Patch<UIComponents::UIRectComponent>(track, [&](auto& r) -> auto {
            r.parentEntity   = root;
            r.width          = cfg.scrollbarWidth;
            r.height         = 0.0f;
            r.hierarchyDepth = depth + 1;
        });
        m_reg->Patch<UIComponents::UIFlexComponent>(track, [&](auto& f) -> auto {
            f.direction  = FlexDirection::Column;
            f.flexGrow   = 0.0f;
            f.flexShrink = 0.0f;
            f.flexBasis  = cfg.scrollbarWidth;
        });
        m_reg->Patch<UIComponents::UIPanelComponent>(track, [&](auto& pc) -> auto { pc.color = cfg.trackColor; });

        const auto* scroll = m_reg->Get<UIComponents::UIScrollComponent>(viewport);
        const auto* vrect  = m_reg->Get<UIComponents::UIRectComponent>(viewport);
        const auto* trect  = m_reg->Get<UIComponents::UIRectComponent>(track);

        float trackHeight = (trect != nullptr) ? (trect->computedAbsMaxY - trect->computedAbsMinY) : 0.0f;
        if (trackHeight <= 0.0f && vrect != nullptr) {
            trackHeight = vrect->computedAbsMaxY - vrect->computedAbsMinY;
        }
        if (trackHeight <= 0.0f) {
            trackHeight = cfg.height;
        }

        float viewHeight = (vrect != nullptr) ? (vrect->computedAbsMaxY - vrect->computedAbsMinY) : cfg.height;
        if (viewHeight <= 0.0f) {
            viewHeight = cfg.height;
        }

        const float contentHeight = (scroll != nullptr) ? std::max(scroll->contentHeight, viewHeight) : viewHeight;
        const float visibleRatio  = (contentHeight > 0.0f) ? std::clamp(viewHeight / contentHeight, 0.0f, 1.0f) : 1.0f;
        float       thumbHeight   = std::clamp(trackHeight * visibleRatio, 16.0f, std::max(16.0f, trackHeight));
        thumbHeight               = std::min(thumbHeight, std::max(16.0f, trackHeight));

        const float maxScroll = (scroll != nullptr) ? scroll->maxScrollY : 0.0f;
        const float fraction  = (maxScroll > 0.0f && scroll != nullptr) ? std::clamp(scroll->scrollY / maxScroll, 0.0f, 1.0f) : 0.0f;
        const float travel    = std::max(0.0f, trackHeight - thumbHeight);
        const bool  hovered   = IsHovered(track);

        Entity thumb = GetOrCreateChild(track, HashStringView("_sb_thumb"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_sb_thumb")},
                UIComponents::UIRectComponent {.parentEntity = track, .width = cfg.scrollbarWidth, .height = thumbHeight, .hierarchyDepth = depth + 2},
                UIComponents::UIPanelComponent {.color = cfg.thumbColor, .borderRadius = {cfg.scrollbarWidth * 0.5f, cfg.scrollbarWidth * 0.5f, cfg.scrollbarWidth * 0.5f, cfg.scrollbarWidth * 0.5f}},
                UIComponents::UIFlexComponent {.flexGrow = 0.0f, .flexShrink = 0.0f, .flexBasis = -1.0f},
                UIComponents::UIButtonComponent {}
            );
        });
        m_reg->Patch<UIComponents::UIRectComponent>(thumb, [&](auto& r) -> auto {
            r.parentEntity   = track;
            r.width          = cfg.scrollbarWidth;
            r.height         = thumbHeight;
            r.hierarchyDepth = depth + 2;
        });
        m_reg->Patch<UIComponents::UIFlexComponent>(thumb, [&](auto& f) -> auto {
            f.flexGrow   = 0.0f;
            f.flexShrink = 0.0f;
            f.flexBasis  = -1.0f;
            f.marginTop  = fraction * travel;
        });
        m_reg->Patch<UIComponents::UIPanelComponent>(thumb, [&](auto& pc) -> auto {
            pc.color = hovered ? cfg.thumbHoverColor : cfg.thumbColor;
        });
    }

[[nodiscard]] auto Context::BeginPopupImpl(Entity owner, const PopupConfig& cfg) -> UIScope {
        Entity   overlay = GetOrCreateOverlayRoot();
        uint64_t key     = HashCombine(owner.Pack(), HashStringView("__popup__"));

        const OwnerAnchor anchor = GetOwnerAnchor(owner);

        // Flip-above needs the popup's height, which only exists once the
        // popup has been laid out at least once; until then it opens downward.
        float lastHeight = 0.0f;
        if (Entity prev = FindChildByKey(overlay, key); prev != Entity::Null()) {
            if (const auto* pr = m_reg->Get<UIComponents::UIRectComponent>(prev)) {
                lastHeight = pr->computedAbsMaxY - pr->computedAbsMinY;
            }
        }

        const float popupWidth = (cfg.width > 0.0f) ? cfg.width : std::max(anchor.width, 120.0f);
        const float popupX     = anchor.x;
        const float popupY     = cfg.openUpward ? std::max(0.0f, anchor.y - lastHeight) : (anchor.y + anchor.height);

        Entity popup = GetOrCreateChild(overlay, key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ui_popup")},
                UIComponents::UIRectComponent {
                    .parentEntity   = overlay,
                    .x              = popupX,
                    .y              = popupY,
                    .width          = popupWidth,
                    .height         = cfg.height,
                    .hierarchyDepth = UI_OVERLAY_DEPTH + 1,
                    .clipChildren   = false
                },
                UIComponents::UIPanelComponent {.color = cfg.bgColor, .borderRadius = cfg.borderRadius, .edgeWidth = 1.0f},
                UIComponents::UIFlexComponent {
                    .direction     = FlexDirection::Column,
                    .alignItems    = FlexAlign::Stretch,
                    .paddingLeft   = cfg.padding,
                    .paddingTop    = cfg.padding,
                    .paddingRight  = cfg.padding,
                    .paddingBottom = cfg.padding,
                    .gapX          = cfg.gap,
                    .gapY          = cfg.gap
                },
                UIComponents::UIPopupComponent {.owner = owner, .open = true}
            );
        });

        m_reg->Patch<UIComponents::UIRectComponent>(popup, [&](auto& r) -> auto {
            r.parentEntity   = overlay;
            r.x              = popupX;
            r.y              = popupY;
            r.width          = popupWidth;
            r.height         = cfg.height;
            r.hierarchyDepth = UI_OVERLAY_DEPTH + 1;
            r.clipChildren   = false;
        });
        m_reg->Patch<UIComponents::UIPanelComponent>(popup, [&](auto& pc) -> auto {
            pc.color        = cfg.bgColor;
            pc.borderRadius = cfg.borderRadius;
            pc.edgeWidth    = 1.0f;
        });
        m_reg->Patch<UIComponents::UIPopupComponent>(popup, [&](auto& p) -> auto { p.owner = owner; });

        return PushScope(popup, UI_OVERLAY_DEPTH + 1);
    }

auto Context::TooltipForImpl(Entity owner, std::string_view text, const TooltipConfig& cfg) -> Entity {
        if (owner == Entity::Null() || !m_reg->IsAlive(owner) || text.empty()) {
            return Entity::Null();
        }

        // The hover clock lives on the owner (as a cache record carrying no
        // geometry), so the delay survives between frames without the Context
        // having to remember anything.
        Entity stateEnt = GetOrCreateChild(owner, HashStringView("_ui_tooltip"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ui_tooltip")},
                UIComponents::UITooltipComponent {.text = String256(text), .delayFrames = cfg.delayFrames}
            );
        });
        auto* tip = m_reg->Get<UIComponents::UITooltipComponent>(stateEnt);
        tip->text.assign(text);
        tip->delayFrames     = cfg.delayFrames;
        tip->scale           = cfg.scale;
        tip->bgColor         = cfg.bgColor;
        tip->textColor       = cfg.textColor;
        tip->borderColor     = cfg.borderColor;
        tip->offsetX         = cfg.offsetX;
        tip->offsetY         = cfg.offsetY;

        if (!IsHovered(owner)) {
            tip->hoverStartFrame = 0;
            return Entity::Null(); // Not rebuilt -> swept from the overlay below.
        }
        if (tip->hoverStartFrame == 0) {
            tip->hoverStartFrame = m_currentFrame;
        }
        if (m_currentFrame < tip->hoverStartFrame + tip->delayFrames) {
            return Entity::Null();
        }

        // Size the bubble from the shaped text so it never clips its own copy.
        const FontAtlas* font       = ResolveFontAtlas();
        const float      innerMax   = (cfg.maxWidth > 0.0f) ? std::max(0.0f, cfg.maxWidth - 2.0f * cfg.padding) : 0.0f;
        float            textWidth  = 0.0f;
        float            textHeight = (font != nullptr) ? TextLineHeight(cfg.scale) : 20.0f;
        if (font != nullptr) {
            const TextBounds b = MeasureWrappedTextBounds(*font, text, cfg.scale, innerMax);
            textWidth          = b.width();
            textHeight         = std::max(b.height(), TextLineHeight(cfg.scale));
        } else {
            textWidth = static_cast<float>(text.size()) * 8.0f * cfg.scale;
        }

        const float boxWidth  = textWidth + 2.0f * cfg.padding;
        const float boxHeight = textHeight + 2.0f * cfg.padding;

        // Anchor at the pointer when the engine publishes one, otherwise hang
        // the bubble off the owner's rect (keeps headless tests deterministic).
        const OwnerAnchor    anchor  = GetOwnerAnchor(owner);
        const PointerPosition pointer = GetPointerPosition();
        const float          baseX   = pointer.present ? pointer.x : anchor.x;
        const float          baseY   = pointer.present ? pointer.y : (anchor.y + anchor.height);

        Entity overlay = GetOrCreateOverlayRoot();
        uint64_t key   = HashCombine(owner.Pack(), HashStringView("__tooltip__"));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity box = GetOrCreateChild(overlay, key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ui_tooltip_box")},
                UIComponents::UIRectComponent {
                    .parentEntity   = overlay,
                    .x              = baseX + cfg.offsetX,
                    .y              = baseY + cfg.offsetY,
                    .width          = boxWidth,
                    .height         = boxHeight,
                    .hierarchyDepth = UI_OVERLAY_DEPTH + 1,
                    .clipChildren   = false
                },
                UIComponents::UIPanelComponent {.color = cfg.bgColor, .borderRadius = {3.0f, 3.0f, 3.0f, 3.0f}, .edgeWidth = 1.0f},
                UIComponents::UIFlexComponent {
                    .direction     = FlexDirection::Column,
                    .paddingLeft   = cfg.padding,
                    .paddingTop    = cfg.padding,
                    .paddingRight  = cfg.padding,
                    .paddingBottom = cfg.padding
                },
                UIComponents::UIPopupComponent {.owner = owner, .open = true}
            );
        });

        m_reg->Patch<UIComponents::UIRectComponent>(box, [&](auto& r) -> auto {
            r.parentEntity   = overlay;
            r.x              = baseX + cfg.offsetX;
            r.y              = baseY + cfg.offsetY;
            r.width          = boxWidth;
            r.height         = boxHeight;
            r.hierarchyDepth = UI_OVERLAY_DEPTH + 1;
        });
        m_reg->Patch<UIComponents::UIPanelComponent>(box, [&](auto& pc) -> auto { pc.color = cfg.bgColor; });

        Entity textEnt = GetOrCreateChild(box, HashStringView("_ui_tooltip_text"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ui_tooltip_text")},
                UIComponents::UIRectComponent {.parentEntity = box, .width = 0.0f, .height = 0.0f, .hierarchyDepth = UI_OVERLAY_DEPTH + 2},
                UIComponents::TextComponent {
                    .text          = String256(text),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = fontHandle,
                    .wrapText      = (innerMax > 0.0f),
                    .wrapWidth     = innerMax
                },
                UIComponents::UIFlexComponent {.flexGrow = 1.0f}
            );
        });
        m_reg->Patch<UIComponents::TextComponent>(textEnt, [&](auto& tc) -> auto {
            tc.text.assign(text);
            tc.color    = cfg.textColor;
            tc.scale    = cfg.scale;
            tc.wrapText = (innerMax > 0.0f);
            tc.wrapWidth = innerMax;
        });

        return box;
    }

[[nodiscard]] auto Context::ResolveFontAtlas() const noexcept -> const FontAtlas* {
        const auto uiSettings = m_reg->GetEntitiesWith<UIComponents::UISettingsComponent>();
        if (!uiSettings.empty()) {
            return &m_reg->Get<UIComponents::UISettingsComponent>(uiSettings[0])->fontAtlas;
        }
        return nullptr;
    }

[[nodiscard]] auto Context::ResolveFontTexture() const noexcept -> TextureHandle {
        const auto uiSettings = m_reg->GetEntitiesWith<UIComponents::UISettingsComponent>();
        if (!uiSettings.empty()) {
            if (const auto* s = m_reg->Get<UIComponents::UISettingsComponent>(uiSettings[0])) {
                return s->fontAtlas.texture;
            }
        }
        return TextureHandle::Invalid;
    }

auto Context::FormatSliderValue(float value) noexcept -> FixedString<32> {
        std::array<char, 32> buf {};
        const size_t         len = Detail::FormatDouble(buf.data(), buf.size(), static_cast<double>(value), 3);
        size_t               end = std::min(len, buf.size());
        if (std::string_view(buf.data(), end).find('.') != std::string_view::npos) {
            while (end > 0 && buf[end - 1] == '0') {
                --end;
            }
            if (end > 0 && buf[end - 1] == '.') {
                --end;
            }
        }
        FixedString<32> out;
        out.assign(std::string_view(buf.data(), end));
        return out;
    }

void Context::RecordError(Error err) noexcept {
        if (!m_error) {
            m_error = err;
        }
    }

} // namespace ZHLN::GUI
