// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Zahlen/Components.hpp"
#include "Zahlen/GUI.hpp"
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <vector>
#include <yoga/Yoga.h>

namespace ZHLN {

class UILayoutSystem {
  public:
    struct UIViewport {
        float width;
        float height;
    };

    struct TextMeasureContext {
        const FontAtlas* font = nullptr;
        std::string      text;
        float            scale = 1.0f;
        // Automatic word wrapping: when enabled the text is re-flowed to the
        // width Yoga offers (or to wrapWidth, when the widget pins one) and the
        // measured height grows by whole lines.
        bool             wrap      = false;
        float            wrapWidth = 0.0f;
    };

    static YGSize MeasureTextNode(YGNodeConstRef node, float width, YGMeasureMode widthMode, float height, YGMeasureMode heightMode) {
        auto* ctx = static_cast<TextMeasureContext*>(YGNodeGetContext(node));
        if (ctx == nullptr || ctx->font == nullptr || ctx->text.empty()) {
            return YGSize {.width = 0.0f, .height = 0.0f};
        }

        // An explicit wrap width wins; otherwise the constraint Yoga passes in
        // is the container's content width, which is exactly what a designer
        // means by "wrap to the panel".
        float wrapAt = 0.0f;
        if (ctx->wrap) {
            wrapAt = (ctx->wrapWidth > 0.0f) ? ctx->wrapWidth : ((widthMode != YGMeasureModeUndefined) ? width : 0.0f);
        }

        GUI::TextBounds bounds = (wrapAt > 0.0f) ? GUI::MeasureWrappedTextBounds(*ctx->font, ctx->text, ctx->scale, wrapAt) :
                                                   GUI::MeasureTextBounds(*ctx->font, ctx->text, ctx->scale);
        float measuredWidth  = bounds.width();
        float measuredHeight = bounds.height();

        if (widthMode == YGMeasureModeExactly) {
            measuredWidth = width;
        } else if (widthMode == YGMeasureModeAtMost) {
            measuredWidth = std::min(measuredWidth, width);
        }

        if (heightMode == YGMeasureModeExactly) {
            measuredHeight = height;
        } else if (heightMode == YGMeasureModeAtMost) {
            measuredHeight = std::min(measuredHeight, height);
        }

        return YGSize {.width = measuredWidth, .height = measuredHeight};
    }

    /// Horizontal space a parent leaves for its children. Used to wrap text
    /// during the intrinsic-height walk, where Yoga has not been asked for a
    /// width yet. Returns 0 when nothing is known, which means "do not wrap".
    static auto AvailableContentWidth(ECS::Registry& reg, Entity parent) -> float {
        if (parent == Entity::Null() || !reg.IsAlive(parent)) {
            return 0.0f;
        }
        const auto* parentRect = reg.Get<GUI::UIComponents::UIRectComponent>(parent);
        if (parentRect == nullptr) {
            return 0.0f;
        }

        float width = parentRect->width;
        if (width <= 0.0f) {
            width = parentRect->computedAbsMaxX - parentRect->computedAbsMinX;
        }
        if (const auto* parentFlex = reg.Get<GUI::UIComponents::UIFlexComponent>(parent)) {
            width -= parentFlex->paddingLeft + parentFlex->paddingRight;
        }
        return std::max(0.0f, width);
    }

    void ResolveLayouts(ECS::Registry& reg, const UIViewport& viewport) {
        auto entities = reg.GetEntitiesWith<GUI::UIComponents::UIRectComponent>();
        auto rects    = reg.GetRawArray<GUI::UIComponents::UIRectComponent>();

        if (entities.empty()) {
            return;
        }

        // Processing order is CREATION order (layoutOrder), not ECS dense
        // order. The dense array reshuffles on every swap-remove destroy --
        // collapsing a section destroys its subtree and the last entity in
        // the array swaps into the hole -- which visibly reshuffled sibling
        // order (a Hierarchy block jumping above its panel's title) and made
        // window layering nondeterministic. Yoga child order follows this
        // sequence, so siblings keep their declared order forever.
        std::vector<size_t> seq(entities.size());
        for (size_t i = 0; i < seq.size(); ++i) {
            seq[i] = i;
        }
        std::stable_sort(seq.begin(), seq.end(), [&](size_t a, size_t b) -> bool { return rects[a].layoutOrder < rects[b].layoutOrder; });

        // 1. Fetch active Font Atlas for text measurement
        const FontAtlas* activeFont     = nullptr;
        auto             uiSettingsEnts = reg.GetEntitiesWith<GUI::UIComponents::UISettingsComponent>();
        if (!uiSettingsEnts.empty()) {
            activeFont = &reg.Get<GUI::UIComponents::UISettingsComponent>(uiSettingsEnts[0])->fontAtlas;
        }

        // 2. Instantiate Yoga Nodes for every active UI Entity
        std::unordered_map<uint64_t, YGNodeRef> nodeMap;
        std::vector<TextMeasureContext*>        measureContexts;

        for (size_t i = 0; i < entities.size(); ++i) {
            Entity    e       = entities[i];
            YGNodeRef node    = YGNodeNew();
            nodeMap[e.Pack()] = node;

            const auto& rect = rects[i];
            auto*       flex = reg.Get<GUI::UIComponents::UIFlexComponent>(e);
            auto* parentRect = (rect.parentEntity != Entity::Null() && reg.IsAlive(rect.parentEntity)) ?
                                   reg.Get<GUI::UIComponents::UIRectComponent>(rect.parentEntity) :
                                   nullptr;

            float pWidth  = (parentRect != nullptr) ?
                                (parentRect->width > 0.0f ? parentRect->width : (parentRect->computedAbsMaxX - parentRect->computedAbsMinX)) :
                                viewport.width;
            float pHeight = (parentRect != nullptr) ?
                                (parentRect->height > 0.0f ? parentRect->height : (parentRect->computedAbsMaxY - parentRect->computedAbsMinY)) :
                                viewport.height;

            if (pWidth <= 0.0f) {
                pWidth = viewport.width;
            }
            if (pHeight <= 0.0f) {
                pHeight = viewport.height;
            }

            // Explicit Dimensions
            if (rect.width > 0.0f) {
                YGNodeStyleSetWidth(node, rect.width);
            }
            if (rect.height > 0.0f) {
                YGNodeStyleSetHeight(node, rect.height);
            }

            // --- ANCHOR vs FLEX POSITIONING ---
            bool isFlexChild = (rect.parentEntity != Entity::Null() && reg.IsAlive(rect.parentEntity) &&
                                reg.Get<GUI::UIComponents::UIFlexComponent>(rect.parentEntity) != nullptr);

            if (!isFlexChild) {
                // Anchor-based Canvas Positioning (HUD popups, tool windows,
                // docked panels). anchorMin/Max form a fractional rect in the
                // parent; x/y/width/height are offsets/insets.
                //
                //   anchorMinX == anchorMaxX  ->  horizontal pivot (no stretch),
                //       position Left from pivot + x; width honours rect.width.
                //       When the pivot is ~0.5 (centered), x is interpreted as
                //       a center offset and the widget is shifted left by half
                //       its width so its CENTER lands on the pivot point.
                //
                //   anchorMinX != anchorMaxX  ->  stretch horizontally between
                //       the two anchors; x is the left inset, rect.width the
                //       right inset (set as Right=... inset in Yoga).
                //
                // Same semantics for Y.
                YGNodeStyleSetPositionType(node, YGPositionTypeAbsolute);
                // Absolute children must opt out of the parent's alignItems
                // (which defaults to Stretch in our flex containers and in
                // Yoga). Without this, Yoga stretches the cross-axis size of
                // absolute children to the parent's size on layout, ignoring
                // the widget's explicit width/height whenever the parent
                // resizes (the "window resize stretches panels" bug).
                YGNodeStyleSetAlignSelf(node, YGAlignFlexStart);

                const float aMinX = std::clamp(rect.anchorMinX, 0.0f, 1.0f);
                const float aMaxX = std::clamp(rect.anchorMaxX, 0.0f, 1.0f);
                const float aMinY = std::clamp(rect.anchorMinY, 0.0f, 1.0f);
                const float aMaxY = std::clamp(rect.anchorMaxY, 0.0f, 1.0f);

                const bool hStretch = (aMaxX - aMinX) > 0.001f;
                const bool vStretch = (aMaxY - aMinY) > 0.001f;
                const bool hCenter  = (!hStretch) && std::abs(aMinX - 0.5f) < 0.001f && (rect.width > 0.0f);
                const bool vCenter  = (!vStretch) && std::abs(aMinY - 0.5f) < 0.001f && (rect.height > 0.0f);

                if (hStretch) {
                    YGNodeStyleSetPosition(node, YGEdgeLeft,  aMinX * pWidth  + rect.x);
                    YGNodeStyleSetPosition(node, YGEdgeRight, (1.0f - aMaxX) * pWidth - (rect.width > 0.0f ? rect.width : 0.0f));
                } else {
                    float left = aMinX * pWidth + rect.x - (hCenter ? rect.width * 0.5f : 0.0f);
                    YGNodeStyleSetPosition(node, YGEdgeLeft, left);
                    if (rect.width > 0.0f) YGNodeStyleSetWidth(node, rect.width);
                }

                if (vStretch) {
                    YGNodeStyleSetPosition(node, YGEdgeTop,    aMinY * pHeight + rect.y);
                    YGNodeStyleSetPosition(node, YGEdgeBottom,(1.0f - aMaxY) * pHeight - (rect.height > 0.0f ? rect.height : 0.0f));
                } else {
                    float top = aMinY * pHeight + rect.y - (vCenter ? rect.height * 0.5f : 0.0f);
                    YGNodeStyleSetPosition(node, YGEdgeTop, top);
                    if (rect.height > 0.0f) YGNodeStyleSetHeight(node, rect.height);
                }
            }

            if (flex != nullptr) {
                // Flexbox Style Configuration for laying out children
                switch (flex->direction) {
                    case GUI::FlexDirection::Column:
                        YGNodeStyleSetFlexDirection(node, YGFlexDirectionColumn);
                        break;
                    case GUI::FlexDirection::ColumnReverse:
                        YGNodeStyleSetFlexDirection(node, YGFlexDirectionColumnReverse);
                        break;
                    case GUI::FlexDirection::Row:
                        YGNodeStyleSetFlexDirection(node, YGFlexDirectionRow);
                        break;
                    case GUI::FlexDirection::RowReverse:
                        YGNodeStyleSetFlexDirection(node, YGFlexDirectionRowReverse);
                        break;
                }

                switch (flex->justify) {
                    case GUI::FlexJustify::FlexStart:
                        YGNodeStyleSetJustifyContent(node, YGJustifyFlexStart);
                        break;
                    case GUI::FlexJustify::Center:
                        YGNodeStyleSetJustifyContent(node, YGJustifyCenter);
                        break;
                    case GUI::FlexJustify::FlexEnd:
                        YGNodeStyleSetJustifyContent(node, YGJustifyFlexEnd);
                        break;
                    case GUI::FlexJustify::SpaceBetween:
                        YGNodeStyleSetJustifyContent(node, YGJustifySpaceBetween);
                        break;
                    case GUI::FlexJustify::SpaceAround:
                        YGNodeStyleSetJustifyContent(node, YGJustifySpaceAround);
                        break;
                    case GUI::FlexJustify::SpaceEvenly:
                        YGNodeStyleSetJustifyContent(node, YGJustifySpaceEvenly);
                        break;
                }

                switch (flex->alignItems) {
                    case GUI::FlexAlign::FlexStart:
                        YGNodeStyleSetAlignItems(node, YGAlignFlexStart);
                        break;
                    case GUI::FlexAlign::Center:
                        YGNodeStyleSetAlignItems(node, YGAlignCenter);
                        break;
                    case GUI::FlexAlign::FlexEnd:
                        YGNodeStyleSetAlignItems(node, YGAlignFlexEnd);
                        break;
                    case GUI::FlexAlign::Stretch:
                        YGNodeStyleSetAlignItems(node, YGAlignStretch);
                        break;
                    case GUI::FlexAlign::Baseline:
                        YGNodeStyleSetAlignItems(node, YGAlignBaseline);
                        break;
                    default:
                        YGNodeStyleSetAlignItems(node, YGAlignAuto);
                        break;
                }

                switch (flex->alignSelf) {
                    case GUI::FlexAlign::FlexStart:
                        YGNodeStyleSetAlignSelf(node, YGAlignFlexStart);
                        break;
                    case GUI::FlexAlign::Center:
                        YGNodeStyleSetAlignSelf(node, YGAlignCenter);
                        break;
                    case GUI::FlexAlign::FlexEnd:
                        YGNodeStyleSetAlignSelf(node, YGAlignFlexEnd);
                        break;
                    case GUI::FlexAlign::Stretch:
                        YGNodeStyleSetAlignSelf(node, YGAlignStretch);
                        break;
                    case GUI::FlexAlign::Baseline:
                        YGNodeStyleSetAlignSelf(node, YGAlignBaseline);
                        break;
                    default:
                        YGNodeStyleSetAlignSelf(node, YGAlignAuto);
                        break;
                }

                switch (flex->wrap) {
                    case GUI::FlexWrap::NoWrap:
                        YGNodeStyleSetFlexWrap(node, YGWrapNoWrap);
                        break;
                    case GUI::FlexWrap::Wrap:
                        YGNodeStyleSetFlexWrap(node, YGWrapWrap);
                        break;
                    case GUI::FlexWrap::WrapReverse:
                        YGNodeStyleSetFlexWrap(node, YGWrapWrapReverse);
                        break;
                }

                YGNodeStyleSetFlexGrow(node, flex->flexGrow);
                YGNodeStyleSetFlexShrink(node, flex->flexShrink);
                if (flex->flexBasis >= 0.0f) {
                    YGNodeStyleSetFlexBasis(node, flex->flexBasis);
                } else {
                    YGNodeStyleSetFlexBasisAuto(node);
                }

                YGNodeStyleSetPadding(node, YGEdgeLeft, flex->paddingLeft);
                YGNodeStyleSetPadding(node, YGEdgeTop, flex->paddingTop);
                YGNodeStyleSetPadding(node, YGEdgeRight, flex->paddingRight);
                YGNodeStyleSetPadding(node, YGEdgeBottom, flex->paddingBottom);

                YGNodeStyleSetMargin(node, YGEdgeLeft, flex->marginLeft);
                YGNodeStyleSetMargin(node, YGEdgeTop, flex->marginTop);
                YGNodeStyleSetMargin(node, YGEdgeRight, flex->marginRight);
                YGNodeStyleSetMargin(node, YGEdgeBottom, flex->marginBottom);

                YGNodeStyleSetGap(node, YGGutterColumn, flex->gapX);
                YGNodeStyleSetGap(node, YGGutterRow, flex->gapY);
            }

            // Children of a scroll viewport never shrink. Yoga's default
            // flex-shrink of 1 squeezes a 3x40px list into a 100px viewport,
            // which silently removes the very overflow the ScrollBox exists to
            // scroll — the content fits, maxScrollY stays 0, and the wheel does
            // nothing. The scroll axis is exempt from shrink-to-fit by design.
            if (rect.parentEntity != Entity::Null() && reg.IsAlive(rect.parentEntity) &&
                reg.Get<GUI::UIComponents::UIScrollComponent>(rect.parentEntity) != nullptr) {
                YGNodeStyleSetFlexShrink(node, 0.0f);
            }

            // Intrinsic Content Measuring for Text Components
            if (rect.width <= 0.0f || rect.height <= 0.0f) {
                if (auto* textComp = reg.Get<GUI::UIComponents::TextComponent>(e)) {
                    auto* textCtx = new TextMeasureContext {
                        .font      = activeFont,
                        .text      = textComp->text.c_str(),
                        .scale     = textComp->scale,
                        .wrap      = textComp->wrapText,
                        .wrapWidth = textComp->wrapWidth
                    };
                    measureContexts.push_back(textCtx);
                    YGNodeSetContext(node, textCtx);
                    YGNodeSetMeasureFunc(node, &UILayoutSystem::MeasureTextNode);
                }
            }

            // Intrinsic Content Sizing for Images. A sprite knows its own
            // native size, so `ui.Image(id, tex, {.mode = FitAspect,
            // .sourceWidth = 64, .sourceHeight = 64})` sizes itself instead of
            // collapsing to zero. An explicit width/height always wins, and a
            // stretched flex child keeps overriding this through align/flex.
            if (auto* image = reg.Get<GUI::UIComponents::UIImageComponent>(e)) {
                if (rect.width <= 0.0f && image->sourceWidth > 0.0f) {
                    YGNodeStyleSetWidth(node, image->sourceWidth);
                }
                if (rect.height <= 0.0f && image->sourceHeight > 0.0f) {
                    YGNodeStyleSetHeight(node, image->sourceHeight);
                }
            }
        }

        // 2b. Supply an intrinsic cross-axis size for auto-height flex
        // containers.  Yoga's flex-basis is a main-axis value: a horizontal
        // splitter whose panes intentionally use flexBasis=0 therefore has no
        // height to contribute while its parent is also auto-height.  That
        // leaves the splitter at zero height even when a descendant (such as
        // the Preview box) has a real height, and the descendant then appears
        // to fall out of the parent panel.
        //
        // Compute the content height bottom-up and expose it as a min-height
        // only for auto-height flex nodes.  Explicit heights retain their
        // normal fixed-size semantics.  This is deliberately done before the
        // Yoga calculation so both the splitter and its auto-height ancestor
        // participate in the same layout pass.
        std::unordered_map<uint64_t, float> intrinsicHeights;
        auto ComputeIntrinsicHeight = [&](auto& self, Entity e) -> float {
            if (const auto found = intrinsicHeights.find(e.Pack()); found != intrinsicHeights.end()) {
                return found->second;
            }

            const auto* rect = reg.Get<GUI::UIComponents::UIRectComponent>(e);
            if (rect == nullptr) {
                return 0.0f;
            }

            // A scroll viewport contributes its OWN height to its ancestors,
            // never its content height. Content that overflows the viewport is
            // the whole point of the widget; letting it leak upward would make
            // every auto-height ancestor grow to fit the scrolled-away rows
            // and leave the scroller with nothing to scroll.
            if (reg.Get<GUI::UIComponents::UIScrollComponent>(e) != nullptr) {
                const float own = std::max(0.0f, rect->height);
                intrinsicHeights.emplace(e.Pack(), own);
                return own;
            }

            float intrinsicHeight = std::max(0.0f, rect->height);
            const auto* flex      = reg.Get<GUI::UIComponents::UIFlexComponent>(e);

            // Fixed-size nodes do not need (and must not acquire) a larger
            // min-height merely because one of their children overflows.
            if (rect->height <= 0.0f && flex != nullptr) {
                const bool column = flex->direction == GUI::FlexDirection::Column || flex->direction == GUI::FlexDirection::ColumnReverse;
                float      contentHeight = 0.0f;
                float      maxChildHeight = 0.0f;
                size_t     childCount = 0;

                for (size_t i = 0; i < entities.size(); ++i) {
                    if (rects[i].parentEntity != e) {
                        continue;
                    }

                    float childHeight = self(self, entities[i]);
                    if (const auto* childFlex = reg.Get<GUI::UIComponents::UIFlexComponent>(entities[i])) {
                        childHeight += childFlex->marginTop + childFlex->marginBottom;
                    }

                    ++childCount;
                    if (column) {
                        contentHeight += childHeight;
                    } else {
                        maxChildHeight = std::max(maxChildHeight, childHeight);
                    }
                }

                if (column) {
                    if (childCount > 1) {
                        contentHeight += static_cast<float>(childCount - 1) * flex->gapY;
                    }
                    contentHeight += flex->paddingTop + flex->paddingBottom;
                } else if (childCount > 0) {
                    contentHeight = maxChildHeight + flex->paddingTop + flex->paddingBottom;
                }

                intrinsicHeight = std::max(intrinsicHeight, contentHeight);
            }

            // A text-only auto-height node can be encountered by the
            // bottom-up walk even when it does not have an explicit widget
            // height.  Match the measure function's result, wrapping included,
            // so a wrapped paragraph contributes every one of its lines here.
            if (intrinsicHeight <= 0.0f && activeFont != nullptr) {
                if (const auto* textComp = reg.Get<GUI::UIComponents::TextComponent>(e)) {
                    float wrapAt = 0.0f;
                    if (textComp->wrapText) {
                        wrapAt = textComp->wrapWidth;
                        if (wrapAt <= 0.0f) {
                            wrapAt = AvailableContentWidth(reg, rect->parentEntity);
                        }
                    }
                    intrinsicHeight = (wrapAt > 0.0f) ? GUI::MeasureWrappedTextBounds(*activeFont, textComp->text, textComp->scale, wrapAt).height() :
                                                        GUI::MeasureTextBounds(*activeFont, textComp->text, textComp->scale).height();
                }
            }

            intrinsicHeights.emplace(e.Pack(), intrinsicHeight);
            return intrinsicHeight;
        };

        for (Entity e: entities) {
            const auto* rect = reg.Get<GUI::UIComponents::UIRectComponent>(e);
            if (rect == nullptr || rect->height > 0.0f || reg.Get<GUI::UIComponents::UIFlexComponent>(e) == nullptr) {
                continue;
            }

            // A scroll viewport must never acquire a content-sized min-height:
            // that is precisely the height it is supposed to clip away, and
            // granting it would make the container grow to fit everything and
            // leave nothing to scroll.
            if (reg.Get<GUI::UIComponents::UIScrollComponent>(e) != nullptr) {
                continue;
            }

            const float intrinsicHeight = ComputeIntrinsicHeight(ComputeIntrinsicHeight, e);
            if (intrinsicHeight > 0.0f) {
                YGNodeStyleSetMinHeight(nodeMap[e.Pack()], intrinsicHeight);
            }
        }

        // 3. Assemble Yoga Hierarchy Tree
        std::vector<YGNodeRef> rootNodes;

        for (size_t k = 0; k < seq.size(); ++k) {
            const size_t i = seq[k];
            Entity    e         = entities[i];
            YGNodeRef childNode = nodeMap[e.Pack()];
            Entity    parent    = rects[i].parentEntity;

            if (parent != Entity::Null() && reg.IsAlive(parent) && nodeMap.contains(parent.Pack())) {
                YGNodeRef parentNode = nodeMap[parent.Pack()];
                uint32_t  childIndex = YGNodeGetChildCount(parentNode);
                YGNodeInsertChild(parentNode, childNode, childIndex);
            } else {
                rootNodes.push_back(childNode);
            }
        }

        // 4. Wrap roots in a Top-Level Viewport Node & Calculate Layout
        YGNodeRef viewportRoot = YGNodeNew();
        YGNodeStyleSetWidth(viewportRoot, viewport.width);
        YGNodeStyleSetHeight(viewportRoot, viewport.height);

        for (size_t i = 0; i < rootNodes.size(); ++i) {
            YGNodeInsertChild(viewportRoot, rootNodes[i], static_cast<uint32_t>(i));
        }

        YGNodeCalculateLayout(viewportRoot, viewport.width, viewport.height, YGDirectionLTR);

        // 5. Read Back Computed Coordinates into UIRectComponent
        auto ReadBackLayout = [&](auto& self, Entity e, float parentAbsMinX, float parentAbsMinY) -> void {
            YGNodeRef node = nodeMap[e.Pack()];
            auto*     rect = reg.Get<GUI::UIComponents::UIRectComponent>(e);
            if (rect == nullptr) {
                return;
            }

            float left   = YGNodeLayoutGetLeft(node);
            float top    = YGNodeLayoutGetTop(node);
            float width  = YGNodeLayoutGetWidth(node);
            float height = YGNodeLayoutGetHeight(node);

            rect->computedAbsMinX = parentAbsMinX + left;
            rect->computedAbsMinY = parentAbsMinY + top;
            rect->computedAbsMaxX = rect->computedAbsMinX + width;
            rect->computedAbsMaxY = rect->computedAbsMinY + height;

            // A scroll viewport shifts its whole subtree by the scroll offset.
            // Only the CONTENT moves: the viewport's own rect (and therefore
            // the scissor rect the renderer derives from it) stays put, which
            // is what turns the shift into clipping.
            float childOriginX = rect->computedAbsMinX;
            float childOriginY = rect->computedAbsMinY;
            if (const auto* scroll = reg.Get<GUI::UIComponents::UIScrollComponent>(e)) {
                childOriginX -= scroll->scrollX;
                childOriginY -= scroll->scrollY;
            }

            // Recurse children
            for (size_t i = 0; i < entities.size(); ++i) {
                if (rects[i].parentEntity == e) {
                    self(self, entities[i], childOriginX, childOriginY);
                }
            }
        };

        for (size_t i = 0; i < entities.size(); ++i) {
            if (rects[i].parentEntity == Entity::Null() || !reg.IsAlive(rects[i].parentEntity)) {
                ReadBackLayout(ReadBackLayout, entities[i], 0.0f, 0.0f);
            }
        }

        // 5a. Measure scroll content extents now that every rect is final, and
        // clamp the live offsets into range (content that shrank below the
        // viewport must pull the offset back with it).
        GUI::UpdateScrollExtents(reg);

        // 5b. Post-layout re-centering pass. For anchor-positioned widgets
        // (absolute, not flex children) that sit on a centered pivot with an
        // auto-measured axis (width or height <= 0, meaning Yoga sized the
        // axis from content), the first-pass top/left was placed at the
        // pivot fraction because Yoga cannot know the final content size
        // before layout. Shift them so their CENTER lands on the pivot
        // point after we know the measured size. This is what makes a
        // `PanelConfig{.width=400,.height=0}` centered tool window actually
        // sit in the middle of the viewport rather than dropping off the
        // bottom edge (the "centered auto-height panel" case).
        for (size_t i = 0; i < entities.size(); ++i) {
            Entity e = entities[i];
            const auto& r = rects[i];
            const bool isFlexChild =
                (r.parentEntity != Entity::Null() && reg.IsAlive(r.parentEntity) &&
                 reg.Get<GUI::UIComponents::UIFlexComponent>(r.parentEntity) != nullptr);
            if (isFlexChild) continue;

            float pWidth = viewport.width, pHeight = viewport.height;
            if (r.parentEntity != Entity::Null() && reg.IsAlive(r.parentEntity)) {
                if (const auto* pr = reg.Get<GUI::UIComponents::UIRectComponent>(r.parentEntity)) {
                    pWidth  = pr->computedAbsMaxX - pr->computedAbsMinX;
                    pHeight = pr->computedAbsMaxY - pr->computedAbsMinY;
                }
            }

            const bool centeredH = std::abs(r.anchorMinX - 0.5f) < 0.001f && std::abs(r.anchorMaxX - 0.5f) < 0.001f && (r.width <= 0.0f);
            const bool centeredV = std::abs(r.anchorMinY - 0.5f) < 0.001f && std::abs(r.anchorMaxY - 0.5f) < 0.001f && (r.height <= 0.0f);
            if (!centeredH && !centeredV) continue;

            const float w = r.computedAbsMaxX - r.computedAbsMinX;
            const float h = r.computedAbsMaxY - r.computedAbsMinY;
            float shiftX = 0.0f, shiftY = 0.0f;
            if (centeredH) shiftX = (pWidth  * 0.5f) - (r.computedAbsMinX + w * 0.5f);
            if (centeredV) shiftY = (pHeight * 0.5f) - (r.computedAbsMinY + h * 0.5f);
            if (std::abs(shiftX) < 0.5f && std::abs(shiftY) < 0.5f) continue;

            // Shift this entity AND all its descendants by the delta.
            std::function<void(Entity, float, float)> shift = [&](Entity ent, float dx, float dy) {
                if (auto* rr = reg.Get<GUI::UIComponents::UIRectComponent>(ent)) {
                    rr->computedAbsMinX += dx;
                    rr->computedAbsMaxX += dx;
                    rr->computedAbsMinY += dy;
                    rr->computedAbsMaxY += dy;
                }
                for (size_t j = 0; j < entities.size(); ++j) {
                    if (rects[j].parentEntity == ent) shift(entities[j], dx, dy);
                }
            };
            shift(e, shiftX, shiftY);
        }

        // 6. Memory Reclamation
        YGNodeFreeRecursive(viewportRoot);
        for (auto* textCtx: measureContexts) {
            delete textCtx;
        }
    }
};

} // namespace ZHLN
