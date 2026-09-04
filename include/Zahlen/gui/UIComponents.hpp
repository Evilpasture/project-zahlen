// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/gui/UIComponents.hpp
#pragma once
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Types.hpp>

namespace ZHLN::ECS {
class Registry;
}

namespace ZHLN::GUI {

// Bit flags for UIButtonComponent::flags, so a button can be hovered and
// clicked in the same frame. UIButton has to opt in to the operators, and that
// specialization cannot sit inside ZHLN::GUI -- see the ZHLN block below.
enum class UIButton : uint8_t { None = 0, Hovered = 1 << 0, Pressed = 1 << 1, Clicked = 1 << 2, Disabled = 1 << 3 };

} // namespace ZHLN::GUI

namespace ZHLN {
// EnableEnumFlags is declared in Zahlen/Types.hpp at ZHLN scope, and an
// explicit specialization may only be declared in a namespace enclosing the
// primary template -- which ZHLN::GUI, being nested, is not. Hence the close
// and reopen: the opt-in has to be at ZHLN scope even though the enum is not.
template <>
inline constexpr bool EnableEnumFlags<GUI::UIButton> = true;
} // namespace ZHLN

namespace ZHLN::GUI {

enum class StackDirection : uint8_t { Horizontal = 0, Vertical = 1 };
enum class TextAlignment : uint8_t { Left = 0, Center = 1, Right = 2 };
enum class TextVerticalAlignment : uint8_t { Top = 0, Center = 1, Bottom = 2 };
enum class UIJustify : uint8_t { Start = 0, Center = 1, End = 2, SpaceBetween = 3, SpaceAround = 4 };

enum class FlexDirection : uint8_t { Column = 0, ColumnReverse, Row, RowReverse };
enum class FlexWrap : uint8_t { NoWrap = 0, Wrap, WrapReverse };
enum class FlexJustify : uint8_t { FlexStart = 0, Center, FlexEnd, SpaceBetween, SpaceAround, SpaceEvenly };
enum class FlexAlign : uint8_t { Auto = 0, FlexStart, Center, FlexEnd, Stretch, Baseline };

// How a UI image maps its source region onto the widget's laid-out rect.
//  * Stretch    — ignore the source aspect ratio, fill the rect exactly.
//  * FitAspect  — keep the aspect ratio, scale to fit INSIDE the rect
//                 (letterboxed; the quad is centred and smaller than the rect).
//  * CropAspect — keep the aspect ratio, scale to COVER the rect and crop the
//                 overflow by insetting the UV region.
//  * Tile       — repeat the source region at its native pixel size until the
//                 rect is filled (edge tiles are cropped).
enum class ImageScaleMode : uint8_t { Stretch = 0, FitAspect = 1, CropAspect = 2, Tile = 3 };

// Which way a UIGradientComponent runs its colour stops across the widget.
// Horizontal runs left (first stop) to right (last); Vertical runs top to
// bottom.
enum class UIGradientAxis : uint8_t { Horizontal = 0, Vertical = 1 };

// What a UIPlotComponent draws from its series.
//  * Lines       — a polyline through the samples, each segment a thin quad.
//  * Histogram   — one bar per sample, rising from the zero line (or from the
//                  bottom of the range when the range never crosses zero).
//  * ShadedLines — the polyline plus a translucent area down to the baseline.
enum class UIPlotKind : uint8_t { Lines = 0, Histogram = 1, ShadedLines = 2 };

/// The GUI subsystem's component set.
///
/// Deliberately shaped like ZHLN::Components: a flat namespace of nested
/// POD structs, no inheritance and no virtuals. That is what lets
/// Reflect::ForEachNestedType walk it, so GUI::RegisterUIComponents can
/// register the whole set with one call and a new widget component needs
/// no edit outside this header.
struct UIComponents {
    struct TextComponent {
        ZHLN::String256 text;
        float           scale = 1.0f;
        JPH::Vec4       color = {1.0f, 1.0f, 1.0f, 1.0f};

        TextAlignment         align         = TextAlignment::Left;
        TextVerticalAlignment verticalAlign = TextVerticalAlignment::Top;

        TextureHandle fontIndex = TextureHandle::Invalid;
        float         offsetX   = 0.0f;
        float         offsetY   = 0.0f;

        // Automatic word wrapping. When set, the layout measure function wraps
        // the text at the width Yoga offers (or `wrapWidth` when non-zero) and
        // the renderer breaks the same lines, so measurement and drawing can
        // never disagree. Off by default: single-line labels keep their old
        // "one line, no reflow" behaviour.
        bool wrapText   = false;
        float wrapWidth = 0.0f; // 0 = wrap at the laid-out container width
        char _pad[2]    = {};
    };

    struct UIChildCacheComponent {
        struct ChildRecord {
            Entity           entity           = Entity::Null();
            mutable uint64_t lastVisitedFrame = 0;
        };
        HashMap<uint64_t, ChildRecord> children;
    };

    struct UIStackComponent {
        float          spacing   = 8.0f;
        float          padding   = 8.0f;
        StackDirection direction = StackDirection::Vertical;
        UIJustify      justify   = UIJustify::Start;
    };

    struct UIFlexComponent {
        FlexDirection direction  = FlexDirection::Column;
        FlexJustify   justify    = FlexJustify::FlexStart;
        FlexAlign     alignItems = FlexAlign::Stretch;
        FlexAlign     alignSelf  = FlexAlign::Auto;
        FlexWrap      wrap       = FlexWrap::NoWrap;

        float flexGrow   = 0.0f;
        float flexShrink = 1.0f;
        float flexBasis  = -1.0f;

        float paddingLeft = 0.0f, paddingTop = 0.0f, paddingRight = 0.0f, paddingBottom = 0.0f;
        float marginLeft = 0.0f, marginTop = 0.0f, marginRight = 0.0f, marginBottom = 0.0f;
        float gapX = 0.0f, gapY = 0.0f;

        void SetPadding(float p) noexcept {
            paddingLeft = paddingTop = paddingRight = paddingBottom = p;
        }
        void SetMargin(float m) noexcept {
            marginLeft = marginTop = marginRight = marginBottom = m;
        }
        void SetGap(float g) noexcept {
            gapX = gapY = g;
        }
    };

    struct UISettingsComponent {
        TextureHandle defaultFontAtlas = TextureHandle::Invalid;
        FontAtlas     fontAtlas;
        // Monotonic creation-stamp source for UIRectComponent::layoutOrder.
        // Lives in the REGISTRY (not in GUI::Context, which is rebuilt every
        // frame): a per-context counter restarted at 1 each frame, so a widget
        // recreated after a collapse got a SMALLER order than its surviving
        // siblings and jumped above them -- sections "dropping up" on reopen.
        uint32_t      nextLayoutOrder = 1;
    };

    struct UIRectComponent {
        ZHLN::Entity parentEntity {};

        float x = 0.0f;
        float y = 0.0f;
        // 0 = auto: the size is derived by the flex/anchor layout instead of
        // being pinned. These used to default to 100, which silently froze
        // every widget built without an explicit `.width` (compound-widget
        // labels, collapsing headers, checkbox rows, ...) at exactly 100px, so
        // a 400px panel showed a 100px column of controls hugging its left
        // edge. UILayoutSystem only applies a size when it is > 0.
        float width  = 0.0f;
        float height = 0.0f;

        float anchorMinX = 0.0f;
        float anchorMinY = 0.0f;
        float anchorMaxX = 0.0f;
        float anchorMaxY = 0.0f;

        float computedAbsMinX = 0.0f;
        float computedAbsMinY = 0.0f;
        float computedAbsMaxX = 0.0f;
        float computedAbsMaxY = 0.0f;

        uint32_t hierarchyDepth = 0;
        // Monotonic creation stamp assigned by GUI::Context. The ECS dense-array
        // order reshuffles on every swap-remove destroy (collapsing a section
        // destroys a dozen entities), so it must never decide sibling order or
        // draw/hit-test layering: layout, render and interaction all sort by
        // (hierarchyDepth, layoutOrder) instead. 0 = not stamped (pre-GUI rect).
        uint32_t layoutOrder    = 0;
        bool     clipChildren   = false;
        char     _free_space[3] {};
    };

    // Marks the pane that shows the 3D world instead of chrome.
    //
    // UIInteractionSystem sets InputStateComponent::wantCaptureMouse when the
    // pointer is over a widget, which is what stops a click on a panel from
    // also orbiting the camera or picking into the scene. A viewport is the
    // exception: the world is supposed to receive the pointer there, so
    // hovering anywhere inside a tagged subtree leaves wantCaptureMouse clear
    // and the camera keeps working as if the pointer were over bare screen.
    //
    // This only suppresses *capture*. Widgets inside a viewport are still
    // hit-tested and still receive their own hover and clicks, so the overlays
    // drawn over the 3D view (gizmos, the Simulate toggle) remain usable. It
    // is an empty tag: the interaction pass looks for it by walking the parent
    // chain, so tagging the pane covers everything nested under it.
    struct UIViewportComponent {};

    struct UIPanelComponent {
        JPH::Vec4     color        = {1.0f, 1.0f, 1.0f, 1.0f};
        JPH::Vec4     borderRadius = {0.0f, 0.0f, 0.0f, 0.0f};
        TextureHandle texture      = TextureHandle::Invalid;
        float         edgeWidth    = 0.0f;
        float         uvLeft       = 0.1f;
        float         uvRight      = 0.1f;
        float         uvTop        = 0.1f;
        float         uvBottom     = 0.1f;
    };

    struct UIButtonComponent {
        UIButton flags = UIButton::None;

        void Set(UIButton flag, bool value) noexcept {
            if (value) {
                flags |= flag;
            } else {
                flags &= ~flag;
            }
        }

        [[nodiscard]] bool Has(UIButton flag) const noexcept {
            return (flags & flag) != UIButton::None;
        }
    };

    struct UIDragComponent {
        ZHLN::Entity targetEntity {};
        bool         isDragging = false;
    };

    struct UITextInputComponent {
        String256 text;
        uint32_t  cursorIndex = 0;
        bool      isFocused   = false;
        bool      edited      = false; // Set true by engine on text mutation; builder clears after reading
        // Set on the unfocused->focused transition: the pre-focus content is
        // "selected", so the first printable key REPLACES it ("Default" goes
        // away when you type) and Backspace deletes it wholesale. Any caret
        // movement (Left/Right) or commit clears it, matching how name fields
        // behave in tool UIs.
        bool      selectAll   = false;
        char      _pad[1]     = {};
    };

    struct UICheckboxComponent {
        bool checked       = false;
        bool previousValue = false; // For external-mutation detection
    };

    struct UISliderComponent {
        float value         = 0.0f;
        float minValue      = 0.0f;
        float maxValue      = 1.0f;
        float step          = 0.0f; // 0 = continuous
        float previousValue = 0.0f;
        bool  isDragging    = false;
        char  _pad[3]       = {};
    };

    struct UIDropdownComponent {
        int32_t  selectedIdx    = 0;
        int32_t  previousIdx    = 0;
        bool     expanded       = false;
        char     _pad[3]        = {};
        // Stored option strings (copied at build time so options are retained)
        ZHLN::Array<String128> options;
    };

    // The saturation/value plane of a colour picker: a 2D drag pad whose x is
    // saturation and whose y is value, tinted by `hue`.
    //
    // Lives on the pad itself, not on the ColorEdit root, because the
    // interaction pass needs a widget to hit-test and drag, and because the
    // pad is the one part of the picker that cannot be expressed as a 1D
    // slider. `hue` is stored here too: the pad is *painted* from it, and the
    // renderer only ever sees components, so the pad's own gradient stops have
    // to be derivable from the component alone.
    struct UIColorSVComponent {
        float hue        = 0.0f; // 0..360
        float sat        = 0.0f; // 0..1
        float val        = 1.0f; // 0..1
        bool  isDragging = false;
        char  _pad[3]    = {};
    };

    // A colour field: a swatch that opens a picker popup.
    //
    // `value` is the authoritative RGBA, in 0..1. `hue`/`sat`/`val` are the
    // HSV working copy the picker edits; they are re-derived from `value`
    // whenever `value` changes from the outside (an edited material, a preset,
    // an undo), and `value` is re-derived from them whenever the user drags.
    // The two-way sync is what keeps a colour that was set programmatically
    // from showing a stale picker position.
    //
    // HSV is lossy at the grey axis (hue is undefined when saturation is 0),
    // so the derivation keeps the previous hue for achromatic colours rather
    // than snapping it to 0. Without that, dragging saturation to zero and
    // back would silently recolour a grey to red.
    struct UIColorEditComponent {
        JPH::Vec4 value         = {1.0f, 1.0f, 1.0f, 1.0f};
        JPH::Vec4 previousValue = {1.0f, 1.0f, 1.0f, 1.0f};

        float hue = 0.0f;
        float sat = 0.0f;
        float val = 1.0f;

        // 3 = RGB (alpha hidden and forced to 1), 4 = RGBA.
        int32_t componentCount = 3;

        bool expanded  = false; // Picker popup open
        bool hsvStale  = true;  // Recompute HSV from `value` on the next build
        char _pad[2]   = {};
    };

    struct UICollapsingHeaderComponent {
        bool isOpen       = true;
        bool defaultOpen  = true;
        char _pad[2]      = {};
    };

    struct UISplitterComponent {
        enum Direction : uint8_t { Horizontal = 0, Vertical = 1 };
        float ratio         = 0.5f; // Size of the first panel (0..1)
        float previousRatio = 0.5f;
        bool  isDragging    = false;
        Direction direction = Horizontal;
    };

    // Scrollable viewport state. Lives on the ScrollBox's clipping viewport
    // entity; the layout pass measures the content extent and clamps the
    // offsets, the interaction pass integrates the wheel into `targetScroll*`
    // and eases `scroll*` towards it.
    struct UIScrollComponent {
        float scrollX       = 0.0f;
        float scrollY       = 0.0f;
        float targetScrollX = 0.0f;
        float targetScrollY = 0.0f;

        // Content extent in pixels, measured by the layout pass from the
        // viewport's laid-out children (padding and gaps included).
        float contentWidth  = 0.0f;
        float contentHeight = 0.0f;

        // Scrollable range: max(0, contentExtent - viewportExtent). Written by
        // the layout pass; the interaction pass clamps against it.
        float maxScrollX = 0.0f;
        float maxScrollY = 0.0f;

        float scrollSpeed = 35.0f;  // Pixels per wheel notch
        float smoothSpeed = 15.0f;  // Exponential easing rate (1/seconds)
        bool  smoothScroll = true;
        bool  allowHorizontal = false;
        char  _pad[2]         = {};
    };

    // A textured quad: an icon, a sprite, or a slice of a sprite atlas. The
    // renderer emits this instead of the plain panel quad whenever the entity
    // carries one, and the UV rectangle below selects the source region.
    struct UIImageComponent {
        TextureHandle  texture  = TextureHandle::Invalid;
        ImageScaleMode mode     = ImageScaleMode::Stretch;
        JPH::Vec4      tint     = {1.0f, 1.0f, 1.0f, 1.0f};

        // Sub-UV region inside the texture (sprite-sheet slice). (0,0)-(1,1)
        // is the whole texture.
        float uv0x = 0.0f;
        float uv0y = 0.0f;
        float uv1x = 1.0f;
        float uv1y = 1.0f;

        // Native size of the selected region in pixels. Required by
        // FitAspect/CropAspect (aspect source) and by Tile (repeat pitch).
        float sourceWidth  = 0.0f;
        float sourceHeight = 0.0f;
    };

    // A band of evenly spaced colour stops painted across the widget rect.
    //
    // This is the primitive behind a colour picker's hue strip and its
    // saturation/value plane, and behind any bar that needs a gradient fill.
    // The stops are inline and there are at most kMaxStops of them: a gradient
    // is a per-frame visual that a widget rewrites every time it is built, so
    // an allocating member here would churn the heap on every frame. Eight is
    // enough for a full hue loop (seven stops return to red) and for the
    // two-stop ramps everything else needs.
    //
    // Interpolation is per-vertex, so a gradient is only exact at its stops —
    // which is why the hue strip uses seven of them rather than two. Fewer
    // than two stops paints nothing.
    struct UIGradientComponent {
        static constexpr uint32_t kMaxStops = 8;

        UIGradientAxis axis      = UIGradientAxis::Horizontal;
        uint32_t       stopCount = 0; // < 2 paints nothing
        JPH::Vec4      stops[kMaxStops] = {
            JPH::Vec4(1.0f, 1.0f, 1.0f, 1.0f),
            JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f),
            JPH::Vec4(0.0f, 0.0f, 0.0f, 0.0f),
            JPH::Vec4(0.0f, 0.0f, 0.0f, 0.0f),
            JPH::Vec4(0.0f, 0.0f, 0.0f, 0.0f),
            JPH::Vec4(0.0f, 0.0f, 0.0f, 0.0f),
            JPH::Vec4(0.0f, 0.0f, 0.0f, 0.0f),
            JPH::Vec4(0.0f, 0.0f, 0.0f, 0.0f),
        };
    };

    // A rolling series drawn as lines, bars, or both: frame times, profiler
    // averages, memory history, audio meters.
    //
    // The samples live in `values` and are mapped across the widget's laid-out
    // width, with `minValue`/`maxValue` mapped to the bottom and top edges, so
    // a caller that owns a ring buffer can hand over a span each frame without
    // rescaling anything. Values outside the range are clamped, not dropped: a
    // spike that overshoots maxValue still draws, pinned to the top edge,
    // rather than punching a hole in the strip.
    struct UIPlotComponent {
        // Guard against an unbounded series: 512 samples is 3k vertices worst
        // case (ShadedLines), which one UI batch absorbs. Callers with longer
        // histories should downsample before handing them over.
        static constexpr uint32_t kMaxPoints = 512;

        ZHLN::Array<float> values;

        UIPlotKind kind     = UIPlotKind::Lines;
        float      minValue = 0.0f;
        float      maxValue = 1.0f;

        JPH::Vec4 lineColor = {0.30f, 0.78f, 1.00f, 1.00f};
        JPH::Vec4 fillColor = {0.30f, 0.78f, 1.00f, 0.35f};

        float lineWidth = 1.5f; // Lines / ShadedLines, in pixels
        float barGap    = 1.0f; // Histogram, in pixels between adjacent bars
    };

    // List/tree row state: selection plus the double-click bookkeeping. The
    // double-click window is counted in FRAMES (not seconds) because GUI::Context
    // is fed a frame counter, which keeps the gesture deterministic in tests.
    struct UISelectableComponent {
        bool     selected        = false;
        bool     doubleClicked   = false; // Consumed by the builder, cleared each frame
        uint64_t lastClickFrame  = 0;
        uint32_t doubleClickSpan = 18; // Frames allowed between the two clicks
        bool     _pad0           = false;
        char     _pad[2]         = {};
    };

    // Marks a subtree that is parented under the top-level overlay root instead
    // of under the widget that logically owns it, so popups escape every
    // ancestor scissor (a clipped panel, a scrolled viewport). `owner` is the
    // widget the popup belongs to; the interaction pass uses it to decide
    // whether a click landed "inside the dropdown" or outside it.
    struct UIPopupComponent {
        ZHLN::Entity owner = ZHLN::Entity::Null();
        bool         open  = true;
        char         _pad[3] = {};
    };

    // Hover-introspection state parked on the widget a tooltip describes. The
    // tooltip itself is transient overlay geometry; this component only
    // remembers how long the owner has been hovered so a delay can be applied.
    struct UITooltipComponent {
        String256  text;
        uint64_t   hoverStartFrame = 0; // 0 = not hovered last frame
        uint32_t   delayFrames     = 20;
        float      scale           = 0.80f;
        JPH::Vec4  bgColor         = {0.05f, 0.07f, 0.11f, 0.98f};
        JPH::Vec4  textColor       = {0.90f, 0.95f, 1.00f, 1.00f};
        JPH::Vec4  borderColor     = {0.26f, 0.38f, 0.58f, 1.00f};
        float      offsetX         = 14.0f;
        float      offsetY         = 18.0f;
    };

    struct UIStyleComponent {
        JPH::Vec4 normalColor   = {0.15f, 0.15f, 0.22f, 0.95f};
        JPH::Vec4 hoverColor    = {0.22f, 0.22f, 0.32f, 0.95f};
        JPH::Vec4 pressedColor  = {0.10f, 0.10f, 0.15f, 0.95f};
        JPH::Vec4 disabledColor = {0.08f, 0.08f, 0.10f, 0.50f};

        JPH::Vec4 textColorNormal  = {0.90f, 0.90f, 0.90f, 1.0f};
        JPH::Vec4 textColorHover   = {1.00f, 1.00f, 1.00f, 1.0f};
        JPH::Vec4 textColorPressed = {0.70f, 0.70f, 0.70f, 1.0f};

        float transitionSpeed = 18.0f;
        bool  hasTextColor    = false;
        char  _pad[3]         = {};
    };
};

/// Registers every component nested in UIComponents with `reg`.
///
/// The GUI subsystem owns these types, so it owns their registration: the
/// reflection walk lives in src/gui/UIRegistration.cpp rather than in the
/// engine's blanket ZHLN::Components sweep, and a new widget component needs
/// no edit outside this header. Registration is idempotent (re-registering a
/// type is a no-op), so a per-scene call is safe.
void RegisterUIComponents(ECS::Registry& reg);

} // namespace ZHLN::GUI
