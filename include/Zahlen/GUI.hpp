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
#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
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
// TEXT SHAPING — SHARED BY THE LAYOUT MEASURE FUNCTION AND THE RENDERER
// ============================================================================
// Both the Yoga measure callback and the UI renderer must agree on where a
// glyph lands and how tall a line is, otherwise wrapped text is measured as
// one height and drawn as another. These three helpers are that single source
// of truth.

/// Vertical advance between two baselines, matching AppendTextVertices.
[[nodiscard]] constexpr auto TextLineHeight(float scale) noexcept -> float {
    return 36.0f * scale;
}

/// Capacity of the scratch buffer used to hold wrapped text. Wrapping can only
/// ever ADD characters (one '\n' per break), so this comfortably holds a full
/// String256 label plus its breaks; anything longer is truncated.
constexpr size_t kWrapBufferCapacity = 512;

/// Greedy word wrap. Breaks at spaces so that no line is wider than
/// `maxWidth` (a word longer than the width is hard-broken), and preserves
/// explicit '\n' breaks. Returns the number of lines written. `maxWidth <= 0`
/// disables wrapping and copies the text through unchanged.
///
/// Output is written into a caller-owned fixed-capacity string, so the hot
/// layout path never allocates; text that does not fit the buffer is truncated
/// at a character boundary (and the terminator stays intact).
template <size_t Capacity>
auto WrapTextInto(const FontAtlas& font, std::string_view text, float scale, float maxWidth, ZHLN::FixedString<Capacity>& out) noexcept -> uint32_t {
    out.clear();
    if (text.empty()) {
        return 0;
    }
    if (maxWidth <= 0.0f) {
        out.assign(text);
        uint32_t lines = 1;
        for (char c: text) {
            if (c == '\n') {
                ++lines;
            }
        }
        return lines;
    }

    auto WidthOf = [&](std::string_view s) -> float { return MeasureTextBounds(font, s, scale).width(); };

    uint32_t totalLines = 0;

    // Wraps one '\n'-free line. Appends the wrapped lines separated by '\n'
    // (no trailing break) and returns how many lines it produced — always at
    // least one, so an explicitly blank source line still occupies a row.
    auto WrapHardLine = [&](std::string_view hard) -> uint32_t {
        uint32_t lines         = 0;
        size_t   lineStart     = 0;
        size_t   lineEnd       = 0;
        bool     lineHasContent = false;
        size_t   pos           = 0;

        auto CloseLine = [&]() -> void {
            out.append(hard.substr(lineStart, lineEnd - lineStart));
            out.append("\n");
            ++lines;
        };

        while (pos < hard.size()) {
            while (pos < hard.size() && hard[pos] == ' ') {
                ++pos; // collapse runs of spaces between words
            }
            if (pos >= hard.size()) {
                break;
            }
            const size_t wordStart = pos;
            while (pos < hard.size() && hard[pos] != ' ') {
                ++pos;
            }
            const size_t wordEnd = pos;

            if (!lineHasContent) {
                lineStart     = wordStart;
                lineEnd       = wordEnd;
                lineHasContent = true;
            } else if (WidthOf(hard.substr(lineStart, wordEnd - lineStart)) <= maxWidth) {
                // The word still fits: extend the pending line. The measured
                // slice spans the separating space, so the space is accounted
                // for without any string building.
                lineEnd = wordEnd;
            } else {
                CloseLine();
                lineStart = wordStart;
                lineEnd   = wordEnd;
            }

            // A single word wider than the whole line is hard-broken so it can
            // never overflow the container horizontally.
            while ((lineEnd - lineStart) > 1 && WidthOf(hard.substr(lineStart, lineEnd - lineStart)) > maxWidth) {
                size_t cut = lineStart + 1;
                while (cut + 1 < lineEnd && WidthOf(hard.substr(lineStart, (cut + 1) - lineStart)) <= maxWidth) {
                    ++cut;
                }
                out.append(hard.substr(lineStart, cut - lineStart));
                out.append("\n");
                ++lines;
                lineStart = cut;
            }
        }

        if (lineEnd > lineStart) {
            out.append(hard.substr(lineStart, lineEnd - lineStart));
            ++lines;
        }
        return std::max<uint32_t>(lines, 1);
    };

    size_t hardStart = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            if (totalLines > 0) {
                out.append("\n");
            }
            totalLines += WrapHardLine(text.substr(hardStart, i - hardStart));
            hardStart = i + 1;
        }
    }

    return totalLines;
}

/// Measure `text` as it would be laid out when wrapped at `maxWidth`. Used by
/// Yoga's measure callback so an auto-height label grows by whole lines.
[[nodiscard]] auto MeasureWrappedTextBounds(const FontAtlas& font, std::string_view text, float scale, float maxWidth) noexcept -> TextBounds;

// ============================================================================
// IMAGE / SPRITE PRIMITIVE GEOMETRY
// ============================================================================

/// Number of vertices AppendImageVertices needs for this rect/scale-mode pair.
/// Tile emits one quad per repeat, so the count depends on the rect size.
[[nodiscard]] auto CountImageVertices(const Components::UIRectComponent& rect, const Components::UIImageComponent& image) noexcept -> uint32_t;

/// Emits the textured quad(s) for a UIImageComponent, honouring its scale mode
/// and sub-UV region. Returns the number of vertices written (never more than
/// CountImageVertices).
auto AppendImageVertices(
    VertexPosition*                     outPos,
    VertexAttributes*                   outAttr,
    const Components::UIRectComponent&  rect,
    const Components::UIImageComponent& image
) -> uint32_t;

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

// Depth assigned to the top-level overlay root. The renderer draws in
// ascending depth order and the interaction pass hit-tests in descending
// order, so parking popups, dropdown menus, context menus and tooltips here
// puts them above every widget AND gives them the first claim on the pointer.
// It is far above any reachable nesting depth (MAX_UI_STACK_DEPTH) on purpose.
constexpr uint32_t UI_OVERLAY_DEPTH = 4096;

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

    // Automatic word wrapping. Set `height = 0` as well: a fixed height would
    // clip the extra lines the wrapper produces.
    bool  wrap     = false;
    float maxWidth = 0.0f; // 0 = wrap at the width the container offers
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

// Scrollable container. `height` is the VIEWPORT height: the content inside is
// laid out at its natural height and scrolled, so the box needs an explicit
// height to have anything to scroll within (0 falls back to `defaultHeight`).
struct ScrollBoxConfig {
    float width         = 0.0f;   // 0 = fill the parent's cross axis
    float height        = 240.0f; // Viewport height in pixels
    float gap           = 4.0f;
    float padding       = 8.0f;

    // 0 keeps `height` authoritative, which is what a fixed inspector list
    // wants. Set it to 1 to let the box absorb the parent's free space
    // instead (a docking-panel browser that fills whatever is left over).
    float flexGrow = 0.0f;

    JPH::Vec4 bgColor       = {0.05f, 0.07f, 0.11f, 0.85f};
    JPH::Vec4 trackColor    = {0.08f, 0.11f, 0.16f, 0.90f};
    JPH::Vec4 thumbColor    = {0.26f, 0.38f, 0.58f, 0.85f};
    JPH::Vec4 thumbHoverColor = {0.40f, 0.58f, 0.86f, 1.00f};

    float scrollbarWidth = 8.0f;  // 0 hides the scrollbar (scrolling still works)
    float scrollSpeed    = 35.0f; // Pixels per wheel notch
    float smoothSpeed    = 15.0f; // Easing rate; ignored when smoothScroll is off
    bool  smoothScroll   = true;
    bool  allowHorizontal = false;
};

// ui.Image / ui.Icon. `uv0`/`uv1` select a sprite-sheet region; `sourceWidth`
// and `sourceHeight` are that region's native pixel size and are what
// FitAspect/CropAspect/Tile measure against.
struct ImageConfig {
    float width  = 0.0f;
    float height = 0.0f;

    ImageScaleMode mode = ImageScaleMode::Stretch;
    JPH::Vec4      tint = {1.0f, 1.0f, 1.0f, 1.0f};

    float uv0x = 0.0f;
    float uv0y = 0.0f;
    float uv1x = 1.0f;
    float uv1y = 1.0f;

    float sourceWidth  = 0.0f;
    float sourceHeight = 0.0f;
};

// Full-width list row: scene hierarchies, asset browsers, inventory slots.
struct SelectableConfig {
    float width  = 0.0f; // 0 = fill
    float height = 24.0f;
    float scale  = 0.85f;
    float indent = 0.0f; // Left inset, used by TreeNode for nesting levels

    JPH::Vec4 normalColor   = {0.00f, 0.00f, 0.00f, 0.00f};
    JPH::Vec4 hoverColor    = {0.20f, 0.28f, 0.42f, 0.70f};
    JPH::Vec4 selectedColor = {0.26f, 0.46f, 0.78f, 0.90f};
    JPH::Vec4 activeColor   = {0.34f, 0.56f, 0.92f, 1.00f}; // Selected AND hovered
    JPH::Vec4 textColor     = {0.85f, 0.90f, 1.00f, 1.00};
    JPH::Vec4 selectedTextColor = {1.00f, 1.00f, 1.00f, 1.00f};
    JPH::Vec4 borderRadius  = {3.0f, 3.0f, 3.0f, 3.0f};

    TextAlignment align = TextAlignment::Left;

    uint32_t doubleClickSpan = 18; // Frames allowed between the two clicks
};

// Floating hint drawn on the overlay layer while the owner widget is hovered.
struct TooltipConfig {
    float     scale       = 0.80f;
    uint32_t  delayFrames = 20;   // Hover frames before the tooltip appears
    JPH::Vec4 bgColor     = {0.05f, 0.07f, 0.11f, 0.98f};
    JPH::Vec4 textColor   = {0.90f, 0.95f, 1.00f, 1.00f};
    JPH::Vec4 borderColor = {0.26f, 0.38f, 0.58f, 1.00f};
    float     padding     = 8.0f;
    float     maxWidth    = 320.0f; // 0 = single line, no wrapping
    float     offsetX     = 14.0f;
    float     offsetY     = 18.0f;
};

// Overlay-rooted popup (dropdown menus, context menus). `width`/`height` are
// 0 = size to content; the anchor is taken from the owner widget's last
// laid-out rect (popups therefore settle one frame after the owner moves).
struct PopupConfig {
    float     width         = 0.0f;
    float     height        = 0.0f; // 0 = size to content
    float     maxHeight     = 320.0f;
    JPH::Vec4 bgColor       = {0.07f, 0.10f, 0.16f, 0.98f};
    JPH::Vec4 borderColor   = {0.20f, 0.28f, 0.44f, 1.00f};
    JPH::Vec4 borderRadius  = {4.0f, 4.0f, 4.0f, 4.0f};
    float     gap           = 2.0f;
    float     padding       = 4.0f;
    bool      openUpward    = false; // Flip above the owner when it does not fit
};

// Tree row: a Selectable with an expand arrow plus an indented content box.
struct TreeNodeConfig {
    SelectableConfig row;
    float            indent     = 14.0f;
    float            gap        = 2.0f;
    JPH::Vec4        arrowColor = {0.70f, 0.82f, 1.00f, 1.00f};
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
        // Overlay first: a popup whose owner stopped expanding this frame is
        // still a live child of the overlay root, and only the overlay's own
        // sweep reclaims it. Sweeping the root cache first could destroy the
        // overlay root itself when nothing was popped up this frame, which
        // would then latch a spurious ParentNotAlive into Status().
        if (m_overlayRoot != Entity::Null() && m_reg->IsAlive(m_overlayRoot)) {
            SweepStaleChildren(m_overlayRoot);
        }
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
                m_lastItem               = record->entity; // "last item" for Tooltip()/IsItemHovered()
                return record->entity;
            }
        }

        // 2. Not found -> Spawn new entity
        Entity newEntity = createFn();
        cache->children.Insert(widgetKey, Components::UIChildCacheComponent::ChildRecord {.entity = newEntity, .lastVisitedFrame = m_currentFrame});

        m_lastItem = newEntity;
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
                    .fontIndex     = fontHandle,
                    .wrapText      = cfg.wrap,
                    .wrapWidth     = cfg.maxWidth
                }
            );
        });

        m_reg->Patch<Components::TextComponent>(e, [&](auto& textComp) -> auto {
            textComp.text.assign(text);
            textComp.color     = cfg.color;
            textComp.scale     = cfg.scale;
            textComp.align     = cfg.align;
            textComp.wrapText  = cfg.wrap;
            textComp.wrapWidth = cfg.maxWidth;
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

    // -----------------------------------------------------------------
    // SCROLLBOX  —  ui.ScrollBox(id, cfg, fn) / ui.BeginScrollBox(id, cfg)
    // -----------------------------------------------------------------
    // A fixed-height clipping viewport over content laid out at its natural
    // height. Structure (all flex, nothing absolutely positioned):
    //
    //   id                     Row  [ viewport | scrollbar track ]
    //   +- _sb_viewport        Column, clipChildren, UIScrollComponent
    //   |    +- <your widgets>
    //   +- _sb_track
    //        +- _sb_thumb      marginTop = scroll fraction * travel
    //
    // The scroll state lives on the VIEWPORT, so only its subtree is offset by
    // the scroll delta in the layout pass and only its children contribute to
    // the measured content extent. The caller's widgets are parented under the
    // viewport: the returned guard IS the viewport scope.
    [[nodiscard]] auto BeginScrollBox(std::string_view id, const ScrollBoxConfig& cfg = {}) -> UIScope {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const float viewportHeight = (cfg.height > 0.0f) ? cfg.height : 240.0f;

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                Components::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = viewportHeight, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = cfg.bgColor},
                Components::UIFlexComponent {
                    .direction  = FlexDirection::Row,
                    .alignItems = FlexAlign::Stretch,
                    .flexGrow   = cfg.flexGrow,
                    // Never shrink: the root is an item in the parent's column,
                    // so Yoga's default shrink-to-fit would squeeze the
                    // viewport (and everything in it) the moment the panel's
                    // content overflowed -- a 300px viewport collapsing to 37px
                    // and scrolling nothing. A scroll viewport's height is the
                    // whole point of the widget; overflow belongs to the
                    // content, not to the box.
                    .flexShrink = 0.0f,
                    .flexBasis  = -1.0f
                }
            );
        });

        m_reg->Patch<Components::UIRectComponent>(e, [&](auto& r) -> auto {
            r.parentEntity   = parent;
            r.width          = cfg.width;
            r.height         = viewportHeight;
            r.hierarchyDepth = depth;
            r.clipChildren   = false;
        });
        // flexGrow decides whether `height` is authoritative (0) or a base the
        // parent's free space is added to (1). It is patched every frame so a
        // cached box follows a config change instead of keeping last frame's.
        m_reg->Patch<Components::UIFlexComponent>(e, [&](auto& f) -> auto {
            f.direction  = FlexDirection::Row;
            f.alignItems = FlexAlign::Stretch;
            f.flexGrow   = cfg.flexGrow;
            f.flexShrink = 0.0f; // see the create path: the viewport height is authoritative
            f.flexBasis  = -1.0f;
        });

        // Viewport: the clipping, scrolling surface the content lives in.
        Entity viewport = GetOrCreateChild(e, HashStringView("_sb_viewport"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_sb_viewport")},
                Components::UIRectComponent {.parentEntity = e, .width = 0.0f, .height = 0.0f, .hierarchyDepth = depth + 1, .clipChildren = true},
                Components::UIPanelComponent {.color = {0.0f, 0.0f, 0.0f, 0.0f}},
                Components::UIFlexComponent {
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
                Components::UIScrollComponent {
                    .scrollSpeed     = cfg.scrollSpeed,
                    .smoothSpeed     = cfg.smoothSpeed,
                    .smoothScroll    = cfg.smoothScroll,
                    .allowHorizontal = cfg.allowHorizontal
                }
            );
        });
        m_reg->Patch<Components::UIRectComponent>(viewport, [&](auto& r) -> auto {
            r.parentEntity   = e;
            r.hierarchyDepth = depth + 1;
            r.clipChildren   = true;
        });
        m_reg->Patch<Components::UIFlexComponent>(viewport, [&](auto& f) -> auto {
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
        m_reg->Patch<Components::UIScrollComponent>(viewport, [&](auto& s) -> auto {
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

    // Closure form: opens and closes the viewport scope around `content` and
    // returns the ScrollBox root entity (see the CONTRACT RULE at the top).
    template <typename Fn>
        requires std::invocable<Fn>
    auto ScrollBox(std::string_view id, const ScrollBoxConfig& cfg, Fn&& content) -> Entity {
        UIScope scope = BeginScrollBox(id, cfg);
        Entity  root  = Entity::Null();
        if (scope.IsPushed()) {
            // The pushed scope is the viewport; its parent is the ScrollBox root.
            if (const auto* vr = m_reg->Get<Components::UIRectComponent>(scope.GetEntity())) {
                root = vr->parentEntity;
            }
        }
        std::forward<Fn>(content)();
        return root;
    }

    template <typename Fn>
        requires std::invocable<Fn>
    auto ScrollBox(std::string_view id, Fn&& content) -> Entity {
        return ScrollBox(id, ScrollBoxConfig {}, std::forward<Fn>(content));
    }

    // -----------------------------------------------------------------
    // IMAGE / ICON  —  ui.Image(id, texture, [cfg]) / ui.Icon(id, texture, size)
    // -----------------------------------------------------------------
    // A first-class textured quad. The entity carries no UIPanelComponent, so
    // the renderer emits exactly one primitive for it: the image. Scale modes
    // and the sub-UV region are honoured by GUI::AppendImageVertices.
    auto Image(std::string_view id, TextureHandle texture, const ImageConfig& cfg = {}) -> Entity {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                Components::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                Components::UIImageComponent {
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
        m_reg->Patch<Components::UIRectComponent>(e, [&](auto& r) -> auto {
            r.parentEntity   = parent;
            r.width          = cfg.width;
            r.height         = cfg.height;
            r.hierarchyDepth = depth;
        });
        m_reg->Patch<Components::UIImageComponent>(e, [&](auto& img) -> auto {
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

    // Square, aspect-preserving icon — the 90% case for toolbars and buttons.
    auto Icon(std::string_view id, TextureHandle texture, float size = 24.0f, ImageScaleMode mode = ImageScaleMode::FitAspect) -> Entity {
        ImageConfig cfg;
        cfg.width        = size;
        cfg.height       = size;
        cfg.mode         = mode;
        cfg.sourceWidth  = size;
        cfg.sourceHeight = size;
        return Image(id, texture, cfg);
    }

    // -----------------------------------------------------------------
    // SELECTABLE  —  ui.Selectable(id, label, selected, [cfg], [onClick], [onDoubleClick])
    // -----------------------------------------------------------------
    // A full-width list row with distinct normal / hover / selected / active
    // styling. `selected` is authoritative in ECS (same contract as Checkbox):
    // the caller's bool is the initial value, ECS wins afterwards and is
    // written back every frame. Double-click is counted in FRAMES.
    auto Selectable(std::string_view id, std::string_view label, bool& selected, const SelectableConfig& cfg) -> Entity {
        uint32_t depth = 0;
        SelectableClickInfo info = PrepareSelectable(id, label, selected, cfg, std::string_view {}, false, depth);
        selected                 = info.selected;
        return info.entity;
    }

    auto Selectable(std::string_view id, std::string_view label, bool& selected) -> Entity {
        return Selectable(id, label, selected, SelectableConfig {});
    }

    template <typename OnClickFn>
        requires std::invocable<OnClickFn, bool>
    auto Selectable(std::string_view id, std::string_view label, bool& selected, const SelectableConfig& cfg, OnClickFn&& onClick) -> Entity {
        uint32_t depth            = 0;
        SelectableClickInfo info  = PrepareSelectable(id, label, selected, cfg, std::string_view {}, false, depth);
        selected                  = info.selected;
        if (info.clicked) {
            std::forward<OnClickFn>(onClick)(selected);
        }
        return info.entity;
    }

    template <typename OnClickFn>
        requires std::invocable<OnClickFn, bool>
    auto Selectable(std::string_view id, std::string_view label, bool& selected, OnClickFn&& onClick) -> Entity {
        return Selectable(id, label, selected, SelectableConfig {}, std::forward<OnClickFn>(onClick));
    }

    template <typename OnClickFn, typename OnDoubleClickFn>
        requires std::invocable<OnClickFn, bool> && std::invocable<OnDoubleClickFn>
    auto
        Selectable(std::string_view id, std::string_view label, bool& selected, const SelectableConfig& cfg, OnClickFn&& onClick, OnDoubleClickFn&& onDoubleClick)
            -> Entity {
        uint32_t depth           = 0;
        SelectableClickInfo info = PrepareSelectable(id, label, selected, cfg, std::string_view {}, false, depth);
        selected                 = info.selected;
        if (info.doubleClicked) {
            std::forward<OnDoubleClickFn>(onDoubleClick)();
        }
        if (info.clicked) {
            std::forward<OnClickFn>(onClick)(selected);
        }
        return info.entity;
    }

    // -----------------------------------------------------------------
    // TREENODE  —  ui.TreeNode(id, label, open, fn, [cfg], [onDoubleClick])
    // -----------------------------------------------------------------
    // A Selectable row with an expand arrow that toggles `open`, plus an
    // indented content box that only exists while the node is open. One bool
    // drives all three: a tree row's identity IS its branch, so the arrow, the
    // selected styling and the content all follow `open`.
    //
    // A single click toggles the branch. The second click of a double click is
    // NOT another toggle: it fires `onDoubleClick` (open the asset, focus the
    // object, ...) and leaves the expansion where the first click put it, so
    // double-clicking never closes the branch it just opened.
    template <typename Fn>
        requires std::invocable<Fn>
    auto TreeNode(std::string_view id, std::string_view label, bool& open, Fn&& content, const TreeNodeConfig& cfg = {}) -> Entity {
        return TreeNodeCore(id, label, open, std::forward<Fn>(content), cfg, []() {});
    }

    template <typename Fn>
        requires std::invocable<Fn>
    auto TreeNode(std::string_view label, bool& open, Fn&& content, const TreeNodeConfig& cfg = {}) -> Entity {
        return TreeNode(label, label, open, std::forward<Fn>(content), cfg);
    }

    template <typename Fn, typename OnDoubleClickFn>
        requires std::invocable<Fn> && std::invocable<OnDoubleClickFn>
    auto TreeNode(std::string_view id, std::string_view label, bool& open, Fn&& content, const TreeNodeConfig& cfg, OnDoubleClickFn&& onDoubleClick) -> Entity {
        return TreeNodeCore(id, label, open, std::forward<Fn>(content), cfg, std::forward<OnDoubleClickFn>(onDoubleClick));
    }


    // -----------------------------------------------------------------
    // TOOLTIP  —  ui.Tooltip("hint") / ui.Tooltip("hint", cfg)
    // -----------------------------------------------------------------
    // Attach to the widget built immediately before this call (the "last
    // item"), which is what a designer means when writing
    // `ui.Button("Save", ...); ui.Tooltip("Writes the scene to disk");`.
    // The hint itself is transient overlay geometry: it escapes every ancestor
    // scissor and floats above the whole UI.
    auto Tooltip(std::string_view text, const TooltipConfig& cfg = {}) -> Entity {
        return TooltipFor(m_lastItem, text, cfg);
    }

    // Explicit-owner form: describe a widget you hold an Entity for (an icon
    // inside a list row, a slider knob, ...).
    auto TooltipFor(Entity owner, std::string_view text, const TooltipConfig& cfg) -> Entity {
        return TooltipForImpl(owner, text, cfg);
    }
    auto TooltipFor(Entity owner, std::string_view text) -> Entity {
        return TooltipFor(owner, text, TooltipConfig {});
    }

    // -----------------------------------------------------------------
    // HOVER QUERIES
    // -----------------------------------------------------------------
    // The UIButtonComponent::Hovered flag is the single source of truth for
    // hover; these expose it to designers so a widget can react to hover
    // without owning a UIButtonComponent of its own.
    [[nodiscard]] auto IsHovered(Entity e) const noexcept -> bool {
        if (const auto* btn = m_reg->Get<Components::UIButtonComponent>(e)) {
            return btn->Has(UIButton::Hovered);
        }
        return false;
    }

    // Hover state of the widget built most recently in this frame.
    [[nodiscard]] auto IsItemHovered() const noexcept -> bool {
        return IsHovered(m_lastItem);
    }

    // The widget built most recently in this frame (Entity::Null() before the
    // first builder call). Useful for parenting a hand-rolled overlay to the
    // item a designer is describing.
    [[nodiscard]] auto GetLastItem() const noexcept -> Entity {
        return m_lastItem;
    }

    // -----------------------------------------------------------------
    // POPUP / OVERLAY  —  ui.BeginPopup(owner, cfg) / ui.Popup(owner, cfg, fn)
    // -----------------------------------------------------------------
    // Parented under the top-level overlay root instead of under `owner`, so
    // the popup is never clipped by an ancestor panel or scrolled out of a
    // viewport. Anchored at the owner's last laid-out rect, which means a
    // popup follows its owner with a one-frame delay — the price of building
    // the tree before the layout pass runs.
    template <typename Fn>
        requires std::invocable<Fn>
    auto Popup(Entity owner, const PopupConfig& cfg, Fn&& content) -> Entity {
        UIScope scope = BeginPopup(owner, cfg);
        Entity  ent   = scope.GetEntity();
        std::forward<Fn>(content)();
        return ent;
    }

    [[nodiscard]] auto BeginPopup(Entity owner, const PopupConfig& cfg = {}) -> UIScope {
        return BeginPopupImpl(owner, cfg);
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
    // (Public API: see Context::IsHovered above.)

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

    // --- OVERLAY / POPUP PLUMBING ---

    // The single top-level entity every popup, dropdown menu and tooltip is
    // parented to. It has NO UIFlexComponent on purpose: that keeps its
    // children on the anchor/offset positioning path, so a popup's x/y are
    // literal screen coordinates instead of a slot in a flex column. Its depth
    // (UI_OVERLAY_DEPTH) puts it above every widget in both the render order
    // and the hit-test order, and having no parent means the renderer's
    // scissor propagation never reaches it — which is exactly the "escape the
    // ancestor scissors" property the overlay exists for.
    auto GetOrCreateOverlayRoot() -> Entity {
        if (m_overlayRoot != Entity::Null() && m_reg->IsAlive(m_overlayRoot)) {
            return m_overlayRoot;
        }

        static constexpr std::string_view kOverlayName = "__ui_overlay_root__";
        Entity cache = GetRootCacheEntity();
        m_overlayRoot = GetOrCreateChild(cache, HashStringView(kOverlayName), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(kOverlayName)},
                Components::UIRectComponent {
                    .parentEntity   = Entity::Null(),
                    .width          = 0.0f,
                    .height         = 0.0f,
                    .hierarchyDepth = UI_OVERLAY_DEPTH,
                    .clipChildren   = false
                }
            );
        });
        m_reg->Patch<Components::UIRectComponent>(m_overlayRoot, [&](auto& r) -> auto {
            r.parentEntity   = Entity::Null();
            r.hierarchyDepth = UI_OVERLAY_DEPTH;
            r.clipChildren   = false;
        });
        return m_overlayRoot;
    }

    // Read the owner's last laid-out rect. Popup geometry is necessarily one
    // frame behind its owner: the tree is built before the layout pass runs,
    // so this frame's positions do not exist yet.
    struct OwnerAnchor {
        float x      = 0.0f;
        float y      = 0.0f;
        float width  = 0.0f;
        float height = 0.0f;
        bool  valid  = false;
    };

    [[nodiscard]] auto GetOwnerAnchor(Entity owner) const noexcept -> OwnerAnchor {
        OwnerAnchor a {};
        if (const auto* r = m_reg->Get<Components::UIRectComponent>(owner)) {
            a.x      = r->computedAbsMinX;
            a.y      = r->computedAbsMinY;
            a.width  = r->computedAbsMaxX - r->computedAbsMinX;
            a.height = r->computedAbsMaxY - r->computedAbsMinY;
            a.valid  = (a.width > 0.0f) || (a.height > 0.0f);
        }
        return a;
    }

    // Pointer position, when the engine has published one this frame. Falls
    // back to (0,0) — and reports that it did — so headless tests still get a
    // deterministic anchor instead of a tooltip pinned to the screen corner.
    struct PointerPosition {
        float x       = 0.0f;
        float y       = 0.0f;
        bool  present = false;
    };

    [[nodiscard]] auto GetPointerPosition() const noexcept -> PointerPosition {
        if (const auto* input = m_reg->GetSingleton<Components::InputStateComponent>()) {
            return {.x = input->mouseX, .y = input->mouseY, .present = true};
        }
        return {};
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

    // --- SELECTABLE / TREENODE PLUMBING ---

    struct SelectableClickInfo {
        Entity entity        = Entity::Null();
        bool   clicked       = false;
        bool   doubleClicked = false;
        bool   selected      = false;
    };

    // Builds (or refreshes) one list row and reports what the pointer did to
    // it this frame. Shared by Selectable (no arrow) and TreeNode (arrow), so
    // the two can never drift in how they read click state. `arrowGlyph` is
    // ignored unless `hasArrow`.
    auto PrepareSelectable(
        std::string_view       id,
        std::string_view       label,
        bool                   selectedIn,
        const SelectableConfig& cfg,
        std::string_view       arrowGlyph,
        bool                   hasArrow,
        uint32_t&              outDepth
    ) -> SelectableClickInfo {
        Entity   parent = GetCurrentParent();
        uint32_t depth  = GetCurrentDepth();
        uint64_t key    = HashCombine(parent.Pack(), HashStringView(id));

        const TextureHandle fontHandle = ResolveFontTexture();
        const float         padLeft    = 8.0f + std::max(0.0f, cfg.indent);

        Entity e = GetOrCreateEntity(key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64(id)},
                Components::UIRectComponent {.parentEntity = parent, .width = cfg.width, .height = cfg.height, .hierarchyDepth = depth},
                Components::UIPanelComponent {.color = cfg.normalColor, .borderRadius = cfg.borderRadius},
                Components::UIFlexComponent {
                    .direction     = FlexDirection::Row,
                    .alignItems    = FlexAlign::Center,
                    .flexGrow      = 1.0f,
                    .flexShrink    = 1.0f,
                    .flexBasis     = -1.0f,
                    .paddingLeft   = padLeft,
                    .paddingRight  = 8.0f,
                    .gapX          = 6.0f
                },
                Components::UIButtonComponent {},
                Components::UISelectableComponent {.selected = selectedIn, .doubleClickSpan = cfg.doubleClickSpan}
            );
        });

        m_reg->Patch<Components::UIRectComponent>(e, [&](auto& r) -> auto {
            r.parentEntity   = parent;
            r.width          = cfg.width;
            r.height         = cfg.height;
            r.hierarchyDepth = depth;
        });
        m_reg->Patch<Components::UIFlexComponent>(e, [&](auto& f) -> auto {
            f.direction    = FlexDirection::Row;
            f.alignItems   = FlexAlign::Center;
            f.flexGrow     = 1.0f;
            f.flexShrink   = 1.0f;
            f.flexBasis    = -1.0f;
            f.paddingLeft  = padLeft;
            f.paddingRight = 8.0f;
            f.gapX         = 6.0f;
        });

        auto* sel = m_reg->Get<Components::UISelectableComponent>(e);

        SelectableClickInfo info {.entity = e, .selected = sel->selected};

        if (ConsumeClick(e)) {
            // Double-click is a pair of clicks inside a frame window. The
            // window is counted in frames because the context is fed a frame
            // counter, which keeps the gesture reproducible in tests.
            const bool secondClick =
                (sel->lastClickFrame != 0) && (m_currentFrame > sel->lastClickFrame) && ((m_currentFrame - sel->lastClickFrame) <= sel->doubleClickSpan);
            if (secondClick) {
                sel->lastClickFrame = 0;
                info.doubleClicked  = true;
            } else {
                sel->lastClickFrame = m_currentFrame;
            }
            sel->selected = !sel->selected;
            info.clicked  = true;
        } else if (selectedIn != sel->selected) {
            // The caller mutated the bound bool outside a click: accept it.
            sel->selected = selectedIn;
        }
        sel->doubleClicked = info.doubleClicked;
        info.selected      = sel->selected;

        // Row children. The arrow is created BEFORE the label so it precedes
        // it in the Yoga child order (which follows registry creation order).
        if (hasArrow) {
            Entity arrowEnt = GetOrCreateChild(e, HashStringView("_sel_arrow"), [&]() -> Entity {
                return m_reg->Create(
                    Components::NameComponent {.name = String64("_sel_arrow")},
                    Components::UIRectComponent {.parentEntity = e, .width = 14.0f, .height = cfg.height, .hierarchyDepth = depth + 1},
                    Components::TextComponent {
                        .text          = String256(arrowGlyph),
                        .scale         = cfg.scale,
                        .color         = cfg.textColor,
                        .align         = TextAlignment::Left,
                        .verticalAlign = TextVerticalAlignment::Center,
                        .fontIndex     = fontHandle
                    }
                );
            });
            m_reg->Patch<Components::UIRectComponent>(arrowEnt, [&](auto& r) -> auto {
                r.parentEntity   = e;
                r.width          = 14.0f;
                r.height         = cfg.height;
                r.hierarchyDepth = depth + 1;
            });
            PatchSelectableArrow(e, arrowGlyph, cfg.selectedTextColor);
        }

        Entity lblEnt = GetOrCreateChild(e, HashStringView("_sel_label"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_sel_label")},
                Components::UIRectComponent {.parentEntity = e, .height = cfg.height, .hierarchyDepth = depth + 1},
                Components::TextComponent {
                    .text          = String256(label),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = cfg.align,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = fontHandle
                },
                Components::UIFlexComponent {.flexGrow = 1.0f}
            );
        });
        m_reg->Patch<Components::UIRectComponent>(lblEnt, [&](auto& r) -> auto {
            r.parentEntity   = e;
            r.height         = cfg.height;
            r.hierarchyDepth = depth + 1;
        });

        m_reg->Patch<Components::TextComponent>(lblEnt, [&](auto& tc) -> auto {
            tc.text.assign(label);
            tc.scale = cfg.scale;
            tc.align = cfg.align;
        });
        ApplySelectableVisual(e, cfg, sel->selected);

        outDepth = depth;
        return info;
    }

    // The single place a list row's colours are decided, so a later re-sync
    // can never drift from the look the row was built with.
    void ApplySelectableVisual(Entity row, const SelectableConfig& cfg, bool selected) {
        const bool hovered = IsHovered(row);
        const bool active  = selected && hovered;
        m_reg->Patch<Components::UIPanelComponent>(row, [&](auto& pc) -> auto {
            pc.color        = active ? cfg.activeColor : (selected ? cfg.selectedColor : (hovered ? cfg.hoverColor : cfg.normalColor));
            pc.borderRadius = cfg.borderRadius;
        });
        if (Entity lbl = FindChildByKey(row, HashStringView("_sel_label")); lbl != Entity::Null()) {
            m_reg->Patch<Components::TextComponent>(lbl, [&](auto& tc) -> auto { tc.color = selected ? cfg.selectedTextColor : cfg.textColor; });
        }
    }

    // Forces a row's bound selection state AND repaints it. TreeNode needs
    // this: its double click skips the toggle, and leaving the row's own flag
    // flipped would desaturate the very branch it just activated.
    void ApplySelectableSelection(Entity row, const SelectableConfig& cfg, bool selected) {
        if (auto* sel = m_reg->Get<Components::UISelectableComponent>(row)) {
            sel->selected = selected;
        }
        ApplySelectableVisual(row, cfg, selected);
    }

    void PatchSelectableArrow(Entity rowEntity, std::string_view glyph, JPH::Vec4 color) {
        Entity arrowEnt = FindChildByKey(rowEntity, HashStringView("_sel_arrow"));
        if (arrowEnt == Entity::Null()) {
            return;
        }
        m_reg->Patch<Components::TextComponent>(arrowEnt, [&](auto& tc) -> auto {
            tc.text.assign(glyph);
            tc.color = color;
        });
    }

    // Shared TreeNode body (private: the public TreeNode overloads above are
    // the API). Keeping the click rules and the content-box lifecycle in one
    // place is what stops the plain and double-click forms from drifting.
    template <typename Fn, typename OnDoubleClickFn>
        requires std::invocable<Fn> && std::invocable<OnDoubleClickFn>
    auto TreeNodeCore(std::string_view id, std::string_view label, bool& open, Fn&& content, const TreeNodeConfig& cfg, OnDoubleClickFn&& onDoubleClick) -> Entity {
        uint32_t depth           = 0;
        SelectableClickInfo info = PrepareSelectable(id, label, open, cfg.row, open ? "v" : ">", true, depth);

        if (info.doubleClicked) {
            std::forward<OnDoubleClickFn>(onDoubleClick)();
        }
        // Only a click that is NOT the second half of a double click toggles:
        // otherwise a double click would open the branch and close it again
        // before the designer's activation callback ever ran.
        if (info.clicked && !info.doubleClicked) {
            open = !open;
        }
        // Keep the arrow in sync with the (possibly just toggled) state.
        PatchSelectableArrow(info.entity, open ? "v" : ">", cfg.arrowColor);
        // The row's highlight tracks `open`; on a double click the toggle was
        // deliberately skipped, so re-sync the flag PrepareSelectable flipped.
        ApplySelectableSelection(info.entity, cfg.row, open);

        if (open) {
            std::array<char, 64> boxNameBuf {};
            std::string_view     contentBoxName = FormatTo(boxNameBuf, "{}_children", id);
            BoxConfig            boxCfg;
            boxCfg.width     = 0.0f;
            boxCfg.height    = 0.0f;
            boxCfg.color     = {0.0f, 0.0f, 0.0f, 0.0f};
            boxCfg.direction = FlexDirection::Column;
            boxCfg.gap       = cfg.gap;
            boxCfg.padding   = 0.0f;
            boxCfg.margin    = 0.0f;

            UIScope scope = PushScope(info.entity, depth);
            auto    boxScope = Box(contentBoxName, boxCfg);
            Entity  boxEnt   = boxScope.GetEntity();
            m_reg->Patch<Components::UIRectComponent>(boxEnt, [&](auto& r) -> auto {
                r.parentEntity   = info.entity;
                r.hierarchyDepth = depth + 1;
            });
            m_reg->Patch<Components::UIFlexComponent>(boxEnt, [&](auto& f) -> auto {
                f.direction   = FlexDirection::Column;
                f.marginLeft  = cfg.indent;
                f.marginTop   = 0.0f;
                f.marginRight = 0.0f;
                f.gapY        = cfg.gap;
            });
            std::forward<Fn>(content)();
            return info.entity;
        }

        // Closed: reclaim last frame's children subtree.
        SweepStaleChildren(info.entity);
        return info.entity;
    }

    // --- SCROLLBAR CHROME ---

    // Track + thumb beside the viewport. The thumb's travel comes from the
    // previous frame's measured content extent, so it converges one frame
    // after the content changes size — the same lag the scroll offset has.
    void UpdateScrollbar(Entity root, Entity viewport, const ScrollBoxConfig& cfg, uint32_t depth) {
        if (cfg.scrollbarWidth <= 0.0f) {
            return;
        }

        Entity track = GetOrCreateChild(root, HashStringView("_sb_track"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_sb_track")},
                Components::UIRectComponent {.parentEntity = root, .width = cfg.scrollbarWidth, .height = 0.0f, .hierarchyDepth = depth + 1},
                Components::UIPanelComponent {.color = cfg.trackColor},
                Components::UIFlexComponent {.direction = FlexDirection::Column, .flexGrow = 0.0f, .flexShrink = 0.0f, .flexBasis = cfg.scrollbarWidth}
            );
        });
        m_reg->Patch<Components::UIRectComponent>(track, [&](auto& r) -> auto {
            r.parentEntity   = root;
            r.width          = cfg.scrollbarWidth;
            r.height         = 0.0f;
            r.hierarchyDepth = depth + 1;
        });
        m_reg->Patch<Components::UIFlexComponent>(track, [&](auto& f) -> auto {
            f.direction  = FlexDirection::Column;
            f.flexGrow   = 0.0f;
            f.flexShrink = 0.0f;
            f.flexBasis  = cfg.scrollbarWidth;
        });
        m_reg->Patch<Components::UIPanelComponent>(track, [&](auto& pc) -> auto { pc.color = cfg.trackColor; });

        const auto* scroll = m_reg->Get<Components::UIScrollComponent>(viewport);
        const auto* vrect  = m_reg->Get<Components::UIRectComponent>(viewport);
        const auto* trect  = m_reg->Get<Components::UIRectComponent>(track);

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
                Components::UIRectComponent {.parentEntity = track, .width = cfg.scrollbarWidth, .height = thumbHeight, .hierarchyDepth = depth + 2},
                Components::UIPanelComponent {.color = cfg.thumbColor, .borderRadius = {cfg.scrollbarWidth * 0.5f, cfg.scrollbarWidth * 0.5f, cfg.scrollbarWidth * 0.5f, cfg.scrollbarWidth * 0.5f}},
                Components::UIFlexComponent {.flexGrow = 0.0f, .flexShrink = 0.0f, .flexBasis = -1.0f},
                Components::UIButtonComponent {}
            );
        });
        m_reg->Patch<Components::UIRectComponent>(thumb, [&](auto& r) -> auto {
            r.parentEntity   = track;
            r.width          = cfg.scrollbarWidth;
            r.height         = thumbHeight;
            r.hierarchyDepth = depth + 2;
        });
        m_reg->Patch<Components::UIFlexComponent>(thumb, [&](auto& f) -> auto {
            f.flexGrow   = 0.0f;
            f.flexShrink = 0.0f;
            f.flexBasis  = -1.0f;
            f.marginTop  = fraction * travel;
        });
        m_reg->Patch<Components::UIPanelComponent>(thumb, [&](auto& pc) -> auto {
            pc.color = hovered ? cfg.thumbHoverColor : cfg.thumbColor;
        });
    }

    // --- POPUP / TOOLTIP IMPLEMENTATIONS ---

    [[nodiscard]] auto BeginPopupImpl(Entity owner, const PopupConfig& cfg) -> UIScope {
        Entity   overlay = GetOrCreateOverlayRoot();
        uint64_t key     = HashCombine(owner.Pack(), HashStringView("__popup__"));

        const OwnerAnchor anchor = GetOwnerAnchor(owner);

        // Flip-above needs the popup's height, which only exists once the
        // popup has been laid out at least once; until then it opens downward.
        float lastHeight = 0.0f;
        if (Entity prev = FindChildByKey(overlay, key); prev != Entity::Null()) {
            if (const auto* pr = m_reg->Get<Components::UIRectComponent>(prev)) {
                lastHeight = pr->computedAbsMaxY - pr->computedAbsMinY;
            }
        }

        const float popupWidth = (cfg.width > 0.0f) ? cfg.width : std::max(anchor.width, 120.0f);
        const float popupX     = anchor.x;
        const float popupY     = cfg.openUpward ? std::max(0.0f, anchor.y - lastHeight) : (anchor.y + anchor.height);

        Entity popup = GetOrCreateChild(overlay, key, [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ui_popup")},
                Components::UIRectComponent {
                    .parentEntity   = overlay,
                    .x              = popupX,
                    .y              = popupY,
                    .width          = popupWidth,
                    .height         = cfg.height,
                    .hierarchyDepth = UI_OVERLAY_DEPTH + 1,
                    .clipChildren   = false
                },
                Components::UIPanelComponent {.color = cfg.bgColor, .borderRadius = cfg.borderRadius, .edgeWidth = 1.0f},
                Components::UIFlexComponent {
                    .direction     = FlexDirection::Column,
                    .alignItems    = FlexAlign::Stretch,
                    .paddingLeft   = cfg.padding,
                    .paddingTop    = cfg.padding,
                    .paddingRight  = cfg.padding,
                    .paddingBottom = cfg.padding,
                    .gapX          = cfg.gap,
                    .gapY          = cfg.gap
                },
                Components::UIPopupComponent {.owner = owner, .open = true}
            );
        });

        m_reg->Patch<Components::UIRectComponent>(popup, [&](auto& r) -> auto {
            r.parentEntity   = overlay;
            r.x              = popupX;
            r.y              = popupY;
            r.width          = popupWidth;
            r.height         = cfg.height;
            r.hierarchyDepth = UI_OVERLAY_DEPTH + 1;
            r.clipChildren   = false;
        });
        m_reg->Patch<Components::UIPanelComponent>(popup, [&](auto& pc) -> auto {
            pc.color        = cfg.bgColor;
            pc.borderRadius = cfg.borderRadius;
            pc.edgeWidth    = 1.0f;
        });
        m_reg->Patch<Components::UIPopupComponent>(popup, [&](auto& p) -> auto { p.owner = owner; });

        return PushScope(popup, UI_OVERLAY_DEPTH + 1);
    }

    auto TooltipForImpl(Entity owner, std::string_view text, const TooltipConfig& cfg) -> Entity {
        if (owner == Entity::Null() || !m_reg->IsAlive(owner) || text.empty()) {
            return Entity::Null();
        }

        // The hover clock lives on the owner (as a cache record carrying no
        // geometry), so the delay survives between frames without the Context
        // having to remember anything.
        Entity stateEnt = GetOrCreateChild(owner, HashStringView("_ui_tooltip"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ui_tooltip")},
                Components::UITooltipComponent {.text = String256(text), .delayFrames = cfg.delayFrames}
            );
        });
        auto* tip = m_reg->Get<Components::UITooltipComponent>(stateEnt);
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
                Components::UIRectComponent {
                    .parentEntity   = overlay,
                    .x              = baseX + cfg.offsetX,
                    .y              = baseY + cfg.offsetY,
                    .width          = boxWidth,
                    .height         = boxHeight,
                    .hierarchyDepth = UI_OVERLAY_DEPTH + 1,
                    .clipChildren   = false
                },
                Components::UIPanelComponent {.color = cfg.bgColor, .borderRadius = {3.0f, 3.0f, 3.0f, 3.0f}, .edgeWidth = 1.0f},
                Components::UIFlexComponent {
                    .direction     = FlexDirection::Column,
                    .paddingLeft   = cfg.padding,
                    .paddingTop    = cfg.padding,
                    .paddingRight  = cfg.padding,
                    .paddingBottom = cfg.padding
                },
                Components::UIPopupComponent {.owner = owner, .open = true}
            );
        });

        m_reg->Patch<Components::UIRectComponent>(box, [&](auto& r) -> auto {
            r.parentEntity   = overlay;
            r.x              = baseX + cfg.offsetX;
            r.y              = baseY + cfg.offsetY;
            r.width          = boxWidth;
            r.height         = boxHeight;
            r.hierarchyDepth = UI_OVERLAY_DEPTH + 1;
        });
        m_reg->Patch<Components::UIPanelComponent>(box, [&](auto& pc) -> auto { pc.color = cfg.bgColor; });

        Entity textEnt = GetOrCreateChild(box, HashStringView("_ui_tooltip_text"), [&]() -> Entity {
            return m_reg->Create(
                Components::NameComponent {.name = String64("_ui_tooltip_text")},
                Components::UIRectComponent {.parentEntity = box, .width = 0.0f, .height = 0.0f, .hierarchyDepth = UI_OVERLAY_DEPTH + 2},
                Components::TextComponent {
                    .text          = String256(text),
                    .scale         = cfg.scale,
                    .color         = cfg.textColor,
                    .align         = TextAlignment::Left,
                    .verticalAlign = TextVerticalAlignment::Center,
                    .fontIndex     = fontHandle,
                    .wrapText      = (innerMax > 0.0f),
                    .wrapWidth     = innerMax
                },
                Components::UIFlexComponent {.flexGrow = 1.0f}
            );
        });
        m_reg->Patch<Components::TextComponent>(textEnt, [&](auto& tc) -> auto {
            tc.text.assign(text);
            tc.color    = cfg.textColor;
            tc.scale    = cfg.scale;
            tc.wrapText = (innerMax > 0.0f);
            tc.wrapWidth = innerMax;
        });

        return box;
    }

    [[nodiscard]] auto ResolveFontAtlas() const noexcept -> const FontAtlas* {
        const auto uiSettings = m_reg->GetEntitiesWith<Components::UISettingsComponent>();
        if (!uiSettings.empty()) {
            return &m_reg->Get<Components::UISettingsComponent>(uiSettings[0])->fontAtlas;
        }
        return nullptr;
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
    Entity                                      m_overlayRoot     = Entity::Null();
    Entity                                      m_lastItem        = Entity::Null();
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
// SCROLLING — CONTENT MEASUREMENT AND WHEEL DISPATCH
// ============================================================================
// Free functions rather than Context members: scrolling is per-frame system
// work over the whole registry, not widget construction, and keeping it here
// means the interaction system and the tests drive exactly the same code.

/// Per-frame pointer snapshot for ApplyScrollInput. A plain struct so nothing
/// outside the engine needs an Engine to drive scrolling.
struct ScrollInput {
    float mouseX     = 0.0f;
    float mouseY     = 0.0f;
    float wheelDelta = 0.0f;
    float deltaTime  = 1.0f / 60.0f;
};

/// True when the point lies inside `ent`'s rect AND inside every ancestor that
/// clips its children. Without the ancestor test a row scrolled out of a
/// ScrollBox viewport would still swallow hover and clicks — the clipped part
/// of a scroller must be inert, not just invisible.
[[nodiscard]] inline auto IsPointVisible(ECS::Registry& reg, Entity ent, float x, float y) noexcept -> bool {
    auto Inside = [](const Components::UIRectComponent& r, float px, float py) -> bool {
        return px >= r.computedAbsMinX && px <= r.computedAbsMaxX && py >= r.computedAbsMinY && py <= r.computedAbsMaxY;
    };

    const auto* rect = reg.Get<Components::UIRectComponent>(ent);
    if (rect == nullptr || !Inside(*rect, x, y)) {
        return false;
    }

    Entity curr = rect->parentEntity;
    while (curr != Entity::Null() && reg.IsAlive(curr)) {
        const auto* parent = reg.Get<Components::UIRectComponent>(curr);
        if (parent == nullptr) {
            break;
        }
        if (parent->clipChildren && !Inside(*parent, x, y)) {
            return false;
        }
        curr = parent->parentEntity;
    }
    return true;
}

/// Measures every scroll viewport's content extent from its laid-out children
/// (padding included) and clamps the live offsets into the resulting range.
///
/// Runs after the layout readback: the children's absolute rects are already
/// offset by the current scroll delta, but a uniform shift preserves the
/// extent, so the measurement is scroll-invariant.
inline void UpdateScrollExtents(ECS::Registry& reg) noexcept {
    struct Extent {
        float    minTop    = 0.0f;
        float    maxBottom = 0.0f;
        float    minLeft   = 0.0f;
        float    maxRight  = 0.0f;
        uint32_t count     = 0;
    };

    HashMap<uint64_t, Extent> extents;

    const auto entities = reg.GetEntitiesWith<Components::UIRectComponent>();
    const auto rects    = reg.GetRawArray<Components::UIRectComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        const auto& r = rects[i];
        if (r.parentEntity == Entity::Null() || !reg.IsAlive(r.parentEntity)) {
            continue;
        }
        if (reg.Get<Components::UIScrollComponent>(r.parentEntity) == nullptr) {
            continue;
        }

        const uint64_t key = r.parentEntity.Pack();
        Extent*        ex  = extents.Find(key);
        if (ex == nullptr) {
            extents.Insert(key, Extent {});
            ex = extents.Find(key); // Insert may have rehashed
        }
        if (ex == nullptr) {
            continue;
        }

        if (ex->count == 0) {
            ex->minTop    = r.computedAbsMinY;
            ex->maxBottom = r.computedAbsMaxY;
            ex->minLeft   = r.computedAbsMinX;
            ex->maxRight  = r.computedAbsMaxX;
        } else {
            ex->minTop    = std::min(ex->minTop, r.computedAbsMinY);
            ex->maxBottom = std::max(ex->maxBottom, r.computedAbsMaxY);
            ex->minLeft   = std::min(ex->minLeft, r.computedAbsMinX);
            ex->maxRight  = std::max(ex->maxRight, r.computedAbsMaxX);
        }
        ++ex->count;
    }

    for (Entity sc: reg.GetEntitiesWith<Components::UIScrollComponent>()) {
        auto* scroll = reg.Get<Components::UIScrollComponent>(sc);
        const auto* rect = reg.Get<Components::UIRectComponent>(sc);
        if (scroll == nullptr || rect == nullptr) {
            continue;
        }

        const auto* flex = reg.Get<Components::UIFlexComponent>(sc);
        const float padL = (flex != nullptr) ? flex->paddingLeft : 0.0f;
        const float padT = (flex != nullptr) ? flex->paddingTop : 0.0f;
        const float padR = (flex != nullptr) ? flex->paddingRight : 0.0f;
        const float padB = (flex != nullptr) ? flex->paddingBottom : 0.0f;

        const float viewWidth  = rect->computedAbsMaxX - rect->computedAbsMinX;
        const float viewHeight = rect->computedAbsMaxY - rect->computedAbsMinY;

        // Top-left of the content box, un-shifted by the current offset.
        const float contentTop  = rect->computedAbsMinY + padT - scroll->scrollY;
        const float contentLeft = rect->computedAbsMinX + padL - scroll->scrollX;

        float contentWidth  = 0.0f;
        float contentHeight = 0.0f;
        if (const Extent* ex = extents.Find(sc.Pack()); ex != nullptr && ex->count > 0) {
            contentWidth  = std::max(0.0f, ex->maxRight - contentLeft) + padR;
            contentHeight = std::max(0.0f, ex->maxBottom - contentTop) + padB;
        }

        scroll->contentWidth  = contentWidth;
        scroll->contentHeight = contentHeight;
        scroll->maxScrollY    = std::max(0.0f, contentHeight - viewHeight);
        scroll->maxScrollX    = scroll->allowHorizontal ? std::max(0.0f, contentWidth - viewWidth) : 0.0f;

        scroll->scrollY       = std::clamp(scroll->scrollY, 0.0f, scroll->maxScrollY);
        scroll->targetScrollY = std::clamp(scroll->targetScrollY, 0.0f, scroll->maxScrollY);
        scroll->scrollX       = std::clamp(scroll->scrollX, 0.0f, scroll->maxScrollX);
        scroll->targetScrollX = std::clamp(scroll->targetScrollX, 0.0f, scroll->maxScrollX);
    }
}

/// Feeds the wheel to the innermost scrollable under the pointer and eases
/// every viewport towards its target. Returns true when the UI consumed the
/// wheel, so camera zoom or page scroll layered underneath can back off.
inline auto ApplyScrollInput(ECS::Registry& reg, const ScrollInput& input) noexcept -> bool {
    bool consumed = false;

    if (std::abs(input.wheelDelta) > 0.001f) {
        Entity   best       = Entity::Null();
        uint32_t bestDepth  = 0;

        for (Entity sc: reg.GetEntitiesWith<Components::UIScrollComponent>()) {
            const auto* scroll = reg.Get<Components::UIScrollComponent>(sc);
            if (scroll == nullptr) {
                continue;
            }
            const bool canScroll = (scroll->maxScrollY > 0.0f) || (scroll->allowHorizontal && scroll->maxScrollX > 0.0f);
            if (!canScroll || !IsPointVisible(reg, sc, input.mouseX, input.mouseY)) {
                continue;
            }
            const auto*    rect  = reg.Get<Components::UIRectComponent>(sc);
            const uint32_t depth = (rect != nullptr) ? rect->hierarchyDepth : 0;
            if (best == Entity::Null() || depth > bestDepth) {
                best      = sc;
                bestDepth = depth;
            }
        }

        if (best != Entity::Null()) {
            auto* scroll = reg.Get<Components::UIScrollComponent>(best);
            // Wheel up (positive) scrolls the content down, i.e. the offset
            // shrinks — the same sign convention as a browser viewport.
            const float delta = input.wheelDelta * scroll->scrollSpeed;
            if (scroll->maxScrollY > 0.0f) {
                scroll->targetScrollY = std::clamp(scroll->targetScrollY - delta, 0.0f, scroll->maxScrollY);
            } else if (scroll->allowHorizontal) {
                scroll->targetScrollX = std::clamp(scroll->targetScrollX - delta, 0.0f, scroll->maxScrollX);
            }
            consumed = true;
        }
    }

    for (Entity sc: reg.GetEntitiesWith<Components::UIScrollComponent>()) {
        auto* scroll = reg.Get<Components::UIScrollComponent>(sc);
        if (scroll == nullptr) {
            continue;
        }

        if (!scroll->smoothScroll) {
            scroll->scrollX = scroll->targetScrollX;
            scroll->scrollY = scroll->targetScrollY;
            continue;
        }

        const float k = std::clamp(scroll->smoothSpeed * input.deltaTime, 0.0f, 1.0f);
        scroll->scrollX += (scroll->targetScrollX - scroll->scrollX) * k;
        scroll->scrollY += (scroll->targetScrollY - scroll->scrollY) * k;

        // Snap the tail of the easing curve: without this the offset chases
        // its target forever and the scissor rect never stops changing.
        if (std::abs(scroll->targetScrollX - scroll->scrollX) < 0.05f) {
            scroll->scrollX = scroll->targetScrollX;
        }
        if (std::abs(scroll->targetScrollY - scroll->scrollY) < 0.05f) {
            scroll->scrollY = scroll->targetScrollY;
        }
    }

    return consumed;
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
static_assert(std::is_same_v<decltype(std::declval<Context&>().ScrollBox("a", ScrollBoxConfig {}, []() {})), Entity>);
static_assert(std::is_same_v<decltype(std::declval<Context&>().BeginScrollBox("a")), UIScope>);
static_assert(std::is_same_v<decltype(std::declval<Context&>().Popup(Entity::Null(), PopupConfig {}, []() {})), Entity>);
static_assert(std::is_same_v<decltype(std::declval<Context&>().BeginPopup(Entity::Null())), UIScope>);
static_assert(std::is_same_v<decltype(std::declval<Context&>().TreeNode("a", "a", std::declval<bool&>(), []() {})), Entity>);
static_assert(std::is_same_v<decltype(std::declval<Context&>().TreeNode("a", "a", std::declval<bool&>(), []() {}, TreeNodeConfig {}, []() {})), Entity>);

} // namespace ZHLN::GUI
