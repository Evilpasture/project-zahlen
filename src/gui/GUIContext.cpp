// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#define CLAY_IMPLEMENTATION
#include "Text.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <algorithm>
#include <clay.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ZHLN::GUI {

// Internal persistent state for widgets (drag state, foldout status)
struct WidgetState {
    bool     isDragging      = false;
    bool     isPressed       = false;
    bool     isOpen          = false;
    bool     isInitialized   = false;
    uint64_t lastActiveFrame = 0;
};

// ============================================================================
// Context::Impl Definition (Owned per-engine instance)
// ============================================================================
struct Context::Impl {
    Engine&                              engine;
    Clay_Context*                        clayContext = nullptr;
    Clay_Arena                           clayArena   = {};
    std::vector<std::byte>               arenaMemory;
    ZHLN::HashMap<uint64_t, WidgetState> widgetStates;
    const FontAtlas*                     activeFont      = nullptr;
    float                                lastDt          = 0.016667f;
    bool                                 lastItemHovered = false;
    bool                                 lastItemActive  = false;

    explicit Impl(Engine& eng) noexcept: engine(eng) {
    }

    ~Impl() noexcept {
        if (Clay_GetCurrentContext() == clayContext) {
            Clay_SetCurrentContext(nullptr);
        }
        clayContext = nullptr;
    }

    WidgetState& GetState(uint64_t id, uint64_t currentFrame) noexcept {
        auto* state = widgetStates.Find(id);
        if (!state) {
            widgetStates.Insert(id, WidgetState {});
            state = widgetStates.Find(id);
        }
        ZHLN::Assert(state);
        state->lastActiveFrame = currentFrame;
        return *state;
    }

    void PruneStaleStates(uint64_t currentFrame) noexcept {
        widgetStates.ForEach([&](uint64_t id, const WidgetState& s) {
            if (currentFrame > s.lastActiveFrame + 60) {
                widgetStates.Erase(id);
            }
        });
    }

    static Clay_Dimensions MeasureText(Clay_StringSlice text, Clay_TextElementConfig* config, void* userData) {
        auto* impl = static_cast<Impl*>(userData);
        if (!impl || !impl->activeFont || text.length == 0)
            return {0.0f, 0.0f};

        float scale      = static_cast<float>(config->fontSize) / 32.0f;
        float currentX   = 0.0f;
        float maxX       = 0.0f;
        float lineHeight = TextLineHeight(scale);
        float totalH     = lineHeight;

        for (int32_t i = 0; i < text.length; ++i) {
            char c = text.chars[i];
            if (c == '\n') {
                maxX     = std::max(maxX, currentX);
                currentX = 0.0f;
                totalH  += lineHeight;
                continue;
            }
            if (c == '\r') {
                continue;
            }
            uint32_t glyphCode = static_cast<uint8_t>(c);
            if (glyphCode < 32 || glyphCode > 127) {
                glyphCode = '?';
            }
            const auto& g = impl->activeFont->glyphs[glyphCode - 32];
            currentX += g.xadvance * scale;
        }
        maxX = std::max(maxX, currentX);
        return {maxX, totalH};
    }
};

namespace {

constexpr Clay_String ToClayString(std::string_view sv) noexcept {
    return Clay_String {.isStaticallyAllocated = false, .length = static_cast<int32_t>(sv.size()), .chars = sv.data()};
}

Clay_SizingAxis ToClaySizing(const Sizing& s) noexcept {
    if (s.fixed > 0.0f)
        return CLAY_SIZING_FIXED(s.fixed);
    if (s.grow > 0.0f)
        return CLAY_SIZING_GROW();
    return CLAY_SIZING_FIT();
}

Clay_Color ToClayColor(const JPH::Vec4& c) noexcept {
    return {
        static_cast<float>(c.GetX() * 255.0f), static_cast<float>(c.GetY() * 255.0f), static_cast<float>(c.GetZ() * 255.0f),
        static_cast<float>(c.GetW() * 255.0f)
    };
}

Clay_LayoutAlignmentX ToClayAlignX(Alignment align) noexcept {
    switch (align) {
        case Alignment::Center: return CLAY_ALIGN_X_CENTER;
        case Alignment::End:    return CLAY_ALIGN_X_RIGHT;
        default:                return CLAY_ALIGN_X_LEFT;
    }
}

Clay_LayoutAlignmentY ToClayAlignY(Alignment align) noexcept {
    switch (align) {
        case Alignment::Center: return CLAY_ALIGN_Y_CENTER;
        case Alignment::End:    return CLAY_ALIGN_Y_BOTTOM;
        default:                return CLAY_ALIGN_Y_TOP;
    }
}

Clay_ChildAlignment ToClayChildAlignment(const BoxConfig& cfg) noexcept {
    if (cfg.direction == Direction::Row) {
        return {
            .x = ToClayAlignX(cfg.alignMain),
            .y = ToClayAlignY(cfg.alignCross)
        };
    } else {
        return {
            .x = ToClayAlignX(cfg.alignCross),
            .y = ToClayAlignY(cfg.alignMain)
        };
    }
}

struct GUIStateComponent {
    std::unique_ptr<Context::Impl> impl;
};

} // namespace

// ============================================================================
// Lifecycle Methods
// ============================================================================

Context::Context(Engine& engine) noexcept {
    auto& state = engine.GetRegistry().GetOrEmplaceSingleton<GUIStateComponent>();
    if (!state.impl) {
        state.impl = std::make_unique<Impl>(engine);
    }
    _impl = state.impl.get();
}

Context::~Context() noexcept = default;

void Context::BeginFrame(float dt) noexcept {
    _impl->lastDt  = dt;
    auto  winSize  = _impl->engine.GetWindow().GetSize();
    auto* input    = _impl->engine.GetRegistry().GetSingleton<Components::InputStateComponent>();
    auto* settings = _impl->engine.GetRegistry().GetSingleton<UIComponents::UISettingsComponent>();
    if (!input || !settings)
        return;

    _impl->activeFont = &settings->fontAtlas;

    // Lazily allocate Clay memory arena on this instance once
    if (!_impl->clayContext) {
        Clay_SetCurrentContext(nullptr);
        Clay_SetMaxElementCount(8192);
        Clay_SetMaxMeasureTextCacheWordCount(8192);
        uint64_t memSize = Clay_MinMemorySize();
        _impl->arenaMemory.resize(memSize);
        _impl->clayArena   = Clay_CreateArenaWithCapacityAndMemory(memSize, _impl->arenaMemory.data());
        _impl->clayContext = Clay_Initialize(_impl->clayArena, {static_cast<float>(winSize.width), static_cast<float>(winSize.height)}, {});
        Clay_SetMeasureTextFunction(Impl::MeasureText, _impl);
    }

    Clay_SetCurrentContext(_impl->clayContext);

    Clay_SetLayoutDimensions({static_cast<float>(winSize.width), static_cast<float>(winSize.height)});
    Clay_SetPointerState(Clay_Vector2 {input->mouseX, input->mouseY}, input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton)));
    Clay_UpdateScrollContainers(false, Clay_Vector2 {0.0f, input->GetMouseWheel() * 30.0f}, dt);

    _impl->PruneStaleStates(_impl->engine.GetCurrentFrame());
    Clay_BeginLayout();
}

void Context::EndFrameAndRender(RenderContext& rc) noexcept {
    Clay_SetCurrentContext(_impl->clayContext);
    Clay_RenderCommandArray commands = Clay_EndLayout(_impl->lastDt);
    if (commands.length == 0 || !_impl->activeFont)
        return;

    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    std::vector<UIBatch>          batches;

    positions.reserve(commands.length * 6);
    attributes.reserve(commands.length * 6);

    auto EmitQuad = [&](float x0, float y0, float x1, float y1, JPH::Vec4 color) {
        PackedRGBA8   c = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());
        Packed1010102 n = Math::PackNormal(0, 0, 1);
        Packed1010102 t = Math::PackNormal(1, 0, 0, 1);

        positions.push_back({{x0, y0, 0.0f}});
        positions.push_back({{x0, y1, 0.0f}});
        positions.push_back({{x1, y0, 0.0f}});
        positions.push_back({{x1, y0, 0.0f}});
        positions.push_back({{x0, y1, 0.0f}});
        positions.push_back({{x1, y1, 0.0f}});

        attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(0, 0), .color = c});
        attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(0, 1), .color = c});
        attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(1, 0), .color = c});
        attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(1, 0), .color = c});
        attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(0, 1), .color = c});
        attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(1, 1), .color = c});
    };

    ScissorRect activeScissor = {};
    bool        useScissor    = false;

    for (int i = 0; i < commands.length; ++i) {
        Clay_RenderCommand* cmd = &commands.internalArray[i];
        const auto&         bb  = cmd->boundingBox;

        switch (cmd->commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                auto      c = cmd->renderData.rectangle.backgroundColor;
                JPH::Vec4 color(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);

                uint32_t startIdx = static_cast<uint32_t>(positions.size());
                EmitQuad(bb.x, bb.y, bb.x + bb.width, bb.y + bb.height, color);

                batches.push_back(
                    {.texture     = TextureHandle::Invalid,
                     .vertexStart = startIdx,
                     .vertexCount = 6,
                     .useScissor  = useScissor,
                     .isSDF       = false,
                     .scissorRect = activeScissor}
                );
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                auto      tc = cmd->renderData.text.textColor;
                JPH::Vec4 color(tc.r / 255.0f, tc.g / 255.0f, tc.b / 255.0f, tc.a / 255.0f);
                float     scale = static_cast<float>(cmd->renderData.text.fontSize) / 32.0f;

                std::string text(cmd->renderData.text.stringContents.chars, static_cast<size_t>(cmd->renderData.text.stringContents.length));
                uint32_t    maxVerts = static_cast<uint32_t>(text.size()) * 6;

                size_t startIdx = positions.size();
                positions.resize(startIdx + maxVerts);
                attributes.resize(startIdx + maxVerts);

                uint32_t written = AppendTextVertices(&positions[startIdx], &attributes[startIdx], *_impl->activeFont, text, bb.x, bb.y, scale, color);
                positions.resize(startIdx + written);
                attributes.resize(startIdx + written);

                batches.push_back(
                    {.texture     = _impl->activeFont->texture,
                     .vertexStart = static_cast<uint32_t>(startIdx),
                     .vertexCount = written,
                     .useScissor  = useScissor,
                     .isSDF       = true,
                     .scissorRect = activeScissor}
                );
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
                useScissor    = true;
                activeScissor = {
                    .x      = static_cast<int32_t>(bb.x),
                    .y      = static_cast<int32_t>(bb.y),
                    .width  = static_cast<uint32_t>(bb.width),
                    .height = static_cast<uint32_t>(bb.height)
                };
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: {
                useScissor = false;
                break;
            }
            default:
                break;
        }
    }

    rc.SubmitUI(batches.data(), static_cast<uint32_t>(batches.size()), positions.data(), attributes.data(), static_cast<uint32_t>(positions.size()));
}

// ============================================================================
// Layout and Containers
// ============================================================================

void Context::BeginBox(std::string_view id, const BoxConfig& cfg) noexcept {
    Clay_SetCurrentContext(_impl->clayContext);

    Clay_ElementDeclaration decl = {
        .layout =
            {.sizing = {.width = ToClaySizing(cfg.width), .height = ToClaySizing(cfg.height)},
             .padding =
                 {static_cast<uint16_t>(cfg.padding), static_cast<uint16_t>(cfg.padding), static_cast<uint16_t>(cfg.padding),
                  static_cast<uint16_t>(cfg.padding)},
             .childGap        = static_cast<uint16_t>(cfg.gap),
             .childAlignment  = ToClayChildAlignment(cfg),
             .layoutDirection = (cfg.direction == Direction::Row) ? CLAY_LEFT_TO_RIGHT : CLAY_TOP_TO_BOTTOM},
        .backgroundColor = ToClayColor(cfg.color),
        .cornerRadius    = {cfg.cornerRadius.GetX(), cfg.cornerRadius.GetY(), cfg.cornerRadius.GetZ(), cfg.cornerRadius.GetW()}
    };

    if (!id.empty()) {
        uint32_t       numId  = static_cast<uint32_t>(HashCreativeWorkPath(id));
        Clay_ElementId elemId = Clay_GetElementIdWithIndex(ToClayString(id), numId);
        Clay__OpenElementWithId(elemId);
    } else {
        Clay__OpenElement();
    }

    Clay__ConfigureOpenElement(decl);
}

void Context::EndBox() noexcept {
    Clay_SetCurrentContext(_impl->clayContext);
    Clay__CloseElement();
}

void Context::BeginRow(float gap, float padding) noexcept {
    BeginBox("", {.width = {.grow = 1.0f}, .padding = padding, .gap = gap, .direction = Direction::Row, .alignCross = Alignment::Center});
}

void Context::EndRow() noexcept {
    EndBox();
}

void Context::BeginColumn(float gap, float padding) noexcept {
    BeginBox("", {.height = {.grow = 1.0f}, .padding = padding, .gap = gap, .direction = Direction::Column});
}

void Context::EndColumn() noexcept {
    EndBox();
}

// ============================================================================
// Interactive Widgets
// ============================================================================

void Context::Text(std::string_view text, float fontSize, const JPH::Vec4& color) noexcept {
    Clay_SetCurrentContext(_impl->clayContext);
    Clay_TextElementConfig config = {.textColor = ToClayColor(color), .fontSize = static_cast<uint16_t>(fontSize)};
    Clay__OpenTextElement(ToClayString(text), config);
}

bool Context::Button(std::string_view label, const JPH::Vec4& color, const Sizing& width) noexcept {
    Clay_SetCurrentContext(_impl->clayContext);
    bool           clicked = false;
    uint32_t       idNum   = static_cast<uint32_t>(HashCreativeWorkPath(label));
    Clay_ElementId elemId  = Clay_GetElementIdWithIndex(ToClayString(label), idNum);
    auto&          state   = _impl->GetState((static_cast<uint64_t>(idNum) << 32) | 0xB007, _impl->engine.GetCurrentFrame());

    auto* input = _impl->engine.GetRegistry().GetSingleton<Components::InputStateComponent>();
    float mx = input ? input->mouseX : -1.0f;
    float my = input ? input->mouseY : -1.0f;
    bool isMouseDown = input && input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton));

    Clay_ElementData elemData = Clay_GetElementData(elemId);
    bool isHovered = Clay_Hovered() || Clay_PointerOver(elemId) ||
                     (elemData.found && elemData.boundingBox.width > 0.0f &&
                      mx >= elemData.boundingBox.x && mx <= (elemData.boundingBox.x + elemData.boundingBox.width) &&
                      my >= elemData.boundingBox.y && my <= (elemData.boundingBox.y + elemData.boundingBox.height));

    auto pointer = Clay_GetPointerState();
    bool isPressedNow = (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) || (isMouseDown && !state.isPressed);

    if (isHovered && isPressedNow) {
        clicked = true;
        state.isPressed = true;
    }
    if (!isMouseDown) {
        state.isPressed = false;
    }

    _impl->lastItemHovered = isHovered;
    _impl->lastItemActive  = isHovered && isMouseDown;

    Clay__OpenElementWithId(elemId);
    Clay_ElementDeclaration decl = {
        .layout = {
            .sizing          = {.width = ToClaySizing(width), .height = CLAY_SIZING_FIXED(44)},
            .padding         = {24, 24, 10, 10},
            .childAlignment  = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
            .layoutDirection = CLAY_LEFT_TO_RIGHT
        },
        .backgroundColor = isHovered ? (state.isPressed ? ToClayColor(color + JPH::Vec4(0.22f, 0.22f, 0.22f, 0.0f)) : ToClayColor(color + JPH::Vec4(0.12f, 0.12f, 0.12f, 0.0f))) : ToClayColor(color),
        .cornerRadius    = {6, 6, 6, 6}
    };
    Clay__ConfigureOpenElement(decl);

    Text(label, 16.0f, {0.95f, 0.95f, 1.0f, 1.0f});

    Clay__CloseElement();
    return clicked;
}

bool Context::IsItemHovered() const noexcept {
    return _impl ? _impl->lastItemHovered : false;
}

bool Context::IsItemActive() const noexcept {
    return _impl ? _impl->lastItemActive : false;
}

bool Context::Checkbox(std::string_view label, bool& checked) noexcept {
    Clay_SetCurrentContext(_impl->clayContext);
    bool           changed = false;
    uint32_t       idNum   = static_cast<uint32_t>(HashCreativeWorkPath(label));
    Clay_ElementId elemId  = Clay_GetElementIdWithIndex(ToClayString("cb"), idNum);
    auto&          state   = _impl->GetState((static_cast<uint64_t>(idNum) << 32) | 0x00CB, _impl->engine.GetCurrentFrame());

    auto* input = _impl->engine.GetRegistry().GetSingleton<Components::InputStateComponent>();
    float mx = input ? input->mouseX : -1.0f;
    float my = input ? input->mouseY : -1.0f;
    bool isMouseDown = input && input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton));

    Clay_ElementData elemData = Clay_GetElementData(elemId);
    bool isHovered = Clay_Hovered() || Clay_PointerOver(elemId) ||
                     (elemData.found && elemData.boundingBox.width > 0.0f &&
                      mx >= elemData.boundingBox.x && mx <= (elemData.boundingBox.x + elemData.boundingBox.width) &&
                      my >= elemData.boundingBox.y && my <= (elemData.boundingBox.y + elemData.boundingBox.height));

    auto pointer = Clay_GetPointerState();
    bool isPressedNow = (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) || (isMouseDown && !state.isPressed);

    if (isHovered && isPressedNow) {
        checked = !checked;
        changed = true;
        state.isPressed = true;
    }
    if (!isMouseDown) {
        state.isPressed = false;
    }

    _impl->lastItemHovered = isHovered;
    _impl->lastItemActive  = isHovered && isMouseDown;

    BeginRow(8.0f);

    Clay__OpenElementWithId(elemId);
    Clay_ElementDeclaration decl = {
        .layout = {
            .sizing         = {.width = CLAY_SIZING_FIXED(22), .height = CLAY_SIZING_FIXED(22)},
            .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}
        },
        .backgroundColor = isHovered ? Clay_Color {55, 75, 105, 255} : Clay_Color {25, 35, 50, 255},
        .cornerRadius    = {4, 4, 4, 4}
    };
    Clay__ConfigureOpenElement(decl);

    if (checked) {
        Clay__OpenElement();
        Clay_ElementDeclaration mark = {
            .layout          = {.sizing = {.width = CLAY_SIZING_FIXED(14), .height = CLAY_SIZING_FIXED(14)}},
            .backgroundColor = {80, 160, 255, 255},
            .cornerRadius    = {2, 2, 2, 2}
        };
        Clay__ConfigureOpenElement(mark);
        Clay__CloseElement();
    }

    Clay__CloseElement();

    Text(label, 16.0f, {0.9f, 0.9f, 0.9f, 1.0f});

    EndRow();
    return changed;
}

bool Context::Slider(std::string_view label, float& value, float minVal, float maxVal) noexcept {
    Clay_SetCurrentContext(_impl->clayContext);
    bool           changed = false;
    uint32_t       idNum   = static_cast<uint32_t>(HashCreativeWorkPath(label));
    Clay_ElementId elemId  = Clay_GetElementIdWithIndex(ToClayString(label), idNum);

    uint64_t stateKey = (static_cast<uint64_t>(idNum) << 32) | 0x511D;
    auto&    state    = _impl->GetState(stateKey, _impl->engine.GetCurrentFrame());

    auto* input = _impl->engine.GetRegistry().GetSingleton<Components::InputStateComponent>();
    float mx = input ? input->mouseX : -1.0f;
    float my = input ? input->mouseY : -1.0f;
    bool isMouseDown = input && input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton));

    Clay_ElementData elemData = Clay_GetElementData(elemId);
    bool isHovered = Clay_Hovered() || Clay_PointerOver(elemId) ||
                     (elemData.found && elemData.boundingBox.width > 0.0f &&
                      mx >= elemData.boundingBox.x && mx <= (elemData.boundingBox.x + elemData.boundingBox.width) &&
                      my >= elemData.boundingBox.y && my <= (elemData.boundingBox.y + elemData.boundingBox.height));

    auto pointer = Clay_GetPointerState();
    bool isDown  = isMouseDown || (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME || pointer.state == CLAY_POINTER_DATA_PRESSED);

    if (isHovered && isDown) {
        state.isDragging = true;
    }
    if (!isDown) {
        state.isDragging = false;
    }

    if (state.isDragging && elemData.found && elemData.boundingBox.width > 0.0f) {
        float relX   = mx - elemData.boundingBox.x;
        float frac   = std::clamp(relX / elemData.boundingBox.width, 0.0f, 1.0f);
        float newVal = minVal + frac * (maxVal - minVal);
        if (std::abs(newVal - value) > 1e-5f) {
            value   = newVal;
            changed = true;
        }
    }

    BeginRow(8.0f);
    Text(label, 15.0f, {0.9f, 0.9f, 0.9f, 1.0f});

    Clay__OpenElementWithId(elemId);
    Clay_ElementDeclaration trackDecl = {
        .layout = {
            .sizing         = {.width = CLAY_SIZING_FIXED(160), .height = CLAY_SIZING_FIXED(22)},
            .padding        = {2, 2, 2, 2},
            .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER}
        },
        .backgroundColor = isHovered ? Clay_Color {55, 75, 105, 255} : Clay_Color {25, 35, 50, 255},
        .cornerRadius    = {4, 4, 4, 4}
    };
    Clay__ConfigureOpenElement(trackDecl);

    float frac      = (maxVal > minVal) ? std::clamp((value - minVal) / (maxVal - minVal), 0.0f, 1.0f) : 0.0f;
    float fillWidth = std::max(4.0f, frac * 156.0f);
    Clay__OpenElement();
    Clay_ElementDeclaration fillDecl = {
        .layout          = {.sizing = {.width = CLAY_SIZING_FIXED(fillWidth), .height = CLAY_SIZING_GROW()}},
        .backgroundColor = {80, 160, 255, 255},
        .cornerRadius    = {3, 3, 3, 3}
    };
    Clay__ConfigureOpenElement(fillDecl);
    Clay__CloseElement();

    Clay__CloseElement(); // track

    char valBuf[32];
    std::snprintf(valBuf, sizeof(valBuf), "%.2f", static_cast<double>(value));
    Text(valBuf, 14.0f, {0.7f, 0.7f, 0.7f, 1.0f});

    EndRow();
    return changed;
}

bool Context::BeginCollapsingHeader(std::string_view label, bool defaultOpen) noexcept {
    Clay_SetCurrentContext(_impl->clayContext);
    uint32_t       idNum  = static_cast<uint32_t>(HashCreativeWorkPath(label));
    Clay_ElementId elemId = Clay_GetElementIdWithIndex(ToClayString(label), idNum);

    uint64_t stateKey = (static_cast<uint64_t>(idNum) << 32) | 0xC011;
    auto&    state    = _impl->GetState(stateKey, _impl->engine.GetCurrentFrame());
    if (!state.isInitialized) {
        state.isOpen        = defaultOpen;
        state.isInitialized = true;
    }

    auto* input = _impl->engine.GetRegistry().GetSingleton<Components::InputStateComponent>();
    float mx = input ? input->mouseX : -1.0f;
    float my = input ? input->mouseY : -1.0f;
    bool isMouseDown = input && input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton));

    Clay_ElementData elemData = Clay_GetElementData(elemId);
    bool isHovered = Clay_Hovered() || Clay_PointerOver(elemId) ||
                     (elemData.found && elemData.boundingBox.width > 0.0f &&
                      mx >= elemData.boundingBox.x && mx <= (elemData.boundingBox.x + elemData.boundingBox.width) &&
                      my >= elemData.boundingBox.y && my <= (elemData.boundingBox.y + elemData.boundingBox.height));

    auto pointer = Clay_GetPointerState();
    bool isPressedNow = (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) || (isMouseDown && !state.isPressed);

    if (isHovered && isPressedNow) {
        state.isOpen    = !state.isOpen;
        state.isPressed = true;
    }
    if (!isMouseDown) {
        state.isPressed = false;
    }

    BeginColumn(4.0f);

    Clay__OpenElementWithId(elemId);
    Clay_ElementDeclaration headerDecl = {
        .layout =
            {.sizing          = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(32)},
             .padding         = {10, 10, 6, 6},
             .childGap        = 8,
             .childAlignment  = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER},
             .layoutDirection = CLAY_LEFT_TO_RIGHT},
        .backgroundColor = isHovered ? Clay_Color {55, 75, 105, 255} : Clay_Color {30, 40, 58, 255},
        .cornerRadius    = {4, 4, 4, 4}
    };
    Clay__ConfigureOpenElement(headerDecl);

    Text(state.isOpen ? "v" : ">", 15.0f, {0.8f, 0.8f, 0.8f, 1.0f});
    Text(label, 15.0f, {1.0f, 1.0f, 1.0f, 1.0f});

    Clay__CloseElement(); // Header bar

    if (state.isOpen) {
        BeginBox("", {.width = {.grow = 1.0f}, .padding = 8.0f, .gap = 4.0f, .direction = Direction::Column});
        return true;
    }

    EndColumn();
    return false;
}

void Context::EndCollapsingHeader() noexcept {
    EndBox();    // Indented container
    EndColumn(); // Outer column
}

} // namespace ZHLN::GUI
