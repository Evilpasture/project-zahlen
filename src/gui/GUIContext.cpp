// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#define CLAY_IMPLEMENTATION
#include "Text.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <algorithm>
#include <array>
#include <clay.h>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <limits>
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
    // Text fields keep their caret here rather than in the caller's string, so
    // a TextInput stays a plain `gui.TextInput("Name", str)` at the call site.
    TextEdit::Caret caret = {};
    // Keyboard highlight for an open Dropdown, in option indices.
    int32_t highlightIndex = 0;
};

// ============================================================================
// Context::Impl Definition (Owned per-engine or per-registry instance)
// ============================================================================
struct Context::Impl {
    ECS::Registry&                       registry;
    Extent2D                             viewport    = {.width = 1920, .height = 1080};
    Engine*                              engine      = nullptr;
    Clay_Context*                        clayContext = nullptr;
    Clay_Arena                           clayArena   = {};
    std::vector<std::byte>               arenaMemory;
    ZHLN::HashMap<uint64_t, WidgetState> widgetStates;
    const FontAtlas*                     activeFont      = nullptr;
    FontAtlas                            fallbackFont    = {};
    float                                lastDt          = 0.016667f;
    uint64_t                             currentFrame    = 0;
    bool                                 lastItemHovered = false;
    bool                                 lastItemActive  = false;
    bool                                 inLayout        = false;

    // --- Text input ---
    // State key of the field that currently holds focus; 0 means none. Only one
    // field is focused at a time, which is what lets a click on any field
    // defocus the previous one without a focus manager.
    uint64_t focusedTextInput = 0;
    // State key of the open Dropdown, or 0. One at a time, which is what lets a
    // click anywhere close it without a separate dismiss layer.
    uint64_t openDropdown = 0;
    // Ctrl+C/X/V plumbing, installed by the front end (the engine wires it to
    // Window's clipboard). Empty means those three keys do nothing.
    TextEdit::ClipboardSink clipboard = {};

    // --- String interning ---------------------------------------------------
    // Widget labels routinely arrive as temporaries: a FormatTo into a stack
    // array, a view into a stack copy of a component. Clay stores only the
    // pointer and dereferences it in EndFrameAndRender, after the caller that
    // owned the bytes has returned -- the dangling read showed up on screen as
    // runs of '?' because MeasureText maps bytes outside 32..127 to '?'.
    // Every string handed to Clay is therefore copied in on the way through,
    // and the arena is dropped at the top of the next BeginFrame, by which time
    // the previous frame's render commands have all been consumed. Each interned
    // string needs its OWN contiguous buffer: a flat std::deque<char> keeps
    // element pointers stable but stores bytes in 4096-byte chunks, so a label
    // straddling a chunk boundary handed Clay a chars pointer whose read runs
    // off the end of the block (ASan heap-buffer-overflow in Clay's hasher).
    // std::deque<std::string> gives both properties: emplace_back never moves
    // an already-constructed element, and each std::string owns contiguous bytes.
    std::deque<std::string> stringArena;

    auto Intern(std::string_view sv) -> Clay_String {
        auto& stored = stringArena.emplace_back(sv);
        stored.push_back('\0');
        return Clay_String {.isStaticallyAllocated = false, .length = static_cast<int32_t>(sv.size()), .chars = stored.data()};
    }

    // Characters and editing keys arrive from the window between frames, not
    // through InputStateComponent, which only tracks held-down state. They
    // queue here, are drained by whichever field is focused during the next
    // frame, and whatever is left is dropped at the end of it so a keypress can
    // never leak into a field focused later. Fixed size: a key repeat cannot
    // outrun a frame by more than a handful of events, and growing here would
    // put an allocation in the input path.
    struct PendingEvent {
        bool     isChar    = false;
        uint32_t key       = 0;
        uint32_t codepoint = 0;
        // Set by the widget that acted on it. Without this a frame that draws
        // both a focused text field and an open dropdown would apply the same
        // Enter to both -- committing the field and closing the list.
        bool consumed = false;
    };
    static constexpr size_t                     kMaxPendingEvents = 64;
    std::array<PendingEvent, kMaxPendingEvents> pendingEvents     = {};
    size_t                                      pendingEventCount = 0;

    void QueueEvent(const PendingEvent& ev) noexcept {
        if (pendingEventCount < kMaxPendingEvents) {
            pendingEvents[pendingEventCount++] = ev;
        }
    }

    void ClearPendingEvents() noexcept {
        pendingEventCount = 0;
    }

    explicit Impl(ECS::Registry& reg, Extent2D vp = {.width = 1920, .height = 1080}, Engine* eng = nullptr) noexcept: registry(reg), viewport(vp), engine(eng) {
        for (auto& glyph: fallbackFont.glyphs) {
            glyph.xadvance = 18.0f;
        }
    }

    ~Impl() noexcept {
        if (Clay_GetCurrentContext() == clayContext) {
            Clay_SetCurrentContext(nullptr);
        }
        clayContext = nullptr;
    }

    auto GetState(uint64_t id, uint64_t frame) noexcept -> WidgetState& {
        auto* state = widgetStates.Find(id);
        if (state == nullptr) {
            widgetStates.Insert(id, WidgetState {});
            state = widgetStates.Find(id);
        }
        ZHLN::Assert(state != nullptr);
        state->lastActiveFrame = frame;
        return *state;
    }

    void PruneStaleStates(uint64_t frame) noexcept {
        widgetStates.ForEach([&](uint64_t id, const WidgetState& s) -> void {
            if (frame > s.lastActiveFrame + 60) {
                widgetStates.Erase(id);
            }
        });
    }

    static auto MeasureText(Clay_StringSlice text, Clay_TextElementConfig* config, void* userData) -> Clay_Dimensions {
        auto* impl = static_cast<Impl*>(userData);
        if ((impl == nullptr) || (impl->activeFont == nullptr) || text.length == 0) {
            return {0.0f, 0.0f};
        }

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
                totalH += lineHeight;
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

auto ToClaySizing(const Sizing& s) noexcept -> Clay_SizingAxis {
    if (s.fixed > 0.0f) {
        return CLAY_SIZING_FIXED(s.fixed);
    }
    if (s.grow > 0.0f) {
        return CLAY_SIZING_GROW();
    }
    return CLAY_SIZING_FIT();
}

auto ToClayColor(const JPH::Vec4& c) noexcept -> Clay_Color {
    return {(c.GetX() * 255.0f), (c.GetY() * 255.0f), (c.GetZ() * 255.0f), (c.GetW() * 255.0f)};
}

auto ToClayAlignX(Alignment align) noexcept -> Clay_LayoutAlignmentX {
    switch (align) {
        case Alignment::Center:
            return CLAY_ALIGN_X_CENTER;
        case Alignment::End:
            return CLAY_ALIGN_X_RIGHT;
        default:
            return CLAY_ALIGN_X_LEFT;
    }
}

auto ToClayAlignY(Alignment align) noexcept -> Clay_LayoutAlignmentY {
    switch (align) {
        case Alignment::Center:
            return CLAY_ALIGN_Y_CENTER;
        case Alignment::End:
            return CLAY_ALIGN_Y_BOTTOM;
        default:
            return CLAY_ALIGN_Y_TOP;
    }
}

auto ToClayChildAlignment(const BoxConfig& cfg) noexcept -> Clay_ChildAlignment {
    if (cfg.direction == Direction::Row) {
        return {.x = ToClayAlignX(cfg.alignMain), .y = ToClayAlignY(cfg.alignCross)};
    }
    return {.x = ToClayAlignX(cfg.alignCross), .y = ToClayAlignY(cfg.alignMain)};
}

struct GUIStateComponent {
    std::unique_ptr<Context::Impl> impl;
};

} // namespace

// ============================================================================
// Lifecycle Methods
// ============================================================================

Context::Context(Engine& engine) noexcept {
    auto& reg   = engine.GetRegistry();
    auto& state = reg.GetOrEmplaceSingleton<GUIStateComponent>();
    if (!state.impl) {
        state.impl = std::make_unique<Impl>(reg, engine.GetWindow().GetSize(), &engine);
    } else {
        state.impl->viewport = engine.GetWindow().GetSize();
        state.impl->engine   = &engine;
    }
    _impl = state.impl.get();
}

Context::Context(ECS::Registry& registry, Extent2D viewport) noexcept {
    auto& state = registry.GetOrEmplaceSingleton<GUIStateComponent>();
    if (!state.impl) {
        state.impl = std::make_unique<Impl>(registry, viewport, nullptr);
    } else {
        state.impl->viewport = viewport;
        // Preview hosts pass a size without an Engine so BeginFrame uses the
        // supplied viewport instead of the editor window.
        state.impl->engine = nullptr;
    }
    _impl = state.impl.get();
}

void Context::BeginFrame(float dt) noexcept {
    _impl->currentFrame++;
    _impl->stringArena.clear();
    _impl->lastDt    = dt;
    Extent2D winSize = _impl->viewport;
    if (_impl->engine != nullptr) {
        winSize = _impl->engine->GetWindow().GetSize();
    }
    auto* input    = _impl->registry.GetSingleton<Components::InputStateComponent>();
    auto* settings = _impl->registry.GetSingleton<UISettingsComponent>();

    // Fold whatever the window's event pump queued since the last frame into
    // this Context's own queue, so hardware input and PushKey/PushChar (tests,
    // headless hosts) converge on the one mechanism TextInput consumes.
    if (input != nullptr && input->queuedInputCount > 0) {
        for (size_t i = 0; i < input->queuedInputCount; ++i) {
            const auto& queued = input->queuedInput[i];
            if (queued.isChar) {
                _impl->QueueEvent(Impl::PendingEvent {.isChar = true, .key = 0, .codepoint = queued.value});
            } else {
                _impl->QueueEvent(Impl::PendingEvent {.isChar = false, .key = queued.value, .codepoint = 0});
            }
        }
        input->ClearQueuedInput();
    }

    if ((settings != nullptr) && settings->fontAtlas.glyphs[0].xadvance > 0.0f) {
        _impl->activeFont = &settings->fontAtlas;
    } else {
        _impl->activeFont = &_impl->fallbackFont;
    }

    // Lazily allocate Clay memory arena on this instance once
    if (_impl->clayContext == nullptr) {
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

    if (_impl->inLayout) {
        Clay_EndLayout(_impl->lastDt);
        _impl->inLayout = false;
    }

    Clay_SetLayoutDimensions({static_cast<float>(winSize.width), static_cast<float>(winSize.height)});

    float mx          = (input != nullptr) ? input->mouseX : -1.0f;
    float my          = (input != nullptr) ? input->mouseY : -1.0f;
    bool  isMouseDown = (input != nullptr) && input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton));
    // Raw wheel: GetMouseWheel is gated by wantCaptureMouse for gameplay.
    float wheel = (input != nullptr) ? input->mouseWheel : 0.0f;

    Clay_SetPointerState(Clay_Vector2 {mx, my}, isMouseDown);
    Clay_UpdateScrollContainers(false, Clay_Vector2 {0.0f, wheel * 30.0f}, dt);

    _impl->PruneStaleStates(_impl->currentFrame);
    Clay_BeginLayout();
    _impl->inLayout = true;
}

void Context::EndFrame() noexcept {
    if ((_impl == nullptr) || (_impl->clayContext == nullptr) || !_impl->inLayout) {
        return;
    }
    Clay_SetCurrentContext(_impl->clayContext);
    Clay_EndLayout(_impl->lastDt);
    _impl->inLayout = false;
    _impl->ClearPendingEvents();
}

void Context::EndFrameAndRender(RenderContext& rc) noexcept {
    if ((_impl == nullptr) || (_impl->clayContext == nullptr) || !_impl->inLayout) {
        return;
    }
    Clay_SetCurrentContext(_impl->clayContext);
    Clay_RenderCommandArray commands = Clay_EndLayout(_impl->lastDt);
    _impl->inLayout                  = false;
    _impl->ClearPendingEvents();
    if (commands.length == 0 || (_impl->activeFont == nullptr)) {
        return;
    }

    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    std::vector<UIBatch>          batches;

    positions.reserve(static_cast<size_t>(commands.length) * 6);
    attributes.reserve(static_cast<size_t>(commands.length) * 6);

    auto EmitQuad = [&](float x0, float y0, float x1, float y1, JPH::Vec4 color) -> void {
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

                auto startIdx = static_cast<uint32_t>(positions.size());
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

    if (cfg.offsetX != 0.0f || cfg.offsetY != 0.0f) {
        decl.floating = {
            .offset       = {cfg.offsetX, cfg.offsetY},
            .zIndex       = 1,
            .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP},
            .attachTo     = CLAY_ATTACH_TO_PARENT
        };
    }

    if (!id.empty()) {
        auto           numId  = static_cast<uint32_t>(HashCreativeWorkPath(id));
        Clay_ElementId elemId = Clay_GetElementIdWithIndex(_impl->Intern(id), numId);
        Clay__OpenElementWithId(elemId);
    } else {
        Clay__OpenElement();
    }

    if (cfg.clipVertical) {
        decl.clip.vertical    = true;
        decl.clip.childOffset = Clay_GetScrollOffset();
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
    Clay__OpenTextElement(_impl->Intern(text), config);
}

auto Context::Button(std::string_view label, const JPH::Vec4& color, const Sizing& width, std::string_view id) noexcept -> bool {
    Clay_SetCurrentContext(_impl->clayContext);
    bool                   clicked = false;
    const std::string_view key     = id.empty() ? label : id;
    auto                   idNum   = static_cast<uint32_t>(HashCreativeWorkPath(key));
    Clay_ElementId         elemId  = Clay_GetElementIdWithIndex(_impl->Intern(key), idNum);
    auto&                  state   = _impl->GetState((static_cast<uint64_t>(idNum) << 32) | 0xB007, _impl->currentFrame);

    auto* input       = _impl->registry.GetSingleton<Components::InputStateComponent>();
    float mx          = (input != nullptr) ? input->mouseX : -1.0f;
    float my          = (input != nullptr) ? input->mouseY : -1.0f;
    bool  isMouseDown = (input != nullptr) && input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton));

    Clay__OpenElementWithId(elemId);

    Clay_ElementData elemData  = Clay_GetElementData(elemId);
    bool             isHovered = Clay_Hovered() || Clay_PointerOver(elemId) ||
                                 (elemData.found && elemData.boundingBox.width > 0.0f && mx >= elemData.boundingBox.x &&
                                  mx <= (elemData.boundingBox.x + elemData.boundingBox.width) && my >= elemData.boundingBox.y &&
                                  my <= (elemData.boundingBox.y + elemData.boundingBox.height));

    auto pointer      = Clay_GetPointerState();
    bool isPressedNow = (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) || (isHovered && isMouseDown && !state.isPressed);

    if (isHovered && isPressedNow) {
        clicked = true;
    }
    state.isPressed = isMouseDown;

    _impl->lastItemHovered = isHovered;
    _impl->lastItemActive  = isHovered && isMouseDown;

    Clay_ElementDeclaration decl = {
        .layout =
            {.sizing          = {.width = ToClaySizing(width), .height = CLAY_SIZING_FIXED(44)},
             .padding         = {24, 24, 10, 10},
             .childAlignment  = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
             .layoutDirection = CLAY_LEFT_TO_RIGHT},
        .backgroundColor =
            isHovered ? (isMouseDown ? ToClayColor(color + JPH::Vec4(0.22f, 0.22f, 0.22f, 0.0f)) : ToClayColor(color + JPH::Vec4(0.12f, 0.12f, 0.12f, 0.0f))) :
                        ToClayColor(color),
        .cornerRadius = {6, 6, 6, 6}
    };
    Clay__ConfigureOpenElement(decl);

    Text(label, 16.0f, {0.95f, 0.95f, 1.0f, 1.0f});

    Clay__CloseElement();
    return clicked;
}

auto Context::IsItemHovered() const noexcept -> bool {
    return (_impl != nullptr) ? _impl->lastItemHovered : false;
}

auto Context::GetLastFrameRect(std::string_view id) const noexcept -> std::optional<ElementRect> {
    // Unlike the widget methods, this getter is designed to be called OUTSIDE
    // a layout pass -- the editor reads panel rects before its first
    // BeginFrame, when the (lazily created) Clay context does not exist yet
    // and Clay's current-context pointer is null. No context means there is
    // no last-frame data to read; anything else must not touch Clay.
    if ((_impl == nullptr) || (_impl->clayContext == nullptr)) {
        return std::nullopt;
    }
    Clay_SetCurrentContext(_impl->clayContext);

    // Same element-id recipe as every widget above (hash the path, index the
    // string), so the id a Box was opened with resolves here. The string only
    // feeds the hash -- no interning needed for a read.
    Clay_String cs {};
    cs.length                   = static_cast<int32_t>(id.size());
    cs.chars                    = id.data();
    const Clay_ElementData data = Clay_GetElementData(Clay_GetElementIdWithIndex(cs, static_cast<uint32_t>(HashCreativeWorkPath(id))));
    if (!data.found) {
        return std::nullopt;
    }
    return ElementRect {.x = data.boundingBox.x, .y = data.boundingBox.y, .width = data.boundingBox.width, .height = data.boundingBox.height};
}

auto Context::IsItemActive() const noexcept -> bool {
    return (_impl != nullptr) ? _impl->lastItemActive : false;
}

auto Context::IsPointerOver(std::string_view id) const noexcept -> bool {
    const auto rect = GetLastFrameRect(id);
    if (!rect || (_impl == nullptr)) {
        return false;
    }
    auto*       input = _impl->registry.GetSingleton<Components::InputStateComponent>();
    const float mx    = (input != nullptr) ? input->mouseX : -1.0f;
    const float my    = (input != nullptr) ? input->mouseY : -1.0f;
    return mx >= rect->x && mx <= (rect->x + rect->width) && my >= rect->y && my <= (rect->y + rect->height);
}

auto Context::IsPointerPressedThisFrame() const noexcept -> bool {
    if ((_impl == nullptr) || (_impl->clayContext == nullptr)) {
        return false;
    }
    Clay_SetCurrentContext(_impl->clayContext);
    return Clay_GetPointerState().state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME;
}

auto Context::Checkbox(std::string_view label, bool& checked, std::string_view id) noexcept -> bool {
    Clay_SetCurrentContext(_impl->clayContext);
    bool                   changed = false;
    const std::string_view key     = id.empty() ? label : id;
    auto                   idNum   = static_cast<uint32_t>(HashCreativeWorkPath(key));
    Clay_ElementId         elemId  = Clay_GetElementIdWithIndex(_impl->Intern(key), idNum);
    auto&                  state   = _impl->GetState((static_cast<uint64_t>(idNum) << 32) | 0x00CB, _impl->currentFrame);

    auto* input       = _impl->registry.GetSingleton<Components::InputStateComponent>();
    float mx          = (input != nullptr) ? input->mouseX : -1.0f;
    float my          = (input != nullptr) ? input->mouseY : -1.0f;
    bool  isMouseDown = (input != nullptr) && input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton));

    BeginRow(8.0f);

    Clay__OpenElementWithId(elemId);

    Clay_ElementData elemData  = Clay_GetElementData(elemId);
    bool             isHovered = Clay_Hovered() || Clay_PointerOver(elemId) ||
                                 (elemData.found && elemData.boundingBox.width > 0.0f && mx >= elemData.boundingBox.x &&
                                  mx <= (elemData.boundingBox.x + elemData.boundingBox.width) && my >= elemData.boundingBox.y &&
                                  my <= (elemData.boundingBox.y + elemData.boundingBox.height));

    auto pointer      = Clay_GetPointerState();
    bool isPressedNow = (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) || (isHovered && isMouseDown && !state.isPressed);

    if (isHovered && isPressedNow) {
        checked = !checked;
        changed = true;
    }
    state.isPressed = isMouseDown;

    _impl->lastItemHovered = isHovered;
    _impl->lastItemActive  = isHovered && isMouseDown;

    Clay_ElementDeclaration decl = {
        .layout =
            {.sizing         = {.width = CLAY_SIZING_FIXED(22), .height = CLAY_SIZING_FIXED(22)},
             .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
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

auto Context::Slider(std::string_view label, float& value, float minVal, float maxVal, std::string_view id) noexcept -> bool {
    Clay_SetCurrentContext(_impl->clayContext);
    bool                   changed = false;
    const std::string_view key     = id.empty() ? label : id;
    auto                   idNum   = static_cast<uint32_t>(HashCreativeWorkPath(key));
    Clay_ElementId         elemId  = Clay_GetElementIdWithIndex(_impl->Intern(key), idNum);

    uint64_t stateKey = (static_cast<uint64_t>(idNum) << 32) | 0x511D;
    auto&    state    = _impl->GetState(stateKey, _impl->currentFrame);

    auto* input       = _impl->registry.GetSingleton<Components::InputStateComponent>();
    float mx          = (input != nullptr) ? input->mouseX : -1.0f;
    float my          = (input != nullptr) ? input->mouseY : -1.0f;
    bool  isMouseDown = (input != nullptr) && input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton));

    // Fixed label column so tracks share a left edge; grow the track into the
    // leftover; 48px value on the right. Gap 4 instead of 8 so the number sits
    // against the track instead of a padded strip.
    BeginRow(4.0f);
    BeginBox("", {.width = {.fixed = 96.0f}, .height = {.fixed = 22.0f}, .alignMain = Alignment::Center, .alignCross = Alignment::Start});
    Text(label, 15.0f, {0.9f, 0.9f, 0.9f, 1.0f});
    EndBox();

    Clay__OpenElementWithId(elemId);

    Clay_ElementData elemData  = Clay_GetElementData(elemId);
    bool             isHovered = Clay_Hovered() || Clay_PointerOver(elemId) ||
                                 (elemData.found && elemData.boundingBox.width > 0.0f && mx >= elemData.boundingBox.x &&
                                  mx <= (elemData.boundingBox.x + elemData.boundingBox.width) && my >= elemData.boundingBox.y &&
                                  my <= (elemData.boundingBox.y + elemData.boundingBox.height));

    auto pointer = Clay_GetPointerState();

    if (isHovered && ((pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) || (isMouseDown && !state.isDragging))) {
        state.isDragging = true;
    }
    if (!isMouseDown) {
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

    _impl->lastItemHovered = isHovered;
    _impl->lastItemActive  = state.isDragging;

    Clay_ElementDeclaration trackDecl = {
        .layout =
            {.sizing         = {.width = CLAY_SIZING_GROW(120), .height = CLAY_SIZING_FIXED(22)},
             .padding        = {2, 2, 2, 2},
             .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER}},
        .backgroundColor = (isHovered || state.isDragging) ? Clay_Color {55, 75, 105, 255} : Clay_Color {25, 35, 50, 255},
        .cornerRadius    = {4, 4, 4, 4}
    };
    Clay__ConfigureOpenElement(trackDecl);

    const float trackInner = elemData.found ? std::max(4.0f, elemData.boundingBox.width - 4.0f) : 80.0f;
    float       frac       = (maxVal > minVal) ? std::clamp((value - minVal) / (maxVal - minVal), 0.0f, 1.0f) : 0.0f;
    float       fillWidth  = std::max(4.0f, frac * trackInner);
    Clay__OpenElement();
    Clay_ElementDeclaration fillDecl = {
        .layout          = {.sizing = {.width = CLAY_SIZING_FIXED(fillWidth), .height = CLAY_SIZING_GROW()}},
        .backgroundColor = {80, 160, 255, 255},
        .cornerRadius    = {3, 3, 3, 3}
    };
    Clay__ConfigureOpenElement(fillDecl);
    Clay__CloseElement();

    Clay__CloseElement(); // track

    std::array<char, 32> valBuf {};
    ZHLN::FormatTo(valBuf, "{.2f}", value);

    BeginBox("", {.width = {.fixed = 48.0f}, .height = {.fixed = 22.0f}, .alignMain = Alignment::Center, .alignCross = Alignment::End});
    Text(valBuf.data(), 14.0f, {0.7f, 0.7f, 0.7f, 1.0f});
    EndBox();

    EndRow();
    return changed;
}

// --- Text Input ---

namespace {

// Field geometry, in pixels. The caret is placed by measuring prefixes at this
// size, so it has to stay in step with the size Text() is called with below.
constexpr float kTextInputFontSize = 15.0f;
constexpr float kTextInputPadding  = 4.0f;
constexpr float kTextInputHeight   = 24.0f;
constexpr float kTextInputWidth    = 180.0f;

// Dropdown geometry. The list floats at kDropdownListOffset from the field, and
// both the drawing and the hit-testing below derive row rectangles from that
// same arithmetic -- no per-row Clay elements, so no per-row ids whose
// Clay_String would have to outlive the layout pass.
constexpr float kDropdownHeight     = 24.0f;
constexpr float kDropdownWidth      = 160.0f;
constexpr float kDropdownRowHeight  = 20.0f;
constexpr float kDropdownListOffset = 2.0f;
constexpr int   kDropdownMaxVisible = 8;

/// How far the pen moves for one glyph, matching Impl::MeasureText so the caret
/// lands where the text is actually drawn.
///
/// MeasureTextBounds is the wrong tool for this: it returns the ink bounding box
/// (maxX - minX), not the pen advance, so it under-measures proportional fonts
/// and measures zero for any atlas whose glyph rects are unset even though the
/// advances are fine -- which is exactly the fallback atlas.
[[nodiscard]] inline auto GlyphAdvance(const FontAtlas& font, char c, float scale) noexcept -> float {
    uint32_t glyphCode = static_cast<uint8_t>(c);
    if (glyphCode < 32 || glyphCode > 127) {
        glyphCode = '?';
    }
    return font.glyphs[glyphCode - 32].xadvance * scale;
}

/// Byte offset whose glyph boundary is nearest to `localX` pixels into `text`.
[[nodiscard]] inline auto CaretIndexAtX(const FontAtlas& font, std::string_view text, float localX, float scale) noexcept -> size_t {
    float  pen = 0.0f;
    size_t idx = 0;
    while (idx < text.size()) {
        const float advance = GlyphAdvance(font, text[idx], scale);
        if (pen + advance * 0.5f > localX) {
            break;
        }
        pen += advance;
        ++idx;
    }
    return idx;
}

} // namespace

auto Context::TextInputImpl(std::string_view label, std::string& value, size_t maxTextLength, const Sizing& width, std::string_view id) noexcept -> bool {
    Clay_SetCurrentContext(_impl->clayContext);

    const std::string_view key      = id.empty() ? label : id;
    auto                   idNum    = static_cast<uint32_t>(HashCreativeWorkPath(key));
    Clay_ElementId         elemId   = Clay_GetElementIdWithIndex(_impl->Intern(key), idNum);
    const uint64_t         stateKey = (static_cast<uint64_t>(idNum) << 32) | 0x7E17;
    auto&                  state    = _impl->GetState(stateKey, _impl->currentFrame);

    auto* input = _impl->registry.GetSingleton<Components::InputStateComponent>();
    float mx    = (input != nullptr) ? input->mouseX : -1.0f;
    float my    = (input != nullptr) ? input->mouseY : -1.0f;

    // The caller owns the string and may have reassigned it since the last
    // frame; a caret left outside the text would make every offset below wrong.
    state.caret.cursorIndex     = static_cast<uint32_t>(std::min<size_t>(state.caret.cursorIndex, value.size()));
    state.caret.selectionAnchor = static_cast<uint32_t>(std::min<size_t>(state.caret.selectionAnchor, value.size()));

    bool changed = false;

    BeginRow(8.0f);
    Text(label, 15.0f, {0.9f, 0.9f, 0.9f, 1.0f});

    Clay__OpenElementWithId(elemId);

    Clay_ElementData elemData  = Clay_GetElementData(elemId);
    bool             isHovered = Clay_Hovered() || Clay_PointerOver(elemId) ||
                                 (elemData.found && elemData.boundingBox.width > 0.0f && mx >= elemData.boundingBox.x &&
                                  mx <= (elemData.boundingBox.x + elemData.boundingBox.width) && my >= elemData.boundingBox.y &&
                                  my <= (elemData.boundingBox.y + elemData.boundingBox.height));

    auto       pointer          = Clay_GetPointerState();
    const bool pressedThisFrame = (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME);
    const bool wasFocused       = (_impl->focusedTextInput == stateKey);
    bool       isFocused        = wasFocused;

    if (pressedThisFrame) {
        // Whichever field the click lands on takes focus. A field that was
        // focused and was not clicked gives it up, and does so without
        // clobbering a focus another field has already claimed this frame --
        // draw order is not knowable here, so the claim is only ever written by
        // the field that was actually hit.
        if (isHovered) {
            isFocused               = true;
            _impl->focusedTextInput = stateKey;
            state.caret.selectAll   = false;
            // Place the caret where the click landed: walk prefixes until the
            // measured width passes the click, which is the same measurement
            // the layout used, so the bar sits where the glyphs are.
            if (elemData.found && _impl->activeFont != nullptr) {
                const float scale       = kTextInputFontSize / 32.0f;
                const float localX      = std::max(0.0f, mx - (elemData.boundingBox.x + kTextInputPadding));
                state.caret.cursorIndex = static_cast<uint32_t>(CaretIndexAtX(*_impl->activeFont, std::string_view(value), localX, scale));
            }
            state.caret.ClearSelection();
        } else {
            isFocused = false;
            if (_impl->focusedTextInput == stateKey) {
                _impl->focusedTextInput = 0;
            }
        }
    }

    // Drain the events the window delivered since the last frame. Only the
    // focused field consumes them; EndFrame drops whatever nobody took.
    if (isFocused && _impl->pendingEventCount > 0) {
        TextEdit::Modifiers mods {};
        if (input != nullptr) {
            mods.shift = input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::LShift)) || input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::RShift));
            mods.ctrl  = input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::LControl)) || input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::RControl));
        }
        // Edited through a bounded view so a fixed-capacity caller's limit
        // shortens the paste instead of the assign eating the buffer's tail.
        TextEdit::BoundedString buf {.text = &value, .maxLength = maxTextLength};
        for (size_t i = 0; i < _impl->pendingEventCount; ++i) {
            auto& ev = _impl->pendingEvents[i];
            if (ev.consumed) {
                continue;
            }
            ev.consumed = true;
            if (ev.isChar) {
                if (TextEdit::HandleChar(buf, state.caret, ev.codepoint)) {
                    changed = true;
                }
                continue;
            }
            const auto result = TextEdit::HandleKey(buf, state.caret, static_cast<KeyCode>(ev.key), mods, _impl->clipboard);
            if (result == TextEdit::KeyResult::Edited) {
                changed = true;
            }
            if (result == TextEdit::KeyResult::Committed) {
                isFocused               = false;
                _impl->focusedTextInput = 0;
            }
        }
    }

    _impl->lastItemHovered = isHovered;
    _impl->lastItemActive  = isFocused;

    const float             fieldWidth = (width.fixed > 0.0f) ? width.fixed : kTextInputWidth;
    Clay_ElementDeclaration fieldDecl  = {
        .layout =
            {.sizing =
                 {.width = (width.grow > 0.0f) ? CLAY_SIZING_GROW(width.grow) : CLAY_SIZING_FIXED(fieldWidth), .height = CLAY_SIZING_FIXED(kTextInputHeight)},
             .padding =
                 {static_cast<uint16_t>(kTextInputPadding), static_cast<uint16_t>(kTextInputPadding), static_cast<uint16_t>(kTextInputPadding),
                  static_cast<uint16_t>(kTextInputPadding)},
             .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER}},
        .backgroundColor = isFocused ? Clay_Color {40, 56, 80, 255} : Clay_Color {25, 35, 50, 255},
        .cornerRadius    = {4, 4, 4, 4}
    };
    Clay__ConfigureOpenElement(fieldDecl);

    // Draw the text in up to three runs (before / selected / after) with the
    // caret bar spliced in at the caret, so selection and caret are visible
    // without a floating overlay layer.
    const auto   all      = std::string_view(value);
    const bool   hasSel   = state.caret.HasSelection();
    const size_t selStart = hasSel ? state.caret.SelectionStart() : all.size();
    const size_t selEnd   = hasSel ? state.caret.SelectionEnd(all.size()) : all.size();
    const size_t caretIdx = std::min<size_t>(state.caret.cursorIndex, all.size());

    const JPH::Vec4 plainColor {0.9f, 0.9f, 0.9f, 1.0f};
    const JPH::Vec4 selectedColor {1.0f, 1.0f, 1.0f, 1.0f};

    size_t emitted    = 0;
    bool   caretDrawn = false;

    auto drawCaret = [&]() -> void {
        if (!isFocused || caretDrawn) {
            return;
        }
        caretDrawn = true;
        Clay__OpenElement();
        Clay_ElementDeclaration caretDecl = {
            .layout          = {.sizing = {.width = CLAY_SIZING_FIXED(1), .height = CLAY_SIZING_FIXED(kTextInputFontSize + 2.0f)}},
            .backgroundColor = {230, 230, 230, 255}
        };
        Clay__ConfigureOpenElement(caretDecl);
        Clay__CloseElement();
    };

    auto emitRun = [&](size_t from, size_t to, const JPH::Vec4& color) -> void {
        if (from >= to) {
            return;
        }
        // Split this run at the caret when the caret falls strictly inside it.
        const size_t split = (caretIdx > emitted && caretIdx <= to) ? caretIdx : to;
        Text(all.substr(from, split - from), kTextInputFontSize, color);
        if (split < to) {
            drawCaret();
            Text(all.substr(split, to - split), kTextInputFontSize, color);
        } else if (caretIdx == to) {
            drawCaret();
        }
        emitted = to;
    };

    if (caretIdx == 0) {
        drawCaret();
    }
    if (hasSel) {
        emitRun(0, selStart, plainColor);
        emitRun(selStart, selEnd, selectedColor);
        emitRun(selEnd, all.size(), plainColor);
    } else {
        emitRun(0, all.size(), plainColor);
    }
    drawCaret(); // Caret at the end, or an empty field. No-op once drawn.

    Clay__CloseElement(); // field
    EndRow();
    return changed;
}

auto Context::TextInput(std::string_view label, std::string& value, const Sizing& width, std::string_view id) noexcept -> bool {
    return TextInputImpl(label, value, std::numeric_limits<size_t>::max(), width, id);
}

void Context::PushKey(KeyCode key, bool pressed) noexcept {
    if ((_impl == nullptr) || !pressed) {
        return; // The editing rules act on presses and repeats, not releases.
    }
    _impl->QueueEvent({.isChar = false, .key = static_cast<uint32_t>(key), .codepoint = 0});
}

void Context::PushChar(unsigned int codepoint) noexcept {
    if (_impl == nullptr) {
        return;
    }
    _impl->QueueEvent({.isChar = true, .key = 0, .codepoint = codepoint});
}

void Context::SetClipboard(TextEdit::ClipboardSink sink) noexcept {
    if (_impl != nullptr) {
        _impl->clipboard = sink;
    }
}

auto Context::IsTextInputFocused() const noexcept -> bool {
    return (_impl != nullptr) && _impl->focusedTextInput != 0;
}

// --- Dropdown ---

auto Context::Dropdown(std::string_view label, std::span<const std::string_view> options, int& selected, const Sizing& width) noexcept -> bool {
    Clay_SetCurrentContext(_impl->clayContext);

    const int optionCount = static_cast<int>(options.size());

    BeginRow(8.0f);
    Text(label, 15.0f, {0.9f, 0.9f, 0.9f, 1.0f});

    if (optionCount == 0) {
        // Nothing to choose from. Still draw something so the row does not
        // silently vanish from the panel when an enum has no enumerators.
        Text("(no options)", 14.0f, {0.5f, 0.5f, 0.5f, 1.0f});
        EndRow();
        _impl->lastItemHovered = false;
        _impl->lastItemActive  = false;
        return false;
    }

    auto           idNum    = static_cast<uint32_t>(HashCreativeWorkPath(label));
    Clay_ElementId elemId   = Clay_GetElementIdWithIndex(_impl->Intern(label), idNum);
    const uint64_t stateKey = (static_cast<uint64_t>(idNum) << 32) | 0xD209;
    auto&          state    = _impl->GetState(stateKey, _impl->currentFrame);

    auto* input = _impl->registry.GetSingleton<Components::InputStateComponent>();
    float mx    = (input != nullptr) ? input->mouseX : -1.0f;
    float my    = (input != nullptr) ? input->mouseY : -1.0f;

    // The caller owns the index, so it can arrive out of range -- a shrunk enum
    // or an uninitialised field. Clamp before anything indexes with it.
    selected             = std::clamp(selected, 0, optionCount - 1);
    state.highlightIndex = std::clamp(state.highlightIndex, 0, optionCount - 1);

    const float fieldWidth = (width.fixed > 0.0f) ? width.fixed : kDropdownWidth;
    bool        changed    = false;

    const bool wasOpen = (_impl->openDropdown == stateKey);
    bool       isOpen  = wasOpen;

    Clay__OpenElementWithId(elemId);

    Clay_ElementData elemData  = Clay_GetElementData(elemId);
    bool             isHovered = Clay_Hovered() || Clay_PointerOver(elemId) ||
                                 (elemData.found && elemData.boundingBox.width > 0.0f && mx >= elemData.boundingBox.x &&
                                  mx <= (elemData.boundingBox.x + elemData.boundingBox.width) && my >= elemData.boundingBox.y &&
                                  my <= (elemData.boundingBox.y + elemData.boundingBox.height));

    auto       pointer          = Clay_GetPointerState();
    const bool pressedThisFrame = (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME);

    // Rows are windowed so a long enum (KeyCode has 72) does not produce a list
    // taller than the window, with the highlight kept in view.
    const int visibleCount = std::min(optionCount, kDropdownMaxVisible);
    const int windowStart  = (optionCount <= kDropdownMaxVisible) ? 0 :
                                                                    std::clamp(state.highlightIndex - kDropdownMaxVisible / 2, 0, optionCount - visibleCount);

    // Row rectangles come from the field's box plus the same offsets the
    // floating list is drawn with. Clay reports last frame's layout, which is
    // exactly what hit-testing needs.
    // The list opens downward unless it does not fit: a dropdown near the
    // bottom of the viewport (the inspector's Add Component) would otherwise
    // put every row off-screen. In that case it flips upward when the room
    // above is at least the room below.
    const float listX           = elemData.boundingBox.x;
    const float listHeight      = static_cast<float>(visibleCount) * kDropdownRowHeight;
    const float spaceBelow      = Clay_GetLayoutDimensions().height - (elemData.boundingBox.y + elemData.boundingBox.height);
    const bool  openUpward      = spaceBelow < listHeight + kDropdownListOffset && elemData.boundingBox.y >= spaceBelow;
    const float listY           = openUpward ? elemData.boundingBox.y - kDropdownListOffset - listHeight :
                                               elemData.boundingBox.y + elemData.boundingBox.height + kDropdownListOffset;
    auto        rowUnderPointer = [&](int optionIndex) -> bool {
        if (!elemData.found) {
            return false;
        }
        const float top = listY + static_cast<float>(optionIndex - windowStart) * kDropdownRowHeight;
        return mx >= listX && mx <= listX + fieldWidth && my >= top && my <= top + kDropdownRowHeight;
    };

    // Option hit-testing must run before the field decides what the click meant:
    // the list floats outside the field's box, so a click on a row is a click
    // outside the field, and the field would otherwise close the list on the
    // same frame the row was chosen.
    int hitOption = -1;
    if (wasOpen && pressedThisFrame) {
        for (int i = windowStart; i < windowStart + visibleCount; ++i) {
            if (rowUnderPointer(i)) {
                hitOption = i;
                break;
            }
        }
    }

    if (hitOption >= 0) {
        if (selected != hitOption) {
            selected = hitOption;
            changed  = true;
        }
        state.highlightIndex = hitOption;
        isOpen               = false;
        _impl->openDropdown  = 0;
    } else if (pressedThisFrame) {
        if (isHovered) {
            isOpen              = !wasOpen;
            _impl->openDropdown = isOpen ? stateKey : 0;
            if (isOpen) {
                state.highlightIndex = selected;
            }
        } else if (wasOpen) {
            isOpen              = false;
            _impl->openDropdown = 0;
        }
    }

    // Keyboard, for the open list only. Events another widget already claimed
    // are skipped, so a focused text field and an open dropdown cannot both act
    // on one Enter.
    if (isOpen) {
        for (size_t i = 0; i < _impl->pendingEventCount; ++i) {
            auto& ev = _impl->pendingEvents[i];
            if (ev.isChar || ev.consumed) {
                continue;
            }
            switch (static_cast<KeyCode>(ev.key)) {
                case KeyCode::Up:
                    state.highlightIndex = (state.highlightIndex + optionCount - 1) % optionCount;
                    ev.consumed          = true;
                    break;
                case KeyCode::Down:
                    state.highlightIndex = (state.highlightIndex + 1) % optionCount;
                    ev.consumed          = true;
                    break;
                case KeyCode::Enter:
                    if (selected != state.highlightIndex) {
                        selected = state.highlightIndex;
                        changed  = true;
                    }
                    isOpen              = false;
                    _impl->openDropdown = 0;
                    ev.consumed         = true;
                    break;
                case KeyCode::Escape:
                    isOpen              = false;
                    _impl->openDropdown = 0;
                    ev.consumed         = true;
                    break;
                default:
                    break;
            }
        }
    }

    _impl->lastItemHovered = isHovered;
    _impl->lastItemActive  = isOpen;

    Clay_ElementDeclaration fieldDecl = {
        .layout =
            {.sizing =
                 {.width = (width.grow > 0.0f) ? CLAY_SIZING_GROW(width.grow) : CLAY_SIZING_FIXED(fieldWidth), .height = CLAY_SIZING_FIXED(kDropdownHeight)},
             .padding        = {4, 4, 4, 4},
             .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER}},
        .backgroundColor = (isHovered || isOpen) ? Clay_Color {55, 75, 105, 255} : Clay_Color {25, 35, 50, 255},
        .cornerRadius    = {4, 4, 4, 4}
    };
    Clay__ConfigureOpenElement(fieldDecl);

    Text(options[static_cast<size_t>(selected)], 14.0f, {0.9f, 0.9f, 0.9f, 1.0f});

    if (isOpen) {
        // A child of the field, but floating: it layers over what is below
        // without changing the field's own size or the panel's layout.
        Clay__OpenElement();
        Clay_ElementDeclaration listDecl = {
            .layout =
                {.sizing   = {.width = CLAY_SIZING_FIXED(fieldWidth), .height = CLAY_SIZING_FIXED(static_cast<float>(visibleCount) * kDropdownRowHeight)},
                 .childGap = 0,
                 .layoutDirection = CLAY_TOP_TO_BOTTOM},
            .backgroundColor = Clay_Color {20, 28, 40, 250},
            .cornerRadius    = {4, 4, 4, 4},
            .floating        = {
                .offset       = {0.0f, openUpward ? -kDropdownListOffset : kDropdownListOffset},
                .zIndex       = 100,
                .attachPoints = openUpward ? Clay_FloatingAttachPoints {.element = CLAY_ATTACH_POINT_LEFT_BOTTOM, .parent = CLAY_ATTACH_POINT_LEFT_TOP} :
                                             Clay_FloatingAttachPoints {.element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM},
                .attachTo     = CLAY_ATTACH_TO_PARENT
            }
        };
        Clay__ConfigureOpenElement(listDecl);

        for (int i = windowStart; i < windowStart + visibleCount; ++i) {
            const bool hot = (i == hitOption) || rowUnderPointer(i) || (i == state.highlightIndex);
            Clay__OpenElement();
            Clay_ElementDeclaration rowDecl = {
                .layout =
                    {.sizing         = {.width = CLAY_SIZING_GROW(), .height = CLAY_SIZING_FIXED(kDropdownRowHeight)},
                     .padding        = {4, 2, 4, 2},
                     .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER}},
                .backgroundColor = hot ? Clay_Color {70, 100, 140, 255} : Clay_Color {0, 0, 0, 0}
            };
            Clay__ConfigureOpenElement(rowDecl);
            Text(options[static_cast<size_t>(i)], 14.0f, (i == selected) ? JPH::Vec4 {1.0f, 1.0f, 1.0f, 1.0f} : JPH::Vec4 {0.8f, 0.8f, 0.8f, 1.0f});
            Clay__CloseElement();
        }

        Clay__CloseElement(); // list
    }

    Clay__CloseElement(); // field
    EndRow();
    return changed;
}

auto Context::BeginCollapsingHeader(std::string_view label, bool defaultOpen) noexcept -> bool {
    Clay_SetCurrentContext(_impl->clayContext);
    auto           idNum  = static_cast<uint32_t>(HashCreativeWorkPath(label));
    Clay_ElementId elemId = Clay_GetElementIdWithIndex(_impl->Intern(label), idNum);

    uint64_t stateKey = (static_cast<uint64_t>(idNum) << 32) | 0xC011;
    auto&    state    = _impl->GetState(stateKey, _impl->currentFrame);
    if (!state.isInitialized) {
        state.isOpen        = defaultOpen;
        state.isInitialized = true;
    }

    auto* input       = _impl->registry.GetSingleton<Components::InputStateComponent>();
    float mx          = (input != nullptr) ? input->mouseX : -1.0f;
    float my          = (input != nullptr) ? input->mouseY : -1.0f;
    bool  isMouseDown = (input != nullptr) && input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton));

    BeginColumn(4.0f);

    Clay__OpenElementWithId(elemId);

    Clay_ElementData elemData  = Clay_GetElementData(elemId);
    bool             isHovered = Clay_Hovered() || Clay_PointerOver(elemId) ||
                                 (elemData.found && elemData.boundingBox.width > 0.0f && mx >= elemData.boundingBox.x &&
                                  mx <= (elemData.boundingBox.x + elemData.boundingBox.width) && my >= elemData.boundingBox.y &&
                                  my <= (elemData.boundingBox.y + elemData.boundingBox.height));

    auto pointer      = Clay_GetPointerState();
    bool isPressedNow = (pointer.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) || (isHovered && isMouseDown && !state.isPressed);

    if (isHovered && isPressedNow) {
        state.isOpen = !state.isOpen;
    }
    state.isPressed = isMouseDown;

    _impl->lastItemHovered = isHovered;
    _impl->lastItemActive  = isHovered && isMouseDown;

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
