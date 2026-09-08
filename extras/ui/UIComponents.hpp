// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/ui/UIComponents.hpp
//
// Retained ECS UI components. Immediate-mode Clay (`Zahlen/gui/GUI.hpp`) is
// the core toolkit; these PODs exist for extras that still want a widget
// tree in the registry (the native editor inspector, scripts that attach
// UIRect/UIPanel, ...).
//
// Parenting is `ZHLN::Components::HierarchyComponent`, the same link
// DespawnEntity already walks. Do not add a second parent field here.
//
// Deliberately shaped like ZHLN::Components: a flat namespace of nested
// POD structs, so Reflect::ForEachNestedType can walk the whole set with
// one call -- which is how the Lua bindings and the FFI cdef generator
// pick up every widget component without a per-type edit.
#pragma once
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Types.hpp>

namespace ZHLN::GUI {

enum class TextAlignment : uint8_t { Left = 0, Center = 1, Right = 2 };
enum class TextVerticalAlignment : uint8_t { Top = 0, Center = 1, Bottom = 2 };

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

        // Automatic word wrapping. When set, extras/ui wrap helpers wrap the
        // text at the width the layout offers (or `wrapWidth` when non-zero)
        // and the renderer breaks the same lines. Off by default.
        bool  wrapText  = false;
        float wrapWidth = 0.0f; // 0 = wrap at the laid-out container width
        char  _pad[2]   = {};
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

    struct UIRectComponent {
        float x = 0.0f;
        float y = 0.0f;
        // 0 = auto: the size is derived by the flex/anchor layout instead of
        // being pinned. UILayoutSystem only applies a size when it is > 0.
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
        // Monotonic creation stamp assigned from UISettingsComponent::nextLayoutOrder.
        // The ECS dense-array order reshuffles on every swap-remove destroy, so
        // it must never decide sibling order: layout, render and interaction
        // all sort by (hierarchyDepth, layoutOrder) instead. 0 = not stamped.
        uint32_t layoutOrder  = 0;
        bool     clipChildren = false;
        char     _free_space[3] {};
    };

    // A textured quad: an icon, a sprite, or a slice of a sprite atlas. The
    // renderer emits this instead of the plain panel quad whenever the entity
    // carries one, and the UV rectangle below selects the source region.
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

    struct UIImageComponent {
        TextureHandle  texture = TextureHandle::Invalid;
        ImageScaleMode mode    = ImageScaleMode::Stretch;
        JPH::Vec4      tint    = {1.0f, 1.0f, 1.0f, 1.0f};

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
    // The stops are inline and there are at most kMaxStops of them: a gradient
    // is a per-frame visual that a widget rewrites every time it is built, so
    // an allocating member here would churn the heap on every frame.
    struct UIGradientComponent {
        static constexpr uint32_t kMaxStops = 8;

        UIGradientAxis axis           = UIGradientAxis::Horizontal;
        uint32_t       stopCount      = 0; // < 2 paints nothing
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
    struct UIPlotComponent {
        // Guard against an unbounded series: 512 samples is 3k vertices worst
        // case (ShadedLines), which one UI batch absorbs.
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

};

} // namespace ZHLN::GUI
