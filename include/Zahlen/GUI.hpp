// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Components.hpp"
#include "Types.hpp"
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <concepts>
#include <expected>
#include <string_view>
#include <utility>

namespace ZHLN::GUI {

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

auto MeasureTextBounds(const FontAtlas& font, std::string_view text, float scale) noexcept -> TextBounds;

auto AppendTextVertices(
    VertexPosition*    outPos,
    VertexAttributes*  outAttr,
    const FontAtlas&   font,
    const std::string& text,
    float              x,
    float              y,
    float              scale,
    const JPH::Vec4&   color
) -> uint32_t;

auto AppendPanelVertices(VertexPosition* outPos, VertexAttributes* outAttr, const Components::UIRectComponent& rect, const Components::UIPanelComponent& panel)
    -> uint32_t;

// ============================================================================
// FIBER-SAFE & ZERO-ALLOCATION GUI CONTEXT — HYBRID RAII + CLOSURE
// ============================================================================
//
// Two policies, applied by intent:
//
//  * TREE BUILDING (Panel/Box/Label/Button) is fault-tolerant. A UI must keep
//    rendering even when a subtree overflows the stack or a parent vanished.
//    Builders therefore never return errors; the first structural problem is
//    latched as a sticky status, readable through Context::Status(). Forcing
//    std::expected on every Label would be unusable boilerplate.
//
//  * STRUCTURAL CLEANUP: DestroyUIEntity is monadic — it returns
//    std::expected<void, Error>, which is [[nodiscard]] since C++26, so
//    every failure point MUST be handled by the caller — no '(void)' casts,
//    no 'auto _ =', no [[maybe_unused]] escape hatches.
//    SweepStaleChildren is best-effort GC and returns void: like the
//    builders, a dead parent or an entity that refuses to die latches a
//    typed error into Status() instead of forcing monadic plumbing through
//    every destructor and pop path.
//
// Scope management is RAII-first: Panel()/Box() return a [[nodiscard]]
// UIScope whose lifetime is the push/pop pair (the pop also sweeps the
// container's stale children). The three-argument closure overloads are thin
// sugar over that guard. Context itself auto-sweeps the root cache from its
// destructor, so per-frame "end of frame" sweeps at call sites are gone.
//
// GUIError deliberately has NO 'Success' enumerator: success is not an error.
// Codes start at 1 because Error::operator bool treats only non-zero values
// as active errors.

constexpr size_t MAX_UI_STACK_DEPTH = 32;

enum class GUIError : uint8_t {
    HierarchyTooDeep[
        [= ZHLN::Reflect::Description<"UI hierarchy exceeded MAX_UI_STACK_DEPTH; overflowing widgets attached to the deepest live parent instead.">{}]] = 1,
    EntityNotAlive[[= ZHLN::Reflect::Description<"Target UI entity is not alive (already destroyed or never existed).">{}]],
    ParentNotAlive[[= ZHLN::Reflect::Description<"Parent entity for the GUI operation is not alive.">{}]],
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

class Context;

// --- RAII SCOPE GUARD ---
//
// The primary engine of scope management: constructing one pushes the widget
// entity onto the context stack; destruction pops (and runs the container's
// stale-child sweep). [[nodiscard]] by class so a discarded temporary guard —
// which would pop the scope at the end of the full expression — is a compile
// error. Move-only; moving transfers the pop duty exactly once.

class [[nodiscard]] UIScope {
  public:
    UIScope() noexcept = default;
    ~UIScope() {
        Dismiss();
    }

    UIScope(const UIScope&)                    = delete;
    auto operator=(const UIScope&) -> UIScope& = delete;

    UIScope(UIScope&& other) noexcept: m_ctx(other.m_ctx), m_entity(other.m_entity), m_pushed(other.m_pushed) {
        other.m_ctx    = nullptr;
        other.m_entity = Entity::Null();
        other.m_pushed = false;
    }

    auto operator=(UIScope&& other) noexcept -> UIScope& {
        if (this != &other) {
            Dismiss(); // a replaced guard still pops its own scope first
            m_ctx          = other.m_ctx;
            m_entity       = other.m_entity;
            m_pushed       = other.m_pushed;
            other.m_ctx    = nullptr;
            other.m_entity = Entity::Null();
            other.m_pushed = false;
        }
        return *this;
    }

    [[nodiscard]] auto GetEntity() const noexcept -> Entity {
        return m_entity;
    }

    // False when the hierarchy overflowed MAX_UI_STACK_DEPTH (the error is
    // latched in the owning context) or the guard is disengaged/moved-from.
    [[nodiscard]] auto IsPushed() const noexcept -> bool {
        return m_pushed;
    }

    // Early pop; also the destructor's workhorse. Defined after Context.
    void Dismiss() noexcept;

  private:
    friend class Context;

    UIScope(Context* ctx, Entity entity, bool pushed) noexcept: m_ctx(ctx), m_entity(entity), m_pushed(pushed) {
    }

    Context* m_ctx    = nullptr;
    Entity   m_entity = Entity::Null();
    bool     m_pushed = false;
};

// --- STACK-ALLOCATED FIBER-SAFE CONTEXT ---

class Context {
  public:
    explicit Context(ECS::Registry& reg, uint64_t currentFrame = 0) noexcept: m_reg(&reg), m_currentFrame(currentFrame) {
    }

    // Frame teardown: sweeping the root cache here makes a manual
    // SweepStaleChildren(Entity::Null()) at every call site redundant. The sweep
    // is idempotent — records visited this frame survive, re-sweeps are
    // no-ops. It cannot throw out of here: failures latch into Status()
    // (queryable while the context lives), never abort a destructor.
    ~Context() noexcept {
        SweepStaleChildren(Entity::Null());
    }

    Context(const Context&)                    = delete;
    auto operator=(const Context&) -> Context& = delete;

    [[nodiscard]] auto GetRegistry() const noexcept -> ECS::Registry& {
        return *m_reg;
    }

    [[nodiscard]] auto GetCurrentParent() const noexcept -> Entity {
        return (m_stackTop > 0) ? m_stack[m_stackTop - 1].entity : Entity::Null();
    }

    [[nodiscard]] auto GetCurrentDepth() const noexcept -> uint32_t {
        return (m_stackTop > 0) ? m_stack[m_stackTop - 1].depth + 1 : 1;
    }

    // First structural error this context ran into while building (e.g. the
    // hierarchy grew past MAX_UI_STACK_DEPTH). Builders degrade gracefully and
    // keep rendering instead of aborting, so read Status() when a tree looks
    // wrong; an engaged expected means clean, never an error code.
    [[nodiscard]] auto Status() const noexcept -> std::expected<void, Error> {
        if (m_error) {
            return std::unexpected(m_error);
        }
        return {};
    }

    void ClearStatus() noexcept {
        m_error = Error {};
    }

    // --- STRUCTURAL CLEANUP ---
    // DestroyUIEntity is monadic — the caller MUST handle its result.
    // SweepStaleChildren is best-effort GC: failures latch into Status().

    // Destroys a UI entity and its whole subtree, recursing through the
    // entity's OWN child-cache records — O(K) in the subtree size instead of
    // a registry-wide scan per level. A dead input entity fails with
    // GUIError::EntityNotAlive; success is engaged-void and silent
    // (verbose-level trace aside).
    [[nodiscard]] auto DestroyUIEntity(Entity ent) noexcept -> std::expected<void, Error> {
        if (!m_reg->IsAlive(ent)) {
            return std::unexpected(Error(GUIError::EntityNotAlive));
        }

        ZHLN::Log<LogChannel::StdErr, LogLevel::Verbose>(
            "[GUI::Context] DestroyUIEntity: destroying UI entity ({}:{}) and its subtree.", ent.index, ent.generation
        );

        // First failure wins and propagates; a liveness-verified child that
        // fails to die is a real failure, not a discarded temporary.
        std::expected<void, Error> firstFailure {};
        if (const auto* cache = m_reg->Get<Components::UIChildCacheComponent>(ent)) {
            cache->children.ForEach([&](uint64_t, const Components::UIChildCacheComponent::ChildRecord& rec) -> void {
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

    // Sweeps child widgets that were not rebuilt this frame (or whose entities
    // died outside the GUI) from under a parent node — or from the root cache
    // when parentEntity is Entity::Null(). Fault-tolerant like the builders: a
    // dead parent, or an entity that refuses to die, latches a typed error
    // into Status() and the sweep stops there. Success is silent
    // (verbose-level traces aside). Idempotent: a sweep right after a sweep
    // is a no-op.
    void SweepStaleChildren(Entity parentEntity) {
        Entity cacheEntity = (parentEntity != Entity::Null()) ? parentEntity : GetRootCacheEntity();
        if (cacheEntity == Entity::Null() || !m_reg->IsAlive(cacheEntity)) {
            RecordError(Error(GUIError::ParentNotAlive));
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
        cache->children.ForEach([&](uint64_t key, const Components::UIChildCacheComponent::ChildRecord& rec) -> void {
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

    // O(1) Hash-based Entity Lookup with Mark-and-Sweep GC
    template <typename CreateFn>
    auto GetOrCreateEntity(uint64_t widgetKey, CreateFn&& createFn) -> Entity {
        Entity parent      = GetCurrentParent();
        Entity cacheEntity = (parent != Entity::Null()) ? parent : GetRootCacheEntity();

        auto* cache = m_reg->Get<Components::UIChildCacheComponent>(cacheEntity);
        if (cache == nullptr) {
            cache = &m_reg->Add<Components::UIChildCacheComponent>(cacheEntity);
        }

        // 1. O(1) Lookup in cache. A record whose entity was destroyed outside
        // the GUI (plain Registry::Destroy) is transparently respawned below;
        // the insert overwrites the dead record and recovery stays silent.
        if (const auto* record = cache->children.Find(widgetKey)) {
            if (m_reg->IsAlive(record->entity)) {
                record->lastVisitedFrame = m_currentFrame;
                return record->entity;
            }
        }

        // 2. Not found -> Spawn new entity
        Entity newEntity = createFn();
        cache->children.Insert(widgetKey, Components::UIChildCacheComponent::ChildRecord {.entity = newEntity, .lastVisitedFrame = m_currentFrame});

        return newEntity;
    }

    // --- WIDGET BUILDERS (fault-tolerant; problems latch into Status()) ---

    // RAII: the returned guard holds the scope open. Its destructor pops and
    // sweeps the panel's stale children.
    [[nodiscard]] auto Panel(std::string_view name, const PanelConfig& cfg = {}) -> UIScope {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        uint64_t key = HashCombine(parent.Pack(), HashStringView(name));

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
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

        m_reg->Patch<Components::UIRectComponent>(e, [&](auto& rect) -> auto {
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

    // Closure sugar: three lines over the guard. The pop + container sweep run
    // on scope exit no matter what the callback does (early return, continue).
    template <typename Fn>
        requires std::invocable<Fn>
    auto Panel(std::string_view name, const PanelConfig& cfg, Fn&& content) -> Entity {
        auto scope = Panel(name, cfg);
        std::forward<Fn>(content)();
        return scope.GetEntity();
    }

    [[nodiscard]] auto Box(std::string_view name, const BoxConfig& cfg = {}) -> UIScope {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        uint64_t key = HashCombine(parent.Pack(), HashStringView(name));

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
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

        return PushScope(e, depth);
    }

    template <typename Fn>
        requires std::invocable<Fn>
    auto Box(std::string_view name, const BoxConfig& cfg, Fn&& content) -> Entity {
        auto scope = Box(name, cfg);
        std::forward<Fn>(content)();
        return scope.GetEntity();
    }

    // Anonymous box: stable per-frame auto-ID keeps the closure form a one-liner.
    template <typename Fn>
        requires std::invocable<Fn>
    auto Box(const BoxConfig& cfg, Fn&& content) -> Entity {
        std::array<char, 64>   nameBuf {};
        const std::string_view autoName = FormatTo(nameBuf, "Box_D{}_{}", GetCurrentDepth(), m_autoIdCounter++);
        return Box(autoName, cfg, std::forward<Fn>(content));
    }

    auto Label(std::string_view text, const LabelConfig& cfg = {}) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        std::array<char, 128> labelBuf {};
        std::string_view      labelName = FormatTo(labelBuf, "Lbl_{}", text);
        uint64_t              key       = HashCombine(parent.Pack(), HashStringView(labelName));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
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

        m_reg->Patch<Components::TextComponent>(e, [&](auto& textComp) -> auto {
            textComp.text.assign(text);
            textComp.color = cfg.color;
        });

        return e;
    }

    // id + text stays the only overload that hits the registry; the shorter
    // forms all delegate to it so behavior (and cache keys) can never drift.
    template <typename OnClickFn>
    auto Button(std::string_view id, std::string_view text, const ButtonConfig& cfg, OnClickFn&& onClick) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();

        uint64_t key = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity e = GetOrCreateEntity(key, [&]() -> auto {
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
        m_reg->Patch<Components::TextComponent>(e, [&](auto& textComp) -> auto { textComp.text.assign(text); });

        m_reg->Patch<Components::UIButtonComponent>(e, [&](const auto& btn) -> auto {
            if (btn.Has(UIButton::Clicked)) {
                std::forward<OnClickFn>(onClick)();
            }
        });

        return e;
    }

    template <typename OnClickFn>
    auto Button(std::string_view id, std::string_view text, OnClickFn&& onClick) -> Entity {
        return Button(id, text, ButtonConfig {}, std::forward<OnClickFn>(onClick));
    }

    template <typename OnClickFn>
    auto Button(std::string_view text, const ButtonConfig& cfg, OnClickFn&& onClick) -> Entity {
        return Button(text, text, cfg, std::forward<OnClickFn>(onClick));
    }

    template <typename OnClickFn>
    auto Button(std::string_view text, OnClickFn&& onClick) -> Entity {
        return Button(text, text, ButtonConfig {}, std::forward<OnClickFn>(onClick));
    }

  private:
    friend class UIScope;

    struct UIScopeNode {
        Entity   entity = Entity::Null();
        uint32_t depth  = 1;
    };

    // Resolves or creates the root cache entity (UISettingsComponent).
    // Private by design: the cache root is an implementation detail of the GC;
    // tests can derive it via GetEntitiesWith<UISettingsComponent>().
    auto GetRootCacheEntity() -> Entity {
        if (m_rootCacheEntity != Entity::Null() && m_reg->IsAlive(m_rootCacheEntity)) {
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

    // Push/pop are the scope guard's private plumbing — never public API.
    // Exposing them let callers push without sweeping or pop twice; UIScope
    // keeps the pair balanced by construction.
    [[nodiscard]] auto InternalPush(Entity entity, uint32_t depth) noexcept -> bool {
        if (m_stackTop < MAX_UI_STACK_DEPTH) {
            m_stack[m_stackTop++] = {.entity = entity, .depth = depth};
            return true;
        }
        RecordError(Error(GUIError::HierarchyTooDeep)); // surfaced through Status()
        return false;
    }

    // The pop owns the container's stale-child sweep: closing a scope is the
    // exact moment its final child list for the frame is known.
    void InternalPop(bool wasPushed, Entity entity) noexcept {
        if (wasPushed && m_stackTop > 0) {
            --m_stackTop;
        }
        SweepStaleChildren(entity); // a failure latches into Status()
    }

    auto PushScope(Entity e, uint32_t depth) noexcept -> UIScope {
        return {this, e, InternalPush(e, depth)};
    }

    [[nodiscard]] auto ResolveFontTexture() const noexcept -> TextureHandle {
        const auto uiSettings = m_reg->GetEntitiesWith<Components::UISettingsComponent>();
        if (!uiSettings.empty()) {
            if (const auto* s = m_reg->Get<Components::UISettingsComponent>(uiSettings[0])) {
                return s->fontAtlas.texture;
            }
        }
        return TextureHandle::Invalid;
    }

    static constexpr auto HashCombine(uint64_t seed, uint64_t v) noexcept -> uint64_t {
        return seed ^ (v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    }

    static constexpr auto HashStringView(std::string_view str) noexcept -> uint64_t {
        uint64_t hash = 0xcbf29ce484222325ull;
        for (char c: str) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    // First-error-wins: the status surfaces through Status() instead of logs.
    void RecordError(Error err) noexcept {
        if (!m_error) {
            m_error = err;
        }
    }

    ECS::Registry*                              m_reg          = nullptr;
    uint64_t                                    m_currentFrame = 0;
    std::array<UIScopeNode, MAX_UI_STACK_DEPTH> m_stack {};
    uint32_t                                    m_stackTop        = 0;
    uint32_t                                    m_autoIdCounter   = 0;
    Entity                                      m_rootCacheEntity = Entity::Null();
    Error                                       m_error;
};

inline void UIScope::Dismiss() noexcept {
    if (m_ctx != nullptr) {
        m_ctx->InternalPop(m_pushed, m_entity);
        m_ctx    = nullptr;
        m_entity = Entity::Null();
        m_pushed = false;
    }
}

} // namespace ZHLN::GUI
