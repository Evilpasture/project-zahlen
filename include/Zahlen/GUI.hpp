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
#include <type_traits>
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
// TWO MUTUALLY EXCLUSIVE PARADIGMS — NEVER MIX THEM PER WIDGET
// ------------------------------------------------------------
// Every container widget offers exactly two forms, and a single widget must
// be used in exactly ONE of them per call site:
//
//   * RAII-guard form:  `auto scope = ui.Panel("p");` ... `; } // ~UIScope pops`
//     The guard owns the push/pop pair. Capture it; its lifetime IS the scope.
//     Used in a manual C++ scope block.
//
//   * Closure form:     `ui.Panel("p", cfg, [&]{ ... });`
//     The function creates AND destroys the scope internally around the lambda
//     and returns the created Entity. The caller NEVER sees the guard — it has
//     already been popped by the time the call returns.
//
// CONTRACT RULE (enforced by the type system, keep it that way): the closure
// form MUST return `Entity` and MUST NOT return `UIScope`. Returning a live
// UIScope from a closure forces the caller to hold it open (to silence
// [[nodiscard]]), which extends the guard's lifetime and locks the parenting
// stack inside that container for the rest of the calling function — silently
// nesting every subsequent sibling into it. That is exactly the CollapsingHeader
// defect this contract exists to prevent.
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
//
// A UIScope is a LIVE guard: while it is alive it owns an active push on the
// context stack. That means it may ONLY be returned by the RAII-form builder
// (e.g. Panel(name), Box(name), BeginCollapsingHeader(...)) — never by a
// closure form. A closure form that returns a UIScope hands ownership of a
// still-open scope to the caller, which is the exact lifetime-extension bug
// this header was written to prevent (see the top-of-file CONTRACT RULE).

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

    auto Slider(std::string_view id, std::string_view label, float& value, float minVal, float maxVal, float step = 0.0f, const SliderConfig& cfg = {}) -> Entity {
        return DragFloat(id, label, value, minVal, maxVal, step, cfg);
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

        // First, initialise the component text from the caller on create.
        // The root is a plain flex container WITHOUT a TextComponent (so Yoga
        // does not attach a measure function — nodes with measure funcs cannot
        // have children). The editable text lives on a dedicated inner child
        // `_ti_text` that is a leaf (no children of its own), and an optional
        // `_ti_label` child sits alongside it. The UITextInputComponent stays
        // on the root so the engine's key handler finds it by entity type.
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
                Components::UITextInputComponent {.text = initialText, .cursorIndex = 0, .isFocused = false, .edited = false}
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

        // Click on header toggles expansion (ConsumeClick reads + clears the flag).
        if (ConsumeClick(e)) {
            dd->expanded = !dd->expanded;
        }

        // Update header panel color to reflect hover
        m_reg->Patch<Components::UIPanelComponent>(e, [&](auto& pc) -> auto {
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
                            .flexGrow      = 1.0f,
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

                // Ensure parent/depth/sizing stays correct if the menu box moved
                m_reg->Patch<Components::UIRectComponent>(itemEnt, [&](auto& r) -> auto {
                    r.parentEntity   = menuParent;
                    r.hierarchyDepth = menuDepth;
                    r.height         = cfg.itemHeight;
                });
                if (auto* iflex = m_reg->Get<Components::UIFlexComponent>(itemEnt)) {
                    iflex->flexGrow = 1.0f;
                }

                // Update text/color
                m_reg->Patch<Components::TextComponent>(itemEnt, [&](auto& tc) -> auto {
                    tc.text.assign(std::string_view(dd->options[i]));
                });
                bool isSelected = (i == dd->selectedIdx);
                bool isItemHover = IsHovered(itemEnt);
                m_reg->Patch<Components::UIPanelComponent>(itemEnt, [&](auto& pc) -> auto {
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
    // COLLAPSINGHEADER  —  ui.CollapsingHeader(id, label, defaultOpen, fn, [cfg])
    // -----------------------------------------------------------------
    // Closure form: the header scope and the indented content box are created
    // AND destroyed entirely inside this function around `fn`. It returns the
    // created header Entity — never a UIScope. Holding a live UIScope here
    // would force callers to capture it (to satisfy [[nodiscard]]) and extend
    // its lifetime to the end of the caller's function, which locks the
    // parenting stack inside this header for the rest of the frame and silently
    // nests every following sibling into it. Return types are the enforcement:
    // closure → Entity, RAII → BeginCollapsingHeader() → UIScope.
    //
    // When the header is collapsed, `fn` is NOT invoked; the previous frame's
    // content box (and its descendants) are swept at the end of the call.
    template <typename Fn>
        requires std::invocable<Fn>
    auto CollapsingHeader(std::string_view id, std::string_view label, bool defaultOpen, Fn&& content, const CollapsingHeaderConfig& cfg = {}) -> Entity {
        uint32_t depth = 0;
        Entity   e     = PrepareCollapsingHeader(id, label, defaultOpen, cfg, depth);

        auto* hdr = m_reg->Get<Components::UICollapsingHeaderComponent>(e);
        if (hdr->isOpen) {
            // Both guards are strictly local. They are destroyed, in reverse
            // declaration order (boxScope then scope), when this function
            // returns — content box popped and swept, then the header popped
            // and swept. The caller never sees either one.
            UIScope scope = PushScope(e, depth);
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
            return e;
        }

        // Collapsed: the content subtree wasn't visited this frame. Sweep e's
        // children so last frame's _content box (and its descendants) are
        // reclaimed; the header entity itself stays alive for its cached state.
        SweepStaleChildren(e);
        return e;
    }

    // Shorthand: id == label (matches the sample and most tool-inspector uses).
    template <typename Fn>
        requires std::invocable<Fn>
    auto CollapsingHeader(std::string_view label, bool defaultOpen, Fn&& content, const CollapsingHeaderConfig& cfg = {}) -> Entity {
        return CollapsingHeader(label, label, defaultOpen, std::forward<Fn>(content), cfg);
    }

    // -----------------------------------------------------------------
    // BEGINCOLLAPSINGHEADER  —  pure RAII form, no lambda
    // -----------------------------------------------------------------
    // The manual/RAII counterpart to the closure form above. Hand the guard to
    // the caller so a C++ scope block can inject content between the begin and
    // the guard's destruction:
    //
    //     {
    //         auto hdr = ui.BeginCollapsingHeader("Display", "Display", true);
    //         if (hdr.IsPushed()) {
    //             ui.Label("Inside the open header");
    //         }
    //     } // ~UIScope pops the content box (and sweeps it)
    //
    // The returned guard is the INDENTED CONTENT BOX — a direct child of the
    // header (same cache key and components as the closure form's content box),
    // so the caller's widgets land inside it and the pop + sweep runs exactly
    // when the guard is destroyed. Only the content box is pushed; the header
    // is never on the stack, which keeps pops strictly LIFO (a UIScope pops the
    // stack top, so you must never pop an outer scope while an inner one is
    // still open — that is why this form does not stack the header at all).
    // A closed header yields a DISENGAGED guard (IsPushed() == false) — listen
    // to it and skip content, exactly as the closure form skips `fn`.
    [[nodiscard]] auto BeginCollapsingHeader(std::string_view id, std::string_view label, bool defaultOpen, const CollapsingHeaderConfig& cfg = {}) -> UIScope {
        uint32_t depth = 0;
        Entity   e     = PrepareCollapsingHeader(id, label, defaultOpen, cfg, depth);

        auto* hdr = m_reg->Get<Components::UICollapsingHeaderComponent>(e);
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
                    Components::UIRectComponent {.parentEntity = e, .width = 0.0f, .height = 0.0f, .hierarchyDepth = depth + 1},
                    Components::UIPanelComponent {.color = {0.0f, 0.0f, 0.0f, 0.0f}},
                    Components::UIFlexComponent {
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
            m_reg->Patch<Components::UIRectComponent>(contentBox, [&](auto& r) -> auto {
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

    // Shorthand: id == label.
    [[nodiscard]] auto BeginCollapsingHeader(std::string_view label, bool defaultOpen, const CollapsingHeaderConfig& cfg = {}) -> UIScope {
        return BeginCollapsingHeader(label, label, defaultOpen, cfg);
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
    auto Columns(std::string_view id, SplitDirection direction, float& ratio, LeftFn&& leftFn, RightFn&& rightFn, const SplitterConfig& cfg = {}) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        ratio = std::clamp(ratio, 0.05f, 0.95f);

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                Components::UIRectComponent {.parentEntity = parent, .width = 0.0f, .height = 0.0f, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = {0.0f, 0.0f, 0.0f, 0.0f}},
                Components::UIFlexComponent {
                    .direction  = (direction == SplitDirection::Horizontal) ? FlexDirection::Row : FlexDirection::Column,
                    .alignItems = FlexAlign::Stretch,
                    .gapX        = 0.0f,
                    .gapY        = 0.0f
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

        // Cached splitter entities may survive for many frames (and the same
        // id can be used with either orientation), so refresh their layout
        // configuration before building the two child panes.
        m_reg->Patch<Components::UIRectComponent>(e, [&](auto& r) -> auto {
            r.parentEntity   = parent;
            r.width          = 0.0f;
            r.height         = 0.0f;
            r.hierarchyDepth = depth;
        });
        m_reg->Patch<Components::UIFlexComponent>(e, [&](auto& f) -> auto {
            f.direction  = (direction == SplitDirection::Horizontal) ? FlexDirection::Row : FlexDirection::Column;
            f.alignItems = FlexAlign::Stretch;
            f.flexGrow   = 0.0f;
            f.flexShrink = 1.0f;
            f.flexBasis  = -1.0f;
            f.gapX       = 0.0f;
            f.gapY       = 0.0f;
        });

        auto* split = m_reg->Get<Components::UISplitterComponent>(e);

        // Sync external ratio change: if the caller mutated `ratio` since
        // last frame and the user isn't actively dragging, reflect it into
        // the ECS. Otherwise the ECS value is authoritative (user dragged
        // the handle). Clamp defensively either way.
        ratio = std::clamp(ratio, 0.05f, 0.95f);
        if (!split->isDragging && std::abs(ratio - split->previousRatio) > 1e-5f) {
            split->ratio = ratio;
        } else {
            ratio = split->ratio;
        }
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
                // The pane's size is supplied by flex-grow on the main axis;
                // leave the cross-axis height auto so Yoga can derive it from
                // the pane's content.
                lr.width  = 0.0f;
                lr.height = 0.0f;
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
            // Handle hover color — read directly from the handle's own
            // UIButtonComponent (single source of truth; no duplicated flag).
            bool handleHover = IsHovered(handleEnt) || (split != nullptr && split->isDragging);
            m_reg->Patch<Components::UIPanelComponent>(handleEnt, [&](auto& hp) -> auto {
                hp.color = handleHover ? cfg.hoverColor : cfg.handleColor;
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
                rr.width          = 0.0f;
                rr.height         = 0.0f;
            });
            if (auto* rflex = m_reg->Get<Components::UIFlexComponent>(rightEnt)) {
                rflex->flexGrow   = (1.0f - ratio) * 1000.0f;
                rflex->flexShrink = 1.0f;
                rflex->flexBasis  = 0.0f;
            }
            std::forward<RightFn>(rightFn)();
        }

        // Closure-overload contract: like Panel/Box(name, cfg, fn), the
        // scope is held locally so its destructor runs the pop+sweep on
        // return, and we give the caller back the entity (which is the
        // only thing a closure-based caller cares about — the scope was
        // already opened AND closed for them while `fn` executed, unlike
        // the guard-returning RAII form).
        Entity ent = scope.GetEntity();
        return ent;
    }

    // ui.Splitter is the same primitive with a name that evokes the drag handle
    template <typename LeftFn, typename RightFn>
        requires std::invocable<LeftFn> && std::invocable<RightFn>
    auto Splitter(std::string_view id, SplitDirection direction, float& ratio, LeftFn&& leftFn, RightFn&& rightFn, const SplitterConfig& cfg = {}) -> Entity {
        return Columns(id, direction, ratio, std::forward<LeftFn>(leftFn), std::forward<RightFn>(rightFn), cfg);
    }

    template <typename LeftFn, typename RightFn>
        requires std::invocable<LeftFn> && std::invocable<RightFn>
    auto Columns(SplitDirection direction, float& ratio, LeftFn&& leftFn, RightFn&& rightFn, const SplitterConfig& cfg = {}) -> Entity {
        std::array<char, 64> nameBuf {};
        std::string_view     autoName = FormatTo(nameBuf, "Split_D{}_{}", GetCurrentDepth(), m_autoIdCounter++);
        return Columns(autoName, direction, ratio, std::forward<LeftFn>(leftFn), std::forward<RightFn>(rightFn), cfg);
    }

    template <typename LeftFn, typename RightFn>
        requires std::invocable<LeftFn> && std::invocable<RightFn>
    auto Splitter(SplitDirection direction, float& ratio, LeftFn&& leftFn, RightFn&& rightFn, const SplitterConfig& cfg = {}) -> Entity {
        return Columns(direction, ratio, std::forward<LeftFn>(leftFn), std::forward<RightFn>(rightFn), cfg);
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

    // Consume a pending click on an entity's UIButtonComponent and return
    // whether it fired this frame. Centralised so every compound widget
    // handles click state in exactly one way.
    [[nodiscard]] auto ConsumeClick(Entity e) noexcept -> bool {
        bool clicked = false;
        if (auto* btn = m_reg->Get<Components::UIButtonComponent>(e)) {
            if (btn->Has(UIButton::Clicked)) {
                clicked = true;
                btn->Set(UIButton::Clicked, false);
            }
        }
        return clicked;
    }

    // Read hover state directly from the entity's UIButtonComponent. This is
    // the single source of truth for whether a widget is hovered — compound
    // widgets never cache their own hover flag.
    [[nodiscard]] auto IsHovered(Entity e) const noexcept -> bool {
        if (const auto* btn = m_reg->Get<Components::UIButtonComponent>(e)) {
            return btn->Has(UIButton::Hovered);
        }
        return false;
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

    void PatchCheckboxVisuals(Entity e, const CheckboxConfig& cfg, bool checked) {
        bool isHovered = IsHovered(e);
        Entity boxEnt  = FindChildByKey(e, HashStringView("_cb_box"));
        if (boxEnt != Entity::Null()) {
            m_reg->Patch<Components::UIPanelComponent>(boxEnt, [&](auto& panel) -> auto {
                panel.color        = isHovered ? cfg.hoverColor : cfg.boxColor;
                panel.edgeWidth    = 1.0f;
                panel.borderRadius = cfg.borderRadius;
            });
        }
        // Update check-mark visibility (mark is a child of boxEnt)
        if (boxEnt != Entity::Null()) {
            Entity markEnt = FindChildByKey(boxEnt, HashStringView("_cb_mark"));
            if (markEnt != Entity::Null()) {
                m_reg->Patch<Components::UIPanelComponent>(markEnt, [&](auto& mc) -> auto {
                    mc.color = checked ? cfg.checkColor : JPH::Vec4 {0, 0, 0, 0};
                });
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
                Components::UIRectComponent {.parentEntity = parent, .width = 0.0f, .height = cfg.trackHeight, .hierarchyDepth = parentDepth + 1},
                Components::UIPanelComponent {
                    .color        = cfg.trackColor,
                    .borderRadius = {cfg.trackHeight * 0.5f, cfg.trackHeight * 0.5f, cfg.trackHeight * 0.5f, cfg.trackHeight * 0.5f}
                },
                Components::UIFlexComponent {
                    .direction  = FlexDirection::Row,
                    .alignItems = FlexAlign::Center,
                    .flexGrow   = 1.0f,
                    .flexShrink = 1.0f,
                    .flexBasis  = -1.0f
                },
                Components::UIButtonComponent {}
            );
        });

        // The track is a thin visual bar, not another full-height row.  Keep
        // its layout properties in sync for cached widgets as well as newly
        // created ones.
        m_reg->Patch<Components::UIRectComponent>(trackEnt, [&](auto& tr) -> auto {
            tr.parentEntity   = parent;
            tr.width          = 0.0f;
            tr.height         = std::max(0.0f, cfg.trackHeight);
            tr.hierarchyDepth = parentDepth + 1;
        });
        m_reg->Patch<Components::UIFlexComponent>(trackEnt, [&](auto& tf) -> auto {
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
        m_reg->Patch<Components::UIPanelComponent>(trackEnt, [&](auto& tp) -> auto {
            tp.color        = cfg.trackColor;
            const float r   = std::max(0.0f, cfg.trackHeight) * 0.5f;
            tp.borderRadius = {r, r, r, r};
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

        // Knob visual child on the track.  The margin is part of the Yoga
        // layout, so changing the value moves the knob instead of leaving a
        // marker permanently at the track's flex-start edge.
        float range = maxVal - minVal;
        float t     = (range > 0.0f) ? ((value - minVal) / range) : 0.0f;
        t           = std::clamp(t, 0.0f, 1.0f);

        Entity knobEnt = GetOrCreateChild(trackEnt, HashStringView("_sl_knob"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_sl_knob")},
                Components::UIRectComponent {
                    .parentEntity   = trackEnt,
                    .width          = std::max(0.0f, cfg.knobSize),
                    .height         = std::max(0.0f, cfg.knobSize),
                    .hierarchyDepth = parentDepth + 2
                },
                Components::UIPanelComponent {
                    .color        = cfg.knobColor,
                    .borderRadius = {cfg.knobSize * 0.5f, cfg.knobSize * 0.5f, cfg.knobSize * 0.5f, cfg.knobSize * 0.5f}
                },
                Components::UIFlexComponent {},
                Components::UIButtonComponent {},
                Components::UIDragComponent {.targetEntity = sliderEntity, .isDragging = false}
            );
        });

        // `cfg.width` is the best estimate available before Yoga has laid out
        // a fill-width slider.  Once a cached track has a computed width, use
        // that width so the first frame after a resize also converges to the
        // true travel distance.  The fallback keeps the value-dependent
        // position useful on the very first frame of a fill-width widget.
        float trackWidth = 0.0f;
        if (const auto* tr = m_reg->Get<Components::UIRectComponent>(trackEnt)) {
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

        m_reg->Patch<Components::UIRectComponent>(knobEnt, [&](auto& kr) -> auto {
            kr.parentEntity   = trackEnt;
            kr.width          = knobSize;
            kr.height         = knobSize;
            kr.hierarchyDepth = parentDepth + 2;
        });
        // GetOrCreateChild also supports widgets created by an older build;
        // make sure those cached knobs acquire the flex component needed for
        // marginLeft before patching it.
        if (m_reg->Get<Components::UIFlexComponent>(knobEnt) == nullptr) {
            m_reg->Add<Components::UIFlexComponent>(knobEnt);
        }
        m_reg->Patch<Components::UIFlexComponent>(knobEnt, [&](auto& kf) -> auto {
            kf.flexGrow   = 0.0f;
            kf.flexShrink = 0.0f;
            kf.flexBasis  = -1.0f;
            kf.marginLeft = t * knobTravel;
            kf.marginTop = kf.marginRight = kf.marginBottom = 0.0f;
        });
        m_reg->Patch<Components::UIPanelComponent>(knobEnt, [&](auto& pc) -> auto {
            // Hover on either the root slider entity, the track, or the knob itself
            bool hover  = IsHovered(sliderEntity) || IsHovered(trackEnt) || IsHovered(knobEnt);
            auto* s     = m_reg->Get<Components::UISliderComponent>(sliderEntity);
            bool active = (s != nullptr && s->isDragging) || hover;
            pc.color = active ? cfg.hoverColor : cfg.knobColor;
            pc.borderRadius = {knobSize * 0.5f, knobSize * 0.5f, knobSize * 0.5f, knobSize * 0.5f};
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

        // Ensure the leaf text child exists (leaf — no children, so Yoga may
        // safely attach a measure function to it). Sync it from the
        // UITextInputComponent which is the authoritative text store.
        if (auto* input = m_reg->Get<Components::UITextInputComponent>(e)) {
            uint32_t parentDepth = 0;
            if (const auto* r = m_reg->Get<Components::UIRectComponent>(e)) {
                parentDepth = r->hierarchyDepth;
            }
            Entity textEnt = GetOrCreateChild(e, HashStringView("_ti_text"), [&]() -> Entity {
                return m_reg->Create(
                    Components::NameComponent {.name = String64("_ti_text")},
                    Components::UIRectComponent {.parentEntity = e, .height = cfg.height, .hierarchyDepth = parentDepth + 1},
                    Components::UIFlexComponent {.flexGrow = 1.0f},
                    Components::TextComponent {
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
            m_reg->Patch<Components::UIRectComponent>(textEnt, [&](auto& tr) -> auto {
                tr.parentEntity   = e;
                tr.hierarchyDepth = parentDepth + 1;
                tr.height         = cfg.height;
            });
            // Sync displayed text
            m_reg->Patch<Components::TextComponent>(textEnt, [&](auto& tc) -> auto {
                tc.text      = input->text;
                tc.color     = focused ? JPH::Vec4 {1.0f, 1.0f, 1.0f, 1.0f} : cfg.textColor;
                tc.scale     = cfg.scale;
                tc.fontIndex = font;
            });
            if (auto* tflex = m_reg->Get<Components::UIFlexComponent>(textEnt)) {
                tflex->flexGrow = 1.0f;
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
        // Create the menu container box directly under the dropdown entity.
        //
        // IMPORTANT: do NOT call PushScope(ddEnt) and then Dismiss() it here
        // — Dismiss() runs SweepStaleChildren(ddEnt), which at this point has
        // NOT yet seen the menu items we're about to create and would sweep
        // them (and any previously-visited children) as stale. Just create
        // the menuBox via GetOrCreateChild (which marks ddEnt's cache
        // visited on create/lookup) and push only the menuBox onto the stack
        // so option items are parented under it.
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
        return PushScope(menuBox, depth + 1);
    }

    // Shared preamble for the collapsing-header family. Both the closure form
    // (CollapsingHeader -> Entity) and the RAII form (BeginCollapsingHeader ->
    // UIScope) delegate here so the two paradigms can never drift in behaviour.
    // It creates/reuses the header entity, ensures the clickable title child,
    // applies the open/close toggle for the frame, and patches the panel colour.
    // Returns the header entity and writes the header's live depth into
    // `outDepth` so the caller can push the content scope at the right depth.
    auto PrepareCollapsingHeader(std::string_view id, std::string_view label, bool defaultOpen, const CollapsingHeaderConfig& cfg, uint32_t& outDepth) -> Entity {
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
        m_reg->Patch<Components::UIPanelComponent>(e, [&](auto& pc) -> auto {
            pc.color = titleHover ? cfg.hoverColor : (hdr->isOpen ? cfg.openColor : cfg.bgColor);
        });

        outDepth = depth;
        return e;
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

// ============================================================================
// COMPILE-TIME CONTRACT ENFORCEMENT (the "never again" guard)
// ============================================================================
// The closure forms of every container MUST return Entity and MUST NOT return a
// live UIScope. Returning a UIScope from a closure forces callers to capture it
// to satisfy [[nodiscard]], which extends the guard's lifetime to the end of the
// caller's function, locks the parenting stack inside that container, and
// silently nests every subsequent sibling inside it — the exact CollapsingHeader
// defect this header was fixed to prevent. If any assert below fails, a closure
// overload was regressed back to returning UIScope: stop and fix the return
// type, do not silence the assert. The RAII forms (Panel/Box/BeginCollapsingHeader)
// are the ONLY overloads that legitimately return a UIScope.
static_assert(std::is_same_v<decltype(std::declval<Context&>().CollapsingHeader("a", "a", true, []() {})), Entity>);
static_assert(std::is_same_v<decltype(std::declval<Context&>().CollapsingHeader("a", true, []() {})), Entity>);
static_assert(std::is_same_v<decltype(std::declval<Context&>().BeginCollapsingHeader("a", "a", true)), UIScope>);
static_assert(std::is_same_v<decltype(std::declval<Context&>().BeginCollapsingHeader("a", true)), UIScope>);

} // namespace ZHLN::GUI
