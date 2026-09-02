// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Zahlen/Components.hpp"
#include "Zahlen/GUI.hpp"
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
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
    };

    static YGSize MeasureTextNode(YGNodeConstRef node, float width, YGMeasureMode widthMode, float height, YGMeasureMode heightMode) {
        auto* ctx = static_cast<TextMeasureContext*>(YGNodeGetContext(node));
        if (ctx == nullptr || ctx->font == nullptr || ctx->text.empty()) {
            return YGSize {.width = 0.0f, .height = 0.0f};
        }

        GUI::TextBounds bounds         = GUI::MeasureTextBounds(*ctx->font, ctx->text, ctx->scale);
        float           measuredWidth  = bounds.width();
        float           measuredHeight = bounds.height();

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

    void ResolveLayouts(ECS::Registry& reg, const UIViewport& viewport) {
        auto entities = reg.GetEntitiesWith<Components::UIRectComponent>();
        auto rects    = reg.GetRawArray<Components::UIRectComponent>();

        if (entities.empty()) {
            return;
        }

        // 1. Fetch active Font Atlas for text measurement
        const FontAtlas* activeFont     = nullptr;
        auto             uiSettingsEnts = reg.GetEntitiesWith<Components::UISettingsComponent>();
        if (!uiSettingsEnts.empty()) {
            activeFont = &reg.Get<Components::UISettingsComponent>(uiSettingsEnts[0])->fontAtlas;
        }

        // 2. Instantiate Yoga Nodes for every active UI Entity
        std::unordered_map<uint64_t, YGNodeRef> nodeMap;
        std::vector<TextMeasureContext*>        measureContexts;

        for (size_t i = 0; i < entities.size(); ++i) {
            Entity    e       = entities[i];
            YGNodeRef node    = YGNodeNew();
            nodeMap[e.Pack()] = node;

            const auto& rect = rects[i];
            auto*       flex = reg.Get<Components::UIFlexComponent>(e);
            auto* parentRect = (rect.parentEntity != Entity::Null() && reg.IsAlive(rect.parentEntity)) ? reg.Get<Components::UIRectComponent>(rect.parentEntity) :
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
            bool isFlexChild =
                (rect.parentEntity != Entity::Null() && reg.IsAlive(rect.parentEntity) && reg.Get<Components::UIFlexComponent>(rect.parentEntity) != nullptr);

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
                    case FlexDirection::Column:
                        YGNodeStyleSetFlexDirection(node, YGFlexDirectionColumn);
                        break;
                    case FlexDirection::ColumnReverse:
                        YGNodeStyleSetFlexDirection(node, YGFlexDirectionColumnReverse);
                        break;
                    case FlexDirection::Row:
                        YGNodeStyleSetFlexDirection(node, YGFlexDirectionRow);
                        break;
                    case FlexDirection::RowReverse:
                        YGNodeStyleSetFlexDirection(node, YGFlexDirectionRowReverse);
                        break;
                }

                switch (flex->justify) {
                    case FlexJustify::FlexStart:
                        YGNodeStyleSetJustifyContent(node, YGJustifyFlexStart);
                        break;
                    case FlexJustify::Center:
                        YGNodeStyleSetJustifyContent(node, YGJustifyCenter);
                        break;
                    case FlexJustify::FlexEnd:
                        YGNodeStyleSetJustifyContent(node, YGJustifyFlexEnd);
                        break;
                    case FlexJustify::SpaceBetween:
                        YGNodeStyleSetJustifyContent(node, YGJustifySpaceBetween);
                        break;
                    case FlexJustify::SpaceAround:
                        YGNodeStyleSetJustifyContent(node, YGJustifySpaceAround);
                        break;
                    case FlexJustify::SpaceEvenly:
                        YGNodeStyleSetJustifyContent(node, YGJustifySpaceEvenly);
                        break;
                }

                switch (flex->alignItems) {
                    case FlexAlign::FlexStart:
                        YGNodeStyleSetAlignItems(node, YGAlignFlexStart);
                        break;
                    case FlexAlign::Center:
                        YGNodeStyleSetAlignItems(node, YGAlignCenter);
                        break;
                    case FlexAlign::FlexEnd:
                        YGNodeStyleSetAlignItems(node, YGAlignFlexEnd);
                        break;
                    case FlexAlign::Stretch:
                        YGNodeStyleSetAlignItems(node, YGAlignStretch);
                        break;
                    case FlexAlign::Baseline:
                        YGNodeStyleSetAlignItems(node, YGAlignBaseline);
                        break;
                    default:
                        YGNodeStyleSetAlignItems(node, YGAlignAuto);
                        break;
                }

                switch (flex->alignSelf) {
                    case FlexAlign::FlexStart:
                        YGNodeStyleSetAlignSelf(node, YGAlignFlexStart);
                        break;
                    case FlexAlign::Center:
                        YGNodeStyleSetAlignSelf(node, YGAlignCenter);
                        break;
                    case FlexAlign::FlexEnd:
                        YGNodeStyleSetAlignSelf(node, YGAlignFlexEnd);
                        break;
                    case FlexAlign::Stretch:
                        YGNodeStyleSetAlignSelf(node, YGAlignStretch);
                        break;
                    case FlexAlign::Baseline:
                        YGNodeStyleSetAlignSelf(node, YGAlignBaseline);
                        break;
                    default:
                        YGNodeStyleSetAlignSelf(node, YGAlignAuto);
                        break;
                }

                switch (flex->wrap) {
                    case FlexWrap::NoWrap:
                        YGNodeStyleSetFlexWrap(node, YGWrapNoWrap);
                        break;
                    case FlexWrap::Wrap:
                        YGNodeStyleSetFlexWrap(node, YGWrapWrap);
                        break;
                    case FlexWrap::WrapReverse:
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

            // Intrinsic Content Measuring for Text Components
            if (rect.width <= 0.0f || rect.height <= 0.0f) {
                if (auto* textComp = reg.Get<Components::TextComponent>(e)) {
                    auto* textCtx = new TextMeasureContext {.font = activeFont, .text = textComp->text.c_str(), .scale = textComp->scale};
                    measureContexts.push_back(textCtx);
                    YGNodeSetContext(node, textCtx);
                    YGNodeSetMeasureFunc(node, &UILayoutSystem::MeasureTextNode);
                }
            }
        }

        // 3. Assemble Yoga Hierarchy Tree
        std::vector<YGNodeRef> rootNodes;

        for (size_t i = 0; i < entities.size(); ++i) {
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
            auto*     rect = reg.Get<Components::UIRectComponent>(e);
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

            // Recurse children
            for (size_t i = 0; i < entities.size(); ++i) {
                if (rects[i].parentEntity == e) {
                    self(self, entities[i], rect->computedAbsMinX, rect->computedAbsMinY);
                }
            }
        };

        for (size_t i = 0; i < entities.size(); ++i) {
            if (rects[i].parentEntity == Entity::Null() || !reg.IsAlive(rects[i].parentEntity)) {
                ReadBackLayout(ReadBackLayout, entities[i], 0.0f, 0.0f);
            }
        }

        // 6. Memory Reclamation
        YGNodeFreeRecursive(viewportRoot);
        for (auto* textCtx: measureContexts) {
            delete textCtx;
        }
    }
};

} // namespace ZHLN
