// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Components.hpp"
#include "Types.hpp"
#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <concepts>
#include <expected>
#include <span>
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
    HierarchyTooDeep ZHLN_ANNOTATION(ZHLN::Description<"UI hierarchy exceeded MAX_UI_STACK_DEPTH; overflowing widgets attached to the deepest live parent instead.">{}) = 1,
    EntityNotAlive ZHLN_ANNOTATION(ZHLN::Description<"Target UI entity is not alive (already destroyed or never existed).">{}),
    ParentNotAlive ZHLN_ANNOTATION(ZHLN::Description<"Parent entity for the GUI operation is not alive.">{}),
};

// Splitter/Columns orientation. Horizontal = side-by-side columns,
// Vertical = top/bottom rows.
enum class SplitDirection : uint8_t { Horizontal = 0, Vertical = 1 };

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

struct CheckboxConfig {
    float     boxSize      = 20.0f;
    float     height       = 28.0f;
    float     scale        = 0.85f;
    JPH::Vec4 boxColor     = {0.12f, 0.16f, 0.24f, 0.95f};
    JPH::Vec4 checkColor   = {0.40f, 0.72f, 1.00f, 1.0f};
    JPH::Vec4 hoverColor   = {0.20f, 0.28f, 0.42f, 1.0f};
    JPH::Vec4 textColor    = {0.90f, 0.95f, 1.0f, 1.0f};
    JPH::Vec4 borderRadius = {3.0f, 3.0f, 3.0f, 3.0f};
    float     gap          = 8.0f;
};

struct SliderConfig {
    float     width        = 0.0f; // 0 = fill
    float     height       = 28.0f;
    float     trackHeight  = 4.0f;
    float     knobSize     = 14.0f;
    float     scale        = 0.75f;
    JPH::Vec4 trackColor   = {0.12f, 0.16f, 0.24f, 0.95f};
    JPH::Vec4 fillColor    = {0.40f, 0.72f, 1.00f, 1.0f};
    JPH::Vec4 knobColor    = {0.80f, 0.90f, 1.00f, 1.0f};
    JPH::Vec4 hoverColor   = {0.60f, 0.82f, 1.00f, 1.0f};
    JPH::Vec4 textColor    = {0.90f, 0.95f, 1.0f, 1.0f};
    bool      showValue    = true;
};

struct TextInputConfig {
    float     width         = 0.0f; // 0 = fill
    float     height        = 32.0f;
    float     scale         = 0.85f;
    JPH::Vec4 bgColor       = {0.06f, 0.09f, 0.14f, 0.95f};
    JPH::Vec4 focusedColor  = {0.10f, 0.16f, 0.26f, 1.0f};
    JPH::Vec4 textColor     = {0.90f, 0.95f, 1.0f, 1.0f};
    JPH::Vec4 cursorColor   = {0.60f, 0.82f, 1.00f, 1.0f};
    JPH::Vec4 borderColor   = {0.26f, 0.38f, 0.58f, 1.0f};
    JPH::Vec4 borderRadius  = {3.0f, 3.0f, 3.0f, 3.0f};
    float     padding       = 8.0f;
};

struct DropdownConfig {
    float     width         = 0.0f;
    float     height        = 32.0f;
    float     itemHeight    = 28.0f;
    float     scale         = 0.85f;
    JPH::Vec4 bgColor       = {0.10f, 0.14f, 0.22f, 0.95f};
    JPH::Vec4 hoverColor    = {0.20f, 0.30f, 0.48f, 1.0f};
    JPH::Vec4 selectedColor = {0.26f, 0.46f, 0.78f, 1.0f};
    JPH::Vec4 textColor     = {0.90f, 0.95f, 1.0f, 1.0f};
    JPH::Vec4 arrowColor    = {0.70f, 0.82f, 1.00f, 1.0f};
    JPH::Vec4 borderRadius  = {3.0f, 3.0f, 3.0f, 3.0f};
    float     maxMenuHeight = 200.0f;
    float     padding       = 8.0f;
};

struct CollapsingHeaderConfig {
    float     height       = 28.0f;
    float     scale        = 0.85f;
    JPH::Vec4 bgColor      = {0.10f, 0.14f, 0.22f, 0.60f};
    JPH::Vec4 hoverColor   = {0.18f, 0.24f, 0.38f, 0.80f};
    JPH::Vec4 openColor    = {0.14f, 0.20f, 0.34f, 0.80f};
    JPH::Vec4 textColor    = {0.90f, 0.95f, 1.0f, 1.0f};
    JPH::Vec4 arrowColor   = {0.70f, 0.82f, 1.00f, 1.0f};
    float     padding      = 8.0f;
    float     indent       = 12.0f;
};

struct SplitterConfig {
    float     handleSize    = 6.0f;
    JPH::Vec4 handleColor   = {0.30f, 0.46f, 0.70f, 0.60f};
    JPH::Vec4 hoverColor    = {0.50f, 0.70f, 1.00f, 0.90f};
    float     minSize       = 64.0f; // Minimum panel size in pixels
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

    // -----------------------------------------------------------------
    // CHECKBOX  —  ui.Checkbox(label, bool& value, [cfg], [onChange])
    // -----------------------------------------------------------------
    // Value is authoritative in ECS; on first frame it is initialised from
    // the caller's bool&, on subsequent frames the (possibly user-edited)
    // ECS value is written back to the reference.
    auto Checkbox(std::string_view id, std::string_view label, bool& value, const CheckboxConfig& cfg) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                Components::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = {0.0f, 0.0f, 0.0f, 0.0f}},
                Components::UIFlexComponent {
                    .direction     = FlexDirection::Row,
                    .alignItems    = FlexAlign::Center,
                    .paddingLeft   = 4.0f,
                    .paddingRight  = 4.0f,
                    .gapX          = cfg.gap,
                    .gapY          = cfg.gap
                },
                Components::UIButtonComponent {},
                Components::UICheckboxComponent {.checked = value, .previousValue = value},
                // Inner checkbox box
                // Will be child-created below as a sibling to a label
                Components::MeshComponent {}
            );
        });

        // Sync: on first/respawn the entity has no children yet — create the
        // box+check+label inside. On subsequent frames ensure visual state
        // matches the component.
        auto* cb = m_reg->Get<Components::UICheckboxComponent>(e);

        // Accept external programmatic change when the caller flipped the
        // value without a click (unless a click is pending this frame).
        bool clickPending = false;
        m_reg->Patch<Components::UIButtonComponent>(e, [&](auto& btn) -> auto {
            if (btn.Has(UIButton::Clicked)) {
                cb->checked  = !cb->checked;
                clickPending = true;
                btn.Set(UIButton::Clicked, false);
            }
        });
        if (!clickPending && value != cb->checked) {
            cb->checked = value;
        }

        // Create / patch inner children (box + check mark + label)
        EnsureCheckboxChildren(e, label, cfg, fontHandle, cb->checked);

        // Apply hover visual to the checkbox box via UIPanel on inner box child
        PatchCheckboxVisuals(e, cfg, cb->checked, cb->hovered);

        // Push the (possibly new) ECS value back to the caller's reference
        value = cb->checked;
        cb->previousValue = cb->checked;

        return e;
    }

    auto Checkbox(std::string_view id, std::string_view label, bool& value) -> Entity {
        return Checkbox(id, label, value, CheckboxConfig {});
    }

    auto Checkbox(std::string_view label, bool& value, const CheckboxConfig& cfg = {}) -> Entity {
        return Checkbox(label, label, value, cfg);
    }

    template <typename OnChangeFn>
        requires std::invocable<OnChangeFn, bool>
    auto Checkbox(std::string_view id, std::string_view label, bool& value, OnChangeFn&& onChange) -> Entity {
        return Checkbox(id, label, value, CheckboxConfig {}, std::forward<OnChangeFn>(onChange));
    }

    template <typename OnChangeFn>
        requires std::invocable<OnChangeFn, bool>
    auto Checkbox(std::string_view id, std::string_view label, bool& value, const CheckboxConfig& cfg, OnChangeFn&& onChange) -> Entity {
        bool prev = value;
        Entity e  = Checkbox(id, label, value, cfg);
        if (value != prev) {
            std::forward<OnChangeFn>(onChange)(value);
        }
        return e;
    }

    template <typename OnChangeFn>
        requires std::invocable<OnChangeFn, bool>
    auto Checkbox(std::string_view label, bool& value, OnChangeFn&& onChange) -> Entity {
        return Checkbox(label, label, value, CheckboxConfig {}, std::forward<OnChangeFn>(onChange));
    }

    template <typename OnChangeFn>
        requires std::invocable<OnChangeFn, bool>
    auto Checkbox(std::string_view label, bool& value, const CheckboxConfig& cfg, OnChangeFn&& onChange) -> Entity {
        return Checkbox(label, label, value, cfg, std::forward<OnChangeFn>(onChange));
    }

    // -----------------------------------------------------------------
    // DRAGFLOAT / SLIDER  —  ui.DragFloat(label, float& value, min, max, step, [cfg], [onChange])
    // -----------------------------------------------------------------
    auto DragFloat(std::string_view id, std::string_view label, float& value, float minVal, float maxVal, float step, const SliderConfig& cfg) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                Components::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = {0.0f, 0.0f, 0.0f, 0.0f}},
                Components::UIFlexComponent {
                    .direction  = FlexDirection::Row,
                    .alignItems = FlexAlign::Center,
                    .gapX       = 8.0f,
                    .gapY       = 8.0f
                },
                Components::UIButtonComponent {},
                Components::UISliderComponent {
                    .value         = std::clamp(value, minVal, maxVal),
                    .minValue      = minVal,
                    .maxValue      = maxVal,
                    .step          = step,
                    .previousValue = value
                }
            );
        });

        auto* slider = m_reg->Get<Components::UISliderComponent>(e);

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

    auto DragFloat(std::string_view label, float& value, float minVal, float maxVal, float step = 0.0f, const SliderConfig& cfg = {}) -> Entity {
        return DragFloat(label, label, value, minVal, maxVal, step, cfg);
    }

    auto DragFloat(std::string_view id, std::string_view label, float& value, float minVal, float maxVal, float step) -> Entity {
        return DragFloat(id, label, value, minVal, maxVal, step, SliderConfig {});
    }

    auto DragFloat(std::string_view id, std::string_view label, float& value, float minVal, float maxVal) -> Entity {
        return DragFloat(id, label, value, minVal, maxVal, 0.0f, SliderConfig {});
    }

    auto Slider(std::string_view label, float& value, float minVal, float maxVal, float step = 0.0f, const SliderConfig& cfg = {}) -> Entity {
        // Slider is a cosmetic alias for DragFloat (click-to-jump + drag semantics)
        return DragFloat(label, label, value, minVal, maxVal, step, cfg);
    }

    template <typename OnChangeFn>
        requires std::invocable<OnChangeFn, float>
    auto DragFloat(std::string_view id, std::string_view label, float& value, float minVal, float maxVal, OnChangeFn&& onChange) -> Entity {
        return DragFloat(id, label, value, minVal, maxVal, 0.0f, SliderConfig {}, std::forward<OnChangeFn>(onChange));
    }

    template <typename OnChangeFn>
        requires std::invocable<OnChangeFn, float>
    auto DragFloat(std::string_view id, std::string_view label, float& value, float minVal, float maxVal, float step, const SliderConfig& cfg, OnChangeFn&& onChange) -> Entity {
        float prev = value;
        Entity e   = DragFloat(id, label, value, minVal, maxVal, step, cfg);
        if (value != prev) {
            std::forward<OnChangeFn>(onChange)(value);
        }
        return e;
    }

    template <typename OnChangeFn>
        requires std::invocable<OnChangeFn, float>
    auto DragFloat(std::string_view label, float& value, float minVal, float maxVal, OnChangeFn&& onChange) -> Entity {
        return DragFloat(label, label, value, minVal, maxVal, 0.0f, SliderConfig {}, std::forward<OnChangeFn>(onChange));
    }

    template <typename OnChangeFn>
        requires std::invocable<OnChangeFn, float>
    auto Slider(std::string_view label, float& value, float minVal, float maxVal, OnChangeFn&& onChange) -> Entity {
        return DragFloat(label, label, value, minVal, maxVal, 0.0f, SliderConfig {}, std::forward<OnChangeFn>(onChange));
    }

    // -----------------------------------------------------------------
    // TEXTINPUT  —  ui.TextInput(label, StringT& value, [cfg])
    //                  accepts FixedString<N>, String256, std::string
    // -----------------------------------------------------------------
    // Wraps the existing UITextInputComponent. The referenced string is
    // synced every frame from the component (which the engine mutates
    // directly via char/key callbacks), and the component's `edited` flag
    // is consumed+cleared here.
    template <typename StringT>
    auto TextInput(std::string_view id, std::string_view label, StringT& value, const TextInputConfig& cfg) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();

        // First, initialise the component text from the caller on create
        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            String256 initialText;
            initialText.assign(std::string_view(value));
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                Components::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = cfg.bgColor, .borderRadius = cfg.borderRadius, .edgeWidth = 1.0f},
                Components::UIFlexComponent {
                    .direction     = FlexDirection::Row,
                    .alignItems    = FlexAlign::Center,
                    .paddingLeft   = cfg.padding,
                    .paddingRight  = cfg.padding,
                    .gapX          = 6.0f
                },
                Components::UIButtonComponent {},
                Components::UITextInputComponent {.text = initialText, .cursorIndex = 0, .isFocused = false, .edited = false},
                Components::TextComponent {
                    .text          = initialText,
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = fontHandle
                }
            );
        });

        auto* input = m_reg->Get<Components::UITextInputComponent>(e);

        // Sync label if a non-empty label is passed (we render it as a
        // separate text entity inside a horizontal box — or omit for bare inputs)
        if (!label.empty() && label != id) {
            EnsureTextInputLabel(e, label, cfg, fontHandle);
        }

        std::string_view currentText = input->text;
        std::string_view externalText(value);

        // If the caller changed the string externally (and the user isn't
        // actively editing), reflect that change into the component.
        if (!input->isFocused && externalText != currentText) {
            input->text.assign(externalText);
            input->cursorIndex = static_cast<uint32_t>(externalText.size());
            currentText = std::string_view(input->text);
        }

        // Sync ECS text -> caller string
        value.assign(currentText);

        // Update the rendered TextComponent and panel visual (focused vs not)
        PatchTextInputVisuals(e, cfg, input->isFocused, fontHandle);

        // Keep cursor state sensible
        if (input->cursorIndex > currentText.size()) {
            input->cursorIndex = static_cast<uint32_t>(currentText.size());
        }

        return e;
    }

    template <typename StringT>
    auto TextInput(std::string_view label, StringT& value, const TextInputConfig& cfg = {}) -> Entity {
        return TextInput(label, std::string_view {}, value, cfg);
    }

    template <typename StringT>
    auto TextInput(std::string_view id, std::string_view label, StringT& value) -> Entity {
        return TextInput(id, label, value, TextInputConfig {});
    }

    // -----------------------------------------------------------------
    // DROPDOWN  —  ui.Dropdown(label, int& selectedIdx, span<string_view> options, [cfg], [onChange])
    // -----------------------------------------------------------------
    auto Dropdown(std::string_view id, std::string_view label, int& selectedIdx, std::span<const std::string_view> options, const DropdownConfig& cfg) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            auto ent = m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                Components::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = cfg.bgColor, .borderRadius = cfg.borderRadius},
                Components::UIFlexComponent {
                    .direction     = FlexDirection::Column,
                    .paddingLeft   = cfg.padding,
                    .paddingTop    = 4.0f,
                    .paddingRight  = cfg.padding,
                    .paddingBottom = 4.0f
                },
                Components::UIButtonComponent {},
                Components::UIDropdownComponent {
                    .selectedIdx = selectedIdx,
                    .previousIdx = selectedIdx,
                    .expanded    = false,
                    .hovered     = false,
                    .options     = {},
                }
            );
            return ent;
        });

        auto* dd = m_reg->Get<Components::UIDropdownComponent>(e);

        // Store options into the component so they survive between calls
        dd->options.clear();
        for (const auto& opt: options) {
            dd->options.push_back(String128(opt));
        }

        // Click on header toggles expansion; click on an item selects
        m_reg->Patch<Components::UIButtonComponent>(e, [&](auto& btn) -> auto {
            if (btn.Has(UIButton::Clicked)) {
                dd->expanded = !dd->expanded;
                btn.Set(UIButton::Clicked, false); // consume
            }
        });

        // Header shows the currently selected option (or placeholder label)
        std::string_view displayText = label;
        if (dd->selectedIdx >= 0 && static_cast<size_t>(dd->selectedIdx) < dd->options.size()) {
            displayText = std::string_view(dd->options[dd->selectedIdx]);
        }

        // Ensure header child (display text + arrow) exists
        EnsureDropdownHeader(e, displayText, cfg, fontHandle);

        // When expanded, build child items below
        if (dd->expanded) {
            auto scope = PushScopeForDropdownMenu(e, depth, cfg);
            // Current parent after PushScopeForDropdownMenu is the menu box,
            // so GetOrCreateEntity will parent items under it correctly.
            for (int i = 0; i < static_cast<int>(dd->options.size()); ++i) {
                std::array<char, 64> itemKeyBuf {};
                std::string_view     itemName = FormatTo(itemKeyBuf, "{}_opt{}", id, i);
                uint64_t itemKey = HashCombine(GetCurrentParent().Pack(), HashStringView(itemName));

                Entity menuParent = GetCurrentParent();
                uint32_t menuDepth = GetCurrentDepth();
                Entity itemEnt = GetOrCreateEntity(itemKey, [&]() -> Entity {
                    return m_reg->Create(
                        Components::NameComponent {.name = String64(itemName)},
                        Components::UIRectComponent {.parentEntity = menuParent, .height = cfg.itemHeight, .hierarchyDepth = menuDepth},
                        Components::UIPanelComponent {.color = (i == dd->selectedIdx) ? cfg.selectedColor : cfg.bgColor},
                        Components::UIFlexComponent {
                            .direction     = FlexDirection::Row,
                            .alignItems    = FlexAlign::Center,
                            .paddingLeft   = cfg.padding,
                            .paddingRight  = cfg.padding
                        },
                        Components::UIButtonComponent {},
                        Components::TextComponent {
                            .text          = String256(std::string_view(dd->options[i])),
                            .scale         = cfg.scale,
                            .color         = cfg.textColor,
                            .align         = TextAlignment::Left,
                            .verticalAlign = TextVerticalAlignment::Center,
                            .fontIndex     = fontHandle
                        }
                    );
                });

                // Ensure parent/depth stays correct if the menu box moved
                m_reg->Patch<Components::UIRectComponent>(itemEnt, [&](auto& r) -> auto {
                    r.parentEntity   = menuParent;
                    r.hierarchyDepth = menuDepth;
                    r.height         = cfg.itemHeight;
                });

                // Update text/color
                m_reg->Patch<Components::TextComponent>(itemEnt, [&](auto& tc) -> auto {
                    tc.text.assign(std::string_view(dd->options[i]));
                });
                m_reg->Patch<Components::UIPanelComponent>(itemEnt, [&](auto& pc) -> auto {
                    pc.color = (i == dd->selectedIdx) ? cfg.selectedColor : cfg.bgColor;
                });

                // Handle item click
                m_reg->Patch<Components::UIButtonComponent>(itemEnt, [&, i](auto& btn) -> auto {
                    if (btn.Has(UIButton::Clicked)) {
                        dd->selectedIdx = i;
                        dd->expanded    = false;
                        btn.Set(UIButton::Clicked, false);
                    }
                });
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

    auto Dropdown(std::string_view label, int& selectedIdx, std::span<const std::string_view> options, const DropdownConfig& cfg = {}) -> Entity {
        return Dropdown(label, label, selectedIdx, options, cfg);
    }

    auto Dropdown(std::string_view id, std::string_view label, int& selectedIdx, std::span<const std::string_view> options) -> Entity {
        return Dropdown(id, label, selectedIdx, options, DropdownConfig {});
    }

    template <typename OnChangeFn>
        requires std::invocable<OnChangeFn, int>
    auto Dropdown(std::string_view id, std::string_view label, int& selectedIdx, std::span<const std::string_view> options, OnChangeFn&& onChange) -> Entity {
        return Dropdown(id, label, selectedIdx, options, DropdownConfig {}, std::forward<OnChangeFn>(onChange));
    }

    template <typename OnChangeFn>
        requires std::invocable<OnChangeFn, int>
    auto Dropdown(std::string_view label, int& selectedIdx, std::span<const std::string_view> options, OnChangeFn&& onChange) -> Entity {
        return Dropdown(label, label, selectedIdx, options, DropdownConfig {}, std::forward<OnChangeFn>(onChange));
    }

    template <typename OnChangeFn>
        requires std::invocable<OnChangeFn, int>
    auto Dropdown(std::string_view id, std::string_view label, int& selectedIdx, std::span<const std::string_view> options, const DropdownConfig& cfg, OnChangeFn&& onChange) -> Entity {
        int prev = selectedIdx;
        Entity e = Dropdown(id, label, selectedIdx, options, cfg);
        if (selectedIdx != prev) {
            std::forward<OnChangeFn>(onChange)(selectedIdx);
        }
        return e;
    }

    template <typename OnChangeFn>
    auto Dropdown(std::string_view label, int& selectedIdx, std::span<const std::string_view> options, const DropdownConfig& cfg, OnChangeFn&& onChange) -> Entity {
        return Dropdown(label, label, selectedIdx, options, cfg, std::forward<OnChangeFn>(onChange));
    }

    // -----------------------------------------------------------------
    // COLLAPSINGHEADER  —  ui.CollapsingHeader(label, defaultOpen, fn, [cfg])
    // -----------------------------------------------------------------
    // Returns a UIScope that holds the content section open. When the
    // header is collapsed, the inner scope is NOT pushed and `fn` is not
    // invoked, which means child widgets will NOT be visited and will be
    // swept automatically when the header scope closes.
    template <typename Fn>
        requires std::invocable<Fn>
    [[nodiscard]] auto CollapsingHeader(std::string_view id, std::string_view label, bool defaultOpen, Fn&& content, const CollapsingHeaderConfig& cfg = {}) -> UIScope {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                Components::UIRectComponent {.parentEntity = parent, .height = 0.0f, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = defaultOpen ? cfg.openColor : cfg.bgColor},
                Components::UIFlexComponent {
                    .direction     = FlexDirection::Column,
                    .paddingLeft   = 0.0f,
                    .paddingTop    = 0.0f,
                    .paddingRight  = 0.0f,
                    .paddingBottom = 0.0f,
                    .gapX          = 0.0f,
                    .gapY          = 0.0f
                },
                Components::UICollapsingHeaderComponent {.isOpen = defaultOpen, .defaultOpen = defaultOpen}
            );
        });

        auto* hdr = m_reg->Get<Components::UICollapsingHeaderComponent>(e);

        // Ensure the clickable header button child exists
        EnsureCollapsingHeaderTitle(e, label, cfg, fontHandle, hdr->isOpen);

        // Make the title area clickable (we added UIButtonComponent on the _title child)
        Entity titleEnt = FindChildByKey(e, HashStringView("_title"));
        if (auto* titleBtn = (titleEnt != Entity::Null()) ? m_reg->Get<Components::UIButtonComponent>(titleEnt) : nullptr) {
            if (titleBtn->Has(UIButton::Clicked)) {
                hdr->isOpen = !hdr->isOpen;
                titleBtn->Set(UIButton::Clicked, false);
            }
        }

        // Update panel color to reflect state
        m_reg->Patch<Components::UIPanelComponent>(e, [&](auto& pc) -> auto {
            if (hdr->hovered) {
                pc.color = cfg.hoverColor;
            } else {
                pc.color = hdr->isOpen ? cfg.openColor : cfg.bgColor;
            }
        });

        // If open, push a content child box (indented) and invoke content in its scope
        if (hdr->isOpen) {
            UIScope scope = PushScope(e, depth);
            // Content box with indent
            std::array<char, 64> boxNameBuf {};
            std::string_view     contentBoxName = FormatTo(boxNameBuf, "{}_content", id);

            auto boxScope = Box(contentBoxName, BoxConfig {
                .width     = 0.0f,
                .height    = 0.0f,
                .color     = {0.0f, 0.0f, 0.0f, 0.0f},
                .direction = FlexDirection::Column,
                .gap       = 2.0f,
                .padding   = cfg.indent
            });
            std::forward<Fn>(content)();
            // boxScope dismissed here, then outer scope
            return scope;
        }

        // Collapsed: return a disengaged scope (IsPushed() == false) so the
        // caller's UIScope destructor is a no-op. We still need to mark the
        // header entity visited for this frame, which GetOrCreateEntity did.
        return {};
    }

    template <typename Fn>
        requires std::invocable<Fn>
    [[nodiscard]] auto CollapsingHeader(std::string_view label, bool defaultOpen, Fn&& content, const CollapsingHeaderConfig& cfg = {}) -> UIScope {
        return CollapsingHeader(label, label, defaultOpen, std::forward<Fn>(content), cfg);
    }

    // -----------------------------------------------------------------
    // SPLITTER / COLUMNS  —  ui.Columns(direction, ratio, leftFn, rightFn, [cfg])
    //                        ui.Splitter  alias with same signature
    // -----------------------------------------------------------------
    // Direction 0 = Horizontal (side-by-side columns), 1 = Vertical (top/bottom rows).
    // ratio controls the size of the first panel; the divider handle is
    // draggable to adjust the ratio at runtime.

    template <typename LeftFn, typename RightFn>
        requires std::invocable<LeftFn> && std::invocable<RightFn>
    [[nodiscard]] auto Columns(std::string_view id, SplitDirection direction, float& ratio, LeftFn&& leftFn, RightFn&& rightFn, const SplitterConfig& cfg = {}) -> UIScope {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        ratio = std::clamp(ratio, 0.05f, 0.95f);

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                Components::UIRectComponent {.parentEntity = parent, .height = 0.0f, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = {0.0f, 0.0f, 0.0f, 0.0f}},
                Components::UIFlexComponent {
                    .direction  = (direction == SplitDirection::Horizontal) ? FlexDirection::Row : FlexDirection::Column,
                    .alignItems = FlexAlign::Stretch,
                    .gapX       = 0.0f,
                    .gapY       = 0.0f
                },
                Components::UISplitterComponent {
                    .ratio         = ratio,
                    .previousRatio = ratio,
                    .direction     = (direction == SplitDirection::Horizontal)
                                        ? Components::UISplitterComponent::Horizontal
                                        : Components::UISplitterComponent::Vertical
                }
            );
        });

        auto* split = m_reg->Get<Components::UISplitterComponent>(e);
        split->previousRatio = split->ratio;
        ratio = split->ratio;

        // Push scope so children are under the splitter container
        UIScope scope = PushScope(e, depth);

        const bool horizontal = (direction == SplitDirection::Horizontal);

        // Left (or top) panel — flex-grow = ratio*1000, right = (1-ratio)*1000
        // so Yoga distributes space proportionally.
        {
            std::array<char, 64> leftNameBuf {};
            std::string_view     leftName = FormatTo(leftNameBuf, "{}_left", id);
            BoxConfig leftCfg;
            leftCfg.direction = FlexDirection::Column;
            leftCfg.color     = {0.0f, 0.0f, 0.0f, 0.0f};
            leftCfg.padding   = 0.0f;
            leftCfg.margin    = 0.0f;
            leftCfg.gap       = 0.0f;
            auto leftScope = Box(leftName, leftCfg);
            Entity leftEnt = leftScope.GetEntity();
            // Patch parent/depth/sizing each frame (needed for persistent entities)
            m_reg->Patch<Components::UIRectComponent>(leftEnt, [&](auto& lr) -> auto {
                lr.parentEntity   = e;
                lr.hierarchyDepth = depth + 1;
                lr.width          = horizontal ? 0.0f : 0.0f;
                lr.height         = horizontal ? 0.0f : 0.0f;
            });
            if (auto* lflex = m_reg->Get<Components::UIFlexComponent>(leftEnt)) {
                lflex->flexGrow   = ratio * 1000.0f;
                lflex->flexShrink = 1.0f;
                lflex->flexBasis  = 0.0f;
            }
            std::forward<LeftFn>(leftFn)();
        }

        // Divider handle (rendered as a thin draggable panel)
        {
            std::array<char, 64> handleNameBuf {};
            std::string_view     handleName = FormatTo(handleNameBuf, "{}_handle", id);

            Entity handleEnt = GetOrCreateChild(e, HashStringView(handleName), [&]() -> Entity {
                return m_reg->Create(
                    Components::NameComponent {.name = String64(handleName)},
                    Components::UIRectComponent {
                        .parentEntity   = e,
                        .width          = horizontal ? cfg.handleSize : 0.0f,
                        .height         = horizontal ? 0.0f : cfg.handleSize,
                        .hierarchyDepth = depth + 1
                    },
                    Components::UIPanelComponent {.color = cfg.handleColor},
                    Components::UIFlexComponent {
                        .flexGrow   = 0.0f,
                        .flexShrink = 0.0f,
                        .flexBasis  = static_cast<float>(cfg.handleSize)
                    },
                    Components::UIButtonComponent {},
                    Components::UIDragComponent {.targetEntity = e, .isDragging = false}
                );
            });
            // Patch the handle sizing each frame for current direction
            m_reg->Patch<Components::UIRectComponent>(handleEnt, [&](auto& hr) -> auto {
                hr.parentEntity   = e;
                hr.hierarchyDepth = depth + 1;
                if (horizontal) { hr.width = cfg.handleSize; hr.height = 0.0f; }
                else             { hr.width = 0.0f; hr.height = cfg.handleSize; }
            });
            m_reg->Patch<Components::UIFlexComponent>(handleEnt, [&](auto& hf) -> auto {
                hf.flexGrow   = 0.0f;
                hf.flexShrink = 0.0f;
                hf.flexBasis  = static_cast<float>(cfg.handleSize);
            });
            m_reg->Patch<Components::UIPanelComponent>(handleEnt, [&](auto& hp) -> auto {
                hp.color = split->hovered ? cfg.hoverColor : cfg.handleColor;
            });
        }

        // Right (or bottom) panel — fills remaining space via flexGrow
        {
            std::array<char, 64> rightNameBuf {};
            std::string_view     rightName = FormatTo(rightNameBuf, "{}_right", id);
            BoxConfig rightCfg;
            rightCfg.direction = FlexDirection::Column;
            rightCfg.color     = {0.0f, 0.0f, 0.0f, 0.0f};
            rightCfg.padding   = 0.0f;
            rightCfg.margin    = 0.0f;
            rightCfg.gap       = 0.0f;
            auto rightScope = Box(rightName, rightCfg);
            Entity rightEnt = rightScope.GetEntity();
            m_reg->Patch<Components::UIRectComponent>(rightEnt, [&](auto& rr) -> auto {
                rr.parentEntity   = e;
                rr.hierarchyDepth = depth + 1;
            });
            if (auto* rflex = m_reg->Get<Components::UIFlexComponent>(rightEnt)) {
                rflex->flexGrow   = (1.0f - ratio) * 1000.0f;
                rflex->flexShrink = 1.0f;
                rflex->flexBasis  = 0.0f;
            }
            std::forward<RightFn>(rightFn)();
        }

        return scope;
    }

    // ui.Splitter is the same primitive with a name that evokes the drag handle
    template <typename LeftFn, typename RightFn>
        requires std::invocable<LeftFn> && std::invocable<RightFn>
    [[nodiscard]] auto Splitter(std::string_view id, SplitDirection direction, float& ratio, LeftFn&& leftFn, RightFn&& rightFn, const SplitterConfig& cfg = {}) -> UIScope {
        return Columns(id, direction, ratio, std::forward<LeftFn>(leftFn), std::forward<RightFn>(rightFn), cfg);
    }

    template <typename LeftFn, typename RightFn>
        requires std::invocable<LeftFn> && std::invocable<RightFn>
    [[nodiscard]] auto Columns(SplitDirection direction, float& ratio, LeftFn&& leftFn, RightFn&& rightFn, const SplitterConfig& cfg = {}) -> UIScope {
        std::array<char, 64> nameBuf {};
        std::string_view     autoName = FormatTo(nameBuf, "Split_D{}_{}", GetCurrentDepth(), m_autoIdCounter++);
        return Columns(autoName, direction, ratio, std::forward<LeftFn>(leftFn), std::forward<RightFn>(rightFn), cfg);
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

    // --- PRIVATE HELPERS FOR COMPOUND WIDGETS ---

    // Look up a child entity in the parent's child cache by its hash key.
    [[nodiscard]] auto FindChildByKey(Entity parent, uint64_t childKey) const -> Entity {
        if (const auto* cache = m_reg->Get<Components::UIChildCacheComponent>(parent)) {
            if (const auto* rec = cache->children.Find(childKey)) {
                if (m_reg->IsAlive(rec->entity)) {
                    return rec->entity;
                }
            }
        }
        return Entity::Null();
    }

    // Get-or-create a child entity directly under a given parent, using the
    // parent's child cache (instead of the stack-based one). Useful for
    // building inner structure of compound widgets (checkbox box, slider track).
    template <typename CreateFn>
    auto GetOrCreateChild(Entity parent, uint64_t childKey, CreateFn&& createFn) -> Entity {
        auto* cache = m_reg->Get<Components::UIChildCacheComponent>(parent);
        if (cache == nullptr) {
            cache = &m_reg->Add<Components::UIChildCacheComponent>(parent);
        }

        if (const auto* rec = cache->children.Find(childKey)) {
            if (m_reg->IsAlive(rec->entity)) {
                rec->lastVisitedFrame = m_currentFrame;
                return rec->entity;
            }
        }
        Entity newEnt = createFn();
        cache->children.Insert(childKey, Components::UIChildCacheComponent::ChildRecord {.entity = newEnt, .lastVisitedFrame = m_currentFrame});
        return newEnt;
    }

    // Ensure the inner box + check mark + label entities for a Checkbox exist.
    // Layout: [box (containing check mark)] [label (flex-grow)]
    void EnsureCheckboxChildren(Entity cbEntity, std::string_view label, const CheckboxConfig& cfg, TextureHandle font, bool checked) {
        Entity   parent = cbEntity;
        uint32_t parentDepth = 0;
        if (const auto* r = m_reg->Get<Components::UIRectComponent>(cbEntity)) {
            parentDepth = r->hierarchyDepth;
        }

        // Checkbox box (flex column so the mark can be centered inside)
        Entity boxEnt = GetOrCreateChild(parent, HashStringView("_cb_box"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_cb_box")},
                Components::UIRectComponent {.parentEntity = parent, .width = cfg.boxSize, .height = cfg.boxSize, .hierarchyDepth = parentDepth + 1},
                Components::UIPanelComponent {.color = cfg.boxColor, .borderRadius = cfg.borderRadius, .edgeWidth = 1.0f},
                Components::UIFlexComponent {
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
                Components::UIRectComponent {.parentEntity = boxEnt, .width = cfg.boxSize - inset, .height = cfg.boxSize - inset, .hierarchyDepth = parentDepth + 2},
                Components::UIPanelComponent {.color = checked ? cfg.checkColor : JPH::Vec4 {0, 0, 0, 0}, .borderRadius = {2.0f, 2.0f, 2.0f, 2.0f}}
            );
        });
        m_reg->Patch<Components::UIPanelComponent>(markEnt, [&](auto& pc) -> auto {
            pc.color = checked ? cfg.checkColor : JPH::Vec4 {0, 0, 0, 0};
        });

        // Label
        Entity lblEnt = GetOrCreateChild(parent, HashStringView("_cb_label"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_cb_label")},
                Components::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                Components::TextComponent {
                    .text          = String256(label),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                },
                Components::UIFlexComponent {.flexGrow = 1.0f}
            );
        });
        m_reg->Patch<Components::TextComponent>(lblEnt, [&](auto& tc) -> auto {
            tc.text.assign(label);
            tc.color = cfg.textColor;
        });
    }

    void PatchCheckboxVisuals(Entity e, const CheckboxConfig& cfg, bool checked, bool hovered) {
        (void)checked;
        Entity boxEnt = FindChildByKey(e, HashStringView("_cb_box"));
        if (boxEnt != Entity::Null()) {
            if (auto* panel = m_reg->Get<Components::UIPanelComponent>(boxEnt)) {
                panel->color        = hovered ? cfg.hoverColor : cfg.boxColor;
                panel->edgeWidth    = 1.0f;
                panel->borderRadius = cfg.borderRadius;
            }
        }
    }

    void EnsureSliderChildren(Entity sliderEntity, std::string_view label, const SliderConfig& cfg, TextureHandle font, float value, float minVal, float maxVal) {
        Entity   parent = sliderEntity;
        uint32_t parentDepth = 0;
        if (const auto* r = m_reg->Get<Components::UIRectComponent>(sliderEntity)) {
            parentDepth = r->hierarchyDepth;
        }

        // Label
        if (!label.empty()) {
            Entity lblEnt = GetOrCreateChild(parent, HashStringView("_sl_label"), [&]() -> Entity {
                return m_reg->Create(
                    Components::NameComponent {.name = String64("_sl_label")},
                    Components::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                    Components::TextComponent {
                        .text          = String256(label),
                        .scale         = cfg.scale,
                        .color         = cfg.textColor,
                        .align         = TextAlignment::Left,
                        .verticalAlign = TextVerticalAlignment::Center,
                        .fontIndex     = font
                    }
                );
            });
            m_reg->Patch<Components::TextComponent>(lblEnt, [&](auto& tc) -> auto { tc.text.assign(label); });
        }

        // Track (container for filled region and knob)
        Entity trackEnt = GetOrCreateChild(parent, HashStringView("_sl_track"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_sl_track")},
                Components::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                Components::UIPanelComponent {.color = cfg.trackColor},
                Components::UIFlexComponent {
                    .direction     = FlexDirection::Row,
                    .alignItems    = FlexAlign::Center,
                    .flexGrow      = 1.0f,
                    .flexShrink    = 1.0f,
                    .flexBasis     = -1.0f,
                    .paddingLeft   = 4.0f,
                    .paddingTop    = 0.0f,
                    .paddingRight  = 4.0f,
                    .paddingBottom = 0.0f
                },
                Components::UIButtonComponent {}
            );
        });

        // Value text
        if (cfg.showValue) {
            std::array<char, 32> valBuf {};
            std::string_view     valStr = FormatTo(valBuf, "{:.3g}", value);
            Entity valEnt = GetOrCreateChild(parent, HashStringView("_sl_value"), [&]() -> Entity {
                return m_reg->Create(
                    Components::NameComponent {.name = String64("_sl_value")},
                    Components::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                    Components::TextComponent {
                        .text          = String256(valStr),
                        .scale         = cfg.scale,
                        .color         = cfg.textColor,
                        .align         = TextAlignment::Right,
                        .verticalAlign = TextVerticalAlignment::Center,
                        .fontIndex     = font
                    }
                );
            });
            m_reg->Patch<Components::TextComponent>(valEnt, [&](auto& tc) -> auto {
                tc.text.assign(valStr);
            });
        }

        // Knob visual child on track
        float range = maxVal - minVal;
        float t     = (range > 0.0f) ? ((value - minVal) / range) : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        (void)t; // actual sizing done at render time; knob exists as a marker
        Entity knobEnt = GetOrCreateChild(trackEnt, HashStringView("_sl_knob"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_sl_knob")},
                Components::UIRectComponent {.parentEntity = trackEnt, .width = cfg.knobSize, .height = cfg.knobSize, .hierarchyDepth = parentDepth + 2},
                Components::UIPanelComponent {.color = cfg.knobColor, .borderRadius = {cfg.knobSize/2, cfg.knobSize/2, cfg.knobSize/2, cfg.knobSize/2}},
                Components::UIButtonComponent {},
                Components::UIDragComponent {.targetEntity = sliderEntity, .isDragging = false}
            );
        });
        m_reg->Patch<Components::UIPanelComponent>(knobEnt, [&](auto& pc) -> auto {
            auto* s = m_reg->Get<Components::UISliderComponent>(sliderEntity);
            pc.color = (s != nullptr && s->hovered) ? cfg.hoverColor : cfg.knobColor;
        });
    }

    void EnsureTextInputLabel(Entity inputEnt, std::string_view label, const TextInputConfig& cfg, TextureHandle font) {
        uint32_t parentDepth = 0;
        if (const auto* r = m_reg->Get<Components::UIRectComponent>(inputEnt)) {
            parentDepth = r->hierarchyDepth;
        }
        Entity lblEnt = GetOrCreateChild(inputEnt, HashStringView("_ti_label"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ti_label")},
                Components::UIRectComponent {.parentEntity = inputEnt, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                Components::TextComponent {
                    .text          = String256(label),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                }
            );
        });
        m_reg->Patch<Components::TextComponent>(lblEnt, [&](auto& tc) -> auto { tc.text.assign(label); });
    }

    void PatchTextInputVisuals(Entity e, const TextInputConfig& cfg, bool focused, TextureHandle font) {
        m_reg->Patch<Components::UIPanelComponent>(e, [&](auto& pc) -> auto {
            pc.color        = focused ? cfg.focusedColor : cfg.bgColor;
            pc.edgeWidth    = 1.0f;
            pc.borderRadius = cfg.borderRadius;
        });
        // Sync text component from input component text
        if (auto* input = m_reg->Get<Components::UITextInputComponent>(e)) {
            // Ensure we have a TextComponent showing the current text
            if (auto* tc = m_reg->Get<Components::TextComponent>(e)) {
                tc->text      = input->text;
                tc->color     = cfg.textColor;
                tc->scale     = cfg.scale;
                tc->fontIndex = font;
            }
        }
    }

    void EnsureDropdownHeader(Entity ddEnt, std::string_view displayText, const DropdownConfig& cfg, TextureHandle font) {
        Entity   parent = ddEnt;
        uint32_t parentDepth = 0;
        if (const auto* r = m_reg->Get<Components::UIRectComponent>(ddEnt)) {
            parentDepth = r->hierarchyDepth;
        }

        Entity txtEnt = GetOrCreateChild(parent, HashStringView("_dd_text"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_dd_text")},
                Components::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                Components::TextComponent {
                    .text          = String256(displayText),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                },
                Components::UIFlexComponent {.flexGrow = 1.0f}
            );
        });
        m_reg->Patch<Components::TextComponent>(txtEnt, [&](auto& tc) -> auto { tc.text.assign(displayText); });

        // Arrow
        Entity arrowEnt = GetOrCreateChild(parent, HashStringView("_dd_arrow"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_dd_arrow")},
                Components::UIRectComponent {.parentEntity = parent, .width = 16.0f, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                Components::TextComponent {
                    .text          = String256("v"),
                    .scale         = cfg.scale,
                    .color         = cfg.arrowColor,
                    .align         = TextAlignment::Right,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                }
            );
        });
        m_reg->Patch<Components::TextComponent>(arrowEnt, [&](auto& tc) -> auto {
            auto* dd = m_reg->Get<Components::UIDropdownComponent>(ddEnt);
            tc.text.assign((dd != nullptr && dd->expanded) ? "^" : "v");
        });
    }

    // Helper that pushes an entity on the stack without creating a UIScope the
    // caller must hold — used by Dropdown so the child menu items are parented
    // under the dropdown while we iterate options. The push is balanced by
    // InternalPop via a local UIScope we return.
    [[nodiscard]] auto PushScopeForDropdownMenu(Entity ddEnt, uint32_t depth, const DropdownConfig& cfg) -> UIScope {
        // Push dropdown entity
        UIScope outer = PushScope(ddEnt, depth);
        // Create a container box for the menu items (appears below the header)
        std::array<char, 64> menuNameBuf {};
        std::string_view     menuName = FormatTo(menuNameBuf, "_dd_menu_{}", ddEnt.index);
        Entity menuBox = GetOrCreateChild(ddEnt, HashStringView(menuName), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(menuName)},
                Components::UIRectComponent {.parentEntity = ddEnt, .height = 0.0f, .hierarchyDepth = depth + 1},
                Components::UIPanelComponent {.color = {0.07f, 0.10f, 0.16f, 0.98f}, .borderRadius = cfg.borderRadius, .edgeWidth = 1.0f},
                Components::UIFlexComponent {
                    .direction     = FlexDirection::Column,
                    .paddingLeft   = 0.0f,
                    .paddingTop    = 4.0f,
                    .paddingRight  = 0.0f,
                    .paddingBottom = 4.0f,
                    .gapX          = 0.0f,
                    .gapY          = 2.0f
                }
            );
        });
        // Dismiss the outer scope (pushed ddEnt), then push menuBox so child
        // items are parented to the menu container rather than the header.
        outer.Dismiss();
        return PushScope(menuBox, depth + 1);
    }

    void EnsureCollapsingHeaderTitle(Entity hdrEntity, std::string_view label, const CollapsingHeaderConfig& cfg, TextureHandle font, bool isOpen) {
        Entity   parent = hdrEntity;
        uint32_t parentDepth = 0;
        if (const auto* r = m_reg->Get<Components::UIRectComponent>(hdrEntity)) {
            parentDepth = r->hierarchyDepth;
        }

        Entity titleEnt = GetOrCreateChild(parent, HashStringView("_title"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_title")},
                Components::UIRectComponent {.parentEntity = parent, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                Components::UIPanelComponent {.color = {0, 0, 0, 0}},
                Components::UIFlexComponent {
                    .direction     = FlexDirection::Row,
                    .alignItems    = FlexAlign::Center,
                    .paddingLeft   = cfg.padding,
                    .paddingRight  = cfg.padding
                },
                Components::UIButtonComponent {}
            );
        });

        // Arrow + label inside the title
        Entity arrowEnt = GetOrCreateChild(titleEnt, HashStringView("_ch_arrow"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ch_arrow")},
                Components::UIRectComponent {.parentEntity = titleEnt, .width = 16.0f, .height = cfg.height, .hierarchyDepth = parentDepth + 2},
                Components::TextComponent {
                    .text          = String256(isOpen ? "v" : ">"),
                    .scale         = cfg.scale,
                    .color         = cfg.arrowColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                }
            );
        });
        m_reg->Patch<Components::TextComponent>(arrowEnt, [&](auto& tc) -> auto {
            tc.text.assign(isOpen ? "v" : ">");
            tc.color = cfg.arrowColor;
        });

        Entity lblEnt = GetOrCreateChild(titleEnt, HashStringView("_ch_label"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ch_label")},
                Components::UIRectComponent {.parentEntity = titleEnt, .height = cfg.height, .hierarchyDepth = parentDepth + 2},
                Components::TextComponent {
                    .text          = String256(label),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = font
                },
                Components::UIFlexComponent {.flexGrow = 1.0f}
            );
        });
        m_reg->Patch<Components::TextComponent>(lblEnt, [&](auto& tc) -> auto {
            tc.text.assign(label);
            tc.color = cfg.textColor;
        });
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
