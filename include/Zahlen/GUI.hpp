// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Components.hpp"
#include "Types.hpp"
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <string_view>
#include <utility>

namespace ZHLN::GUI {

struct TextBounds {
    float               minX = 0.0f;
    float               maxX = 0.0f;
    float               minY = 0.0f;
    float               maxY = 0.0f;
    [[nodiscard]] float width() const noexcept {
        return maxX - minX;
    }
    [[nodiscard]] float height() const noexcept {
        return maxY - minY;
    }
};

TextBounds MeasureTextBounds(const FontAtlas& font, std::string_view text, float scale) noexcept;

uint32_t AppendTextVertices(
    VertexPosition*    outPos,
    VertexAttributes*  outAttr,
    const FontAtlas&   font,
    const std::string& text,
    float              x,
    float              y,
    float              scale,
    const JPH::Vec4&   color
);

uint32_t
    AppendPanelVertices(VertexPosition* outPos, VertexAttributes* outAttr, const Components::UIRectComponent& rect, const Components::UIPanelComponent& panel);

// ============================================================================
// FIBER-SAFE & ZERO-ALLOCATION GUI CONTEXT
// ============================================================================

constexpr size_t MAX_UI_STACK_DEPTH = 32;

struct UIScopeNode {
    Entity   entity = NullEntity;
    uint32_t depth  = 1;
};

// --- CONFIGURATION STRUCTS ---

struct PanelConfig {
    float width      = 0.0f; // 0 = Flex Auto
    float height     = 0.0f;
    float x          = 0.0f;
    float y          = 0.0f;
    float anchorMinX = 0.5f, anchorMinY = 0.5f;
    float anchorMaxX = 0.5f, anchorMaxY = 0.5f;

    JPH::Vec4 color        = {0.08f, 0.11f, 0.16f, 0.95f};
    JPH::Vec4 borderRadius = {0.0f, 0.0f, 0.0f, 0.0f};
    float     edgeWidth    = 1.0f;
    bool      clipChildren = true;

    FlexDirection direction  = FlexDirection::Column;
    FlexJustify   justify    = FlexJustify::FlexStart;
    FlexAlign     alignItems = FlexAlign::Stretch;
    float         gap        = 8.0f;
    float         padding    = 16.0f;
};

struct BoxConfig {
    float width  = 0.0f;
    float height = 0.0f;

    JPH::Vec4 color     = {0.05f, 0.07f, 0.11f, 0.85f};
    float     edgeWidth = 1.0f;

    FlexDirection direction  = FlexDirection::Column;
    FlexJustify   justify    = FlexJustify::FlexStart;
    FlexAlign     alignItems = FlexAlign::Stretch;
    float         gap        = 4.0f;
    float         padding    = 8.0f;
    float         margin     = 0.0f;
};

struct LabelConfig {
    float                 scale         = 0.85f;
    JPH::Vec4             color         = {0.9f, 0.95f, 1.0f, 1.0f};
    TextAlignment         align         = TextAlignment::Left;
    TextVerticalAlignment verticalAlign = TextVerticalAlignment::Center;
    float                 height        = 24.0f;
};

struct ButtonConfig {
    float                 width         = 0.0f;
    float                 height        = 48.0f;
    float                 scale         = 0.85f;
    JPH::Vec4             normalColor   = {0.16f, 0.24f, 0.36f, 0.95f};
    JPH::Vec4             hoverColor    = {0.26f, 0.38f, 0.58f, 1.0f};
    JPH::Vec4             pressedColor  = {0.10f, 0.14f, 0.22f, 1.0f};
    JPH::Vec4             textColor     = {0.90f, 0.95f, 1.0f, 1.0f};
    JPH::Vec4             borderRadius  = {4.0f, 4.0f, 4.0f, 4.0f};
    TextAlignment         align         = TextAlignment::Center;
    TextVerticalAlignment verticalAlign = TextVerticalAlignment::Center;
};

// --- STACK-ALLOCATED FIBER-SAFE CONTEXT ---

class Context {
  public:
    explicit Context(ECS::Registry& reg, uint64_t currentFrame = 0) noexcept: m_reg(&reg), m_currentFrame(currentFrame) {
    }

    [[nodiscard]] ECS::Registry& GetRegistry() const noexcept {
        return *m_reg;
    }

    [[nodiscard]] Entity GetCurrentParent() const noexcept {
        return (m_stackTop > 0) ? m_stack[m_stackTop - 1].entity : NullEntity;
    }

    [[nodiscard]] uint32_t GetCurrentDepth() const noexcept {
        return (m_stackTop > 0) ? m_stack[m_stackTop - 1].depth + 1 : 1;
    }

    bool PushParent(Entity entity, uint32_t depth) noexcept {
        if (m_stackTop < MAX_UI_STACK_DEPTH) {
            m_stack[m_stackTop++] = {.entity = entity, .depth = depth};
            return true;
        }
        // Past the cap the content lambda still runs, but its widgets silently
        // attach to the deepest live parent. Say so once per context instead of
        // letting the tree look "buggy" with no explanation.
        if (!m_stackOverflowLogged) {
            m_stackOverflowLogged = true;
            ZHLN::Log(
                "[GUI::Context] ERROR: Exceeded maximum UI hierarchy depth of {}. Widgets below this depth will attach to the deepest live parent instead.",
                MAX_UI_STACK_DEPTH
            );
        }
        return false;
    }

    void PopParent(bool wasPushed) noexcept {
        if (wasPushed && m_stackTop > 0) {
            m_stackTop--;
        }
    }

    // Resolves or creates the root cache entity (UISettingsComponent)
    Entity GetRootCacheEntity() {
        if (m_rootCacheEntity != NullEntity && m_reg->IsAlive(m_rootCacheEntity)) {
            return m_rootCacheEntity;
        }

        auto uiSettings = m_reg->GetEntitiesWith<Components::UISettingsComponent>();
        if (!uiSettings.empty()) {
            m_rootCacheEntity = uiSettings[0];
            return m_rootCacheEntity;
        }

        // Fallback: create UISettingsComponent entity if missing
        m_rootCacheEntity = m_reg->Create(Components::UISettingsComponent {});
        return m_rootCacheEntity;
    }

    // O(1) Hash-based Entity Lookup with Mark-and-Sweep GC
    template <typename CreateFn>
    Entity GetOrCreateEntity(uint64_t widgetKey, CreateFn&& createFn) {
        Entity parent      = GetCurrentParent();
        Entity cacheEntity = (parent != NullEntity) ? parent : GetRootCacheEntity();

        auto* cache = m_reg->Get<Components::UIChildCacheComponent>(cacheEntity);
        if (cache == nullptr) {
            cache = &m_reg->Add<Components::UIChildCacheComponent>(cacheEntity);
        }

        // 1. O(1) Lookup in cache
        if (const auto* record = cache->children.Find(widgetKey)) {
            if (m_reg->IsAlive(record->entity)) {
                record->lastVisitedFrame = m_currentFrame;
                return record->entity;
            }
            // The widget entity was destroyed outside the GUI context (e.g. a
            // plain Registry::Destroy). We transparently respawn it below, and
            // the insert overwrites the dead record - log it, because silent
            // respawns are exactly how "flickering widget" bugs present.
            ZHLN::Log(
                "[GUI::Context] Cached UI entity ({}:{}) was destroyed externally; respawning widget under parent ({}:{}). Use DestroyUIEntity() for intentional subtree removal.",
                record->entity.index, record->entity.generation, cacheEntity.index, cacheEntity.generation
            );
        }

        // 2. Not found -> Spawn new entity
        Entity newEntity = createFn();
        cache->children.Insert(widgetKey, Components::UIChildCacheComponent::ChildRecord {.entity = newEntity, .lastVisitedFrame = m_currentFrame});

        return newEntity;
    }

    void DestroyUIEntity(Entity ent) noexcept {
        if (!m_reg->IsAlive(ent)) {
            return;
        }

        ZHLN::Log<LogChannel::StdErr, LogLevel::Verbose>(
            "[GUI::Context] DestroyUIEntity: destroying UI entity ({}:{}) and its subtree.", ent.index, ent.generation
        );

        // Gather all child UI rects referencing 'ent' as their parent
        auto                uiRects = m_reg->GetEntitiesWith<Components::UIRectComponent>();
        ZHLN::Array<Entity> childrenToDestroy;

        for (Entity e: uiRects) {
            if (auto* rect = m_reg->Get<Components::UIRectComponent>(e)) {
                if (rect->parentEntity == ent) {
                    childrenToDestroy.push_back(e);
                }
            }
        }

        for (Entity child: childrenToDestroy) {
            DestroyUIEntity(child);
        }

        m_reg->Destroy(ent);
    }

    // Sweep unvisited child entities under a parent node (or root)
    void SweepStaleChildren(Entity parentEntity) {
        Entity cacheEntity = (parentEntity != NullEntity) ? parentEntity : GetRootCacheEntity();
        if (cacheEntity == NullEntity || !m_reg->IsAlive(cacheEntity)) {
            return;
        }

        auto* cache = m_reg->Get<Components::UIChildCacheComponent>(cacheEntity);
        if (cache == nullptr) {
            return;
        }

        // A record is stale when its widget was not rebuilt this frame, OR when
        // the entity died outside the GUI (orphaned record pointing at a dead
        // entity, e.g. after a direct Registry::Destroy).
        ZHLN::Array<uint64_t> staleKeys;
        cache->children.ForEach([&](uint64_t key, const Components::UIChildCacheComponent::ChildRecord& rec) {
            if (!m_reg->IsAlive(rec.entity) || rec.lastVisitedFrame < m_currentFrame) {
                staleKeys.push_back(key);
            }
        });

        uint32_t destroyedSubtrees = 0;
        uint32_t deadRecords       = 0;
        for (uint64_t key: staleKeys) {
            bool wasAlive = false;
            if (const auto* rec = cache->children.Find(key)) {
                wasAlive = m_reg->IsAlive(rec->entity);
                if (wasAlive) {
                    // Use recursive destruction instead of plain registry destroy
                    DestroyUIEntity(rec->entity);
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
                ++deadRecords;
            }
        }

        if (destroyedSubtrees > 0) {
            ZHLN::Log(
                "[GUI::Context] Swept {} stale UI subtree(s) under parent ({}:{}): widget(s) were not rebuilt this frame. Remaining records: {}.",
                destroyedSubtrees, cacheEntity.index, cacheEntity.generation, cache->children.Size()
            );
        }
        if (deadRecords > 0) {
            ZHLN::Log<LogChannel::StdErr, LogLevel::Verbose>(
                "[GUI::Context] Purged {} orphaned UI cache record(s) under parent ({}:{}) pointing at entities destroyed outside the GUI.", deadRecords,
                cacheEntity.index, cacheEntity.generation
            );
        }
    }

    // --- WIDGET BUILDER METHODS ---

    template <typename Fn>
    Entity BeginPanel(std::string_view name, const PanelConfig& cfg, Fn&& content) {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        uint64_t key = HashCombine(parent.Pack(), HashStringView(name));

        Entity e = GetOrCreateEntity(key, [&]() {
            return m_reg->Create(
                Components::NameComponent {.name = String64(name)},
                Components::UIRectComponent {
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
                Components::UIPanelComponent {.color = cfg.color, .borderRadius = cfg.borderRadius, .edgeWidth = cfg.edgeWidth},
                Components::UIFlexComponent {
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

        m_reg->Patch<Components::UIRectComponent>(e, [&](auto& rect) {
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

        bool pushed = PushParent(e, depth);
        std::forward<Fn>(content)();

        SweepStaleChildren(e);
        PopParent(pushed);

        return e;
    }

    template <typename Fn>
    Entity BeginBox(std::string_view name, const BoxConfig& cfg, Fn&& content) {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        uint64_t key = HashCombine(parent.Pack(), HashStringView(name));

        Entity e = GetOrCreateEntity(key, [&]() {
            return m_reg->Create(
                Components::NameComponent {.name = String64(name)},
                Components::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = cfg.color, .edgeWidth = cfg.edgeWidth},
                Components::UIFlexComponent {
                    .direction     = cfg.direction,
                    .justify       = cfg.justify,
                    .alignItems    = cfg.alignItems,
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

        bool pushed = PushParent(e, depth);
        std::forward<Fn>(content)();

        SweepStaleChildren(e);
        PopParent(pushed);

        return e;
    }

    template <typename Fn>
    Entity BeginBox(const BoxConfig& cfg, Fn&& content) {
        std::array<char, 64> nameBuf {};
        std::string_view     autoName = FormatTo(nameBuf, "Box_D{}_{}", GetCurrentDepth(), m_autoIdCounter++);
        return BeginBox(autoName, cfg, std::forward<Fn>(content));
    }

    Entity Label(std::string_view text, const LabelConfig& cfg = {}) {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        std::array<char, 128> labelBuf {};
        std::string_view      labelName = FormatTo(labelBuf, "Lbl_{}", text);
        uint64_t              key       = HashCombine(parent.Pack(), HashStringView(labelName));

        TextureHandle fontHandle = TextureHandle::Invalid;
        auto          uiSettings = m_reg->GetEntitiesWith<Components::UISettingsComponent>();
        if (!uiSettings.empty()) {
            if (const auto* s = m_reg->Get<Components::UISettingsComponent>(uiSettings[0])) {
                fontHandle = s->fontAtlas.texture;
            }
        }

        Entity e = GetOrCreateEntity(key, [&]() {
            return m_reg->Create(
                Components::NameComponent {.name = String64(labelName)},
                Components::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = depth},
                Components::TextComponent {
                    .text          = String256(text),
                    .scale         = cfg.scale,
                    .color         = cfg.color,
                    .align         = cfg.align,
                    .verticalAlign = cfg.verticalAlign,
                    .fontIndex     = fontHandle
                }
            );
        });

        m_reg->Patch<Components::TextComponent>(e, [&](auto& textComp) {
            textComp.text.assign(text);
            textComp.color = cfg.color;
        });

        return e;
    }

    template <typename OnClickFn>
    Entity Button(std::string_view text, const ButtonConfig& cfg, OnClickFn&& onClick) {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        std::array<char, 128> btnBuf {};
        std::string_view      btnName = FormatTo(btnBuf, "Btn_{}", text);
        uint64_t              key     = HashCombine(parent.Pack(), HashStringView(btnName));

        TextureHandle fontHandle = TextureHandle::Invalid;
        auto          uiSettings = m_reg->GetEntitiesWith<Components::UISettingsComponent>();
        if (!uiSettings.empty()) {
            if (const auto* s = m_reg->Get<Components::UISettingsComponent>(uiSettings[0])) {
                fontHandle = s->fontAtlas.texture;
            }
        }

        Entity e = GetOrCreateEntity(key, [&]() {
            return m_reg->Create(
                Components::NameComponent {.name = String64(btnName)},
                Components::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = cfg.normalColor, .borderRadius = cfg.borderRadius}, Components::UIButtonComponent {},
                Components::UIStyleComponent {
                    .normalColor     = cfg.normalColor,
                    .hoverColor      = cfg.hoverColor,
                    .pressedColor    = cfg.pressedColor,
                    .textColorNormal = cfg.textColor,
                    .textColorHover  = {1.0f, 1.0f, 1.0f, 1.0f},
                    .transitionSpeed = 16.0f,
                    .hasTextColor    = true
                },
                Components::TextComponent {
                    .text          = String256(text),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = cfg.align,
                    .verticalAlign = cfg.verticalAlign,
                    .fontIndex     = fontHandle
                }
            );
        });

        m_reg->Patch<Components::UIButtonComponent>(e, [&](const auto& btn) {
            if (btn.Has(UIButton::Clicked)) {
                std::forward<OnClickFn>(onClick)();
            }
        });

        return e;
    }

    template <typename OnClickFn>
    Entity Button(std::string_view id, std::string_view text, const ButtonConfig& cfg, OnClickFn&& onClick) {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        uint64_t key = HashCombine(parent.Pack(), HashStringView(id));

        TextureHandle fontHandle = TextureHandle::Invalid;
        auto          uiSettings = m_reg->GetEntitiesWith<Components::UISettingsComponent>();
        if (!uiSettings.empty()) {
            if (const auto* s = m_reg->Get<Components::UISettingsComponent>(uiSettings[0])) {
                fontHandle = s->fontAtlas.texture;
            }
        }

        Entity e = GetOrCreateEntity(key, [&]() {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                Components::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = cfg.normalColor, .borderRadius = cfg.borderRadius}, Components::UIButtonComponent {},
                Components::UIStyleComponent {
                    .normalColor     = cfg.normalColor,
                    .hoverColor      = cfg.hoverColor,
                    .pressedColor    = cfg.pressedColor,
                    .textColorNormal = cfg.textColor,
                    .textColorHover  = {1.0f, 1.0f, 1.0f, 1.0f},
                    .transitionSpeed = 16.0f,
                    .hasTextColor    = true
                },
                Components::TextComponent {
                    .text          = String256(text),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = cfg.align,
                    .verticalAlign = cfg.verticalAlign,
                    .fontIndex     = fontHandle
                }
            );
        });

        // Update the text in the TextComponent of the existing entity dynamically
        m_reg->Patch<Components::TextComponent>(e, [&](auto& textComp) { textComp.text.assign(text); });

        m_reg->Patch<Components::UIButtonComponent>(e, [&](const auto& btn) {
            if (btn.Has(UIButton::Clicked)) {
                std::forward<OnClickFn>(onClick)();
            }
        });

        return e;
    }

    template <typename OnClickFn>
    Entity Button(std::string_view id, std::string_view text, OnClickFn&& onClick) {
        return Button(id, text, ButtonConfig {}, std::forward<OnClickFn>(onClick));
    }

    template <typename OnClickFn>
    Entity Button(std::string_view text, OnClickFn&& onClick) {
        return Button(text, ButtonConfig {}, std::forward<OnClickFn>(onClick));
    }

  private:
    static constexpr uint64_t HashCombine(uint64_t seed, uint64_t v) noexcept {
        return seed ^ (v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    }

    static constexpr uint64_t HashStringView(std::string_view str) noexcept {
        uint64_t hash = 0xcbf29ce484222325ull;
        for (char c: str) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    ECS::Registry*                              m_reg          = nullptr;
    uint64_t                                    m_currentFrame = 0;
    std::array<UIScopeNode, MAX_UI_STACK_DEPTH> m_stack {};
    uint32_t                                    m_stackTop            = 0;
    uint32_t                                    m_autoIdCounter       = 0;
    Entity                                      m_rootCacheEntity     = NullEntity;
    bool                                        m_stackOverflowLogged = false;
};

} // namespace ZHLN::GUI
