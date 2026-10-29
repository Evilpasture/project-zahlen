// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#define CLAY_IMPLEMENTATION
#include "Text.hpp"
#include <Zahlen/Common.h>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Core/Format.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <clay.h>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ZHLN::GUI {

struct WidgetState {
    bool            isDragging       = false;
    bool            isPressed        = false;
    bool            isOpen           = false;
    bool            isInitialized    = false;
    uint64_t        lastActiveFrame  = 0;
    TextEdit::Caret caret            = {};
    int32_t         highlightIndex   = 0;
    // Scrollbar drag anchor: where the pointer was when the drag started, and
    // what the scroll position was at that moment. Both are needed because a
    // drag sets an absolute position rather than accumulating deltas -- that
    // keeps the thumb under the cursor even when a frame is dropped.
    float dragOriginY      = 0.0f;
    float dragOriginScroll = 0.0f;
};

struct Context::Impl {
    Extent2D                             viewport           = {.width = 1920, .height = 1080};
    Clay_Context*                        clayContext        = nullptr;
    Clay_Arena                           clayArena          = {};
    std::vector<std::byte>               arenaMemory;
    ZHLN::HashMap<uint64_t, WidgetState> widgetStates;
    FontAtlas                            fontAtlas          = {};
    float                                lastDt             = 0.016667f;
    float                                mouseX             = -1.0f;
    float                                mouseY             = -1.0f;
    float                                mouseScroll        = 0.0f;
    std::array<bool, 3>                  mouseDown          = {false, false, false};
    KeyModifiers                         modifiers          = KeyModifiers::None;
    uint64_t                             currentFrame       = 0;
    bool                                 lastItemHovered    = false;
    bool                                 lastItemActive     = false;
    bool                                 inLayout           = false;
    bool                                 frameCapturedMouse = false;
    bool                                 wantsCaptureMouse  = false;

    uint64_t                focusedTextInput    = 0;
    uint64_t                openDropdown        = 0;
    TextEdit::ClipboardSink persistentClipboard = {};
    TextEdit::ClipboardSink activeClipboard     = {};

    std::deque<std::string> stringArena;

    struct ScrollFrame {
        std::string    id;
        uint64_t       key   = 0;
        ScrollBarStyle style;
    };
    std::vector<ScrollFrame> scrollStack;

    std::vector<VertexPosition> uiPositions;
    std::vector<VertexSurface>  uiSurfaces;
    std::vector<UIBatch>        uiBatches;

    auto Intern(std::string_view sv) -> Clay_String {
        auto& stored = stringArena.emplace_back(sv);
        stored.push_back('\0');
        return Clay_String {.isStaticallyAllocated = false, .length = static_cast<int32_t>(sv.size()), .chars = stored.data()};
    }

    struct PendingEvent {
        bool     isChar    = false;
        uint32_t key       = 0;
        uint32_t codepoint = 0;
        bool     consumed  = false;
    };
    static constexpr size_t                     kMaxPendingEvents = 128;
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

    Impl() noexcept {
        fontAtlas = BuildFontAtlas(GetDefaultBakedFont(), TextureHandle::Invalid);
        if (fontAtlas.glyphCount == 0) {
            fontAtlas.firstCodepoint = 32;
            fontAtlas.glyphCount     = 96;
            for (uint32_t i = 0; i < fontAtlas.glyphCount; ++i) {
                fontAtlas.glyphs[i].xadvance = 18.0f;
            }
        }
    }

    explicit Impl(const FontAtlas& initialAtlas) noexcept: Impl() {
        if (initialAtlas.glyphCount > 0) {
            fontAtlas = initialAtlas;
        }
    }

    ~Impl() noexcept {
        if (Clay_GetCurrentContext() == clayContext) {
            Clay_SetCurrentContext(nullptr);
        }
        clayContext = nullptr;
    }

    auto GetState(uint64_t id, uint64_t frame) noexcept -> WidgetState& {
        auto state = widgetStates.Find(id);
        if (!state) {
            widgetStates.Insert(id, WidgetState {});
            state = widgetStates.Find(id);
        }
        assert(state.has_value());
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
        if ((impl == nullptr) || (impl->fontAtlas.glyphCount == 0) || text.length == 0) {
            return {0.0f, 0.0f};
        }

        const FontAtlas& font       = impl->fontAtlas;
        float            scale      = font.ScaleFor(static_cast<float>(config->fontSize));
        float            currentX   = 0.0f;
        float            maxX       = 0.0f;
        float            lineHeight = TextLineHeight(font, scale);
        float            totalH     = lineHeight;

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
            currentX += font.GlyphFor(static_cast<uint8_t>(c)).xadvance * scale;
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

constexpr float kTextInputFontSize = 15.0f;
constexpr float kTextInputPadding  = 4.0f;
constexpr float kTextInputHeight   = 24.0f;
constexpr float kTextInputWidth    = 180.0f;

constexpr float kDropdownHeight     = 24.0f;
constexpr float kDropdownWidth      = 160.0f;
constexpr float kDropdownRowHeight  = 20.0f;
constexpr float kDropdownListOffset = 2.0f;
constexpr int   kDropdownMaxVisible = 8;

[[nodiscard]] inline auto GlyphAdvance(const FontAtlas& font, char c, float scale) noexcept -> float {
    return font.GlyphFor(static_cast<uint8_t>(c)).xadvance * scale;
}

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

// ===========================================================================
// Context
// ===========================================================================

Context::Context(): _impl(std::make_unique<Impl>()) {}

Context::Context(const FontAtlas& fontAtlas): _impl(std::make_unique<Impl>(fontAtlas)) {}

Context::~Context() noexcept = default;

Context::Context(Context&&) noexcept = default;

auto Context::operator=(Context&&) noexcept -> Context& = default;

void Context::SetFontAtlas(const FontAtlas& fontAtlas) noexcept {
    assert(_impl != nullptr);
    if (fontAtlas.glyphCount > 0) {
        _impl->fontAtlas = fontAtlas;
    } else {
        _impl->fontAtlas.texture = fontAtlas.texture;
    }
}

auto Context::GetFontAtlas() const noexcept -> const FontAtlas& {
    assert(_impl != nullptr);
    return _impl->fontAtlas;
}

void Context::SetClipboard(TextEdit::ClipboardSink sink) noexcept {
    if (_impl != nullptr) {
        _impl->persistentClipboard = sink;
    }
}

auto Context::GetLastFrameRect(std::string_view id) const noexcept -> std::optional<ElementRect> {
    if ((_impl == nullptr) || (_impl->clayContext == nullptr) || id.empty()) {
        return std::nullopt;
    }
    Clay_Context* const prev = Clay_GetCurrentContext();
    Clay_SetCurrentContext(_impl->clayContext);
    ZHLN::defer _([prev]() noexcept { Clay_SetCurrentContext(prev); });

    Clay_String cs {};
    cs.length                   = static_cast<int32_t>(id.size());
    cs.chars                    = id.data();
    const Clay_ElementData data = Clay_GetElementData(Clay_GetElementIdWithIndex(cs, static_cast<uint32_t>(Hash32(id))));
    if (!data.found) {
        return std::nullopt;
    }
    return ElementRect {.x = data.boundingBox.x, .y = data.boundingBox.y, .width = data.boundingBox.width, .height = data.boundingBox.height};
}

auto Context::GetScrollState(std::string_view id) const noexcept -> ScrollState {
    ScrollState state;
    if ((_impl == nullptr) || (_impl->clayContext == nullptr) || id.empty()) {
        return state;
    }
    Clay_Context* const prev = Clay_GetCurrentContext();
    Clay_SetCurrentContext(_impl->clayContext);
    ZHLN::defer _([prev]() noexcept { Clay_SetCurrentContext(prev); });

    Clay_String clayId {};
    clayId.length = static_cast<int32_t>(id.size());
    clayId.chars  = id.data();
    const Clay_ScrollContainerData scroll =
        Clay_GetScrollContainerData(Clay_GetElementIdWithIndex(clayId, static_cast<uint32_t>(Hash32(id))));

    if (!scroll.found) {
        return state;
    }
    state.scrollY    = (scroll.scrollPosition != nullptr) ? scroll.scrollPosition->y : 0.0f;
    state.contentH   = scroll.contentDimensions.height;
    state.containerH = scroll.scrollContainerDimensions.height;
    return state;
}

auto Context::IsPointerOver(std::string_view id) const noexcept -> bool {
    const auto rect = GetLastFrameRect(id);
    if (!rect || (_impl == nullptr)) {
        return false;
    }
    const float mx = _impl->mouseX;
    const float my = _impl->mouseY;
    return mx >= rect->x && mx <= (rect->x + rect->width) && my >= rect->y && my <= (rect->y + rect->height);
}

auto Context::IsTextInputFocused() const noexcept -> bool {
    return (_impl != nullptr) && (_impl->focusedTextInput != 0);
}

auto Context::WantsCaptureMouse() const noexcept -> bool {
    return (_impl != nullptr) && _impl->wantsCaptureMouse;
}

auto Context::WantsCaptureKeyboard() const noexcept -> bool {
    return (_impl != nullptr) && ((_impl->focusedTextInput != 0) || (_impl->openDropdown != 0));
}

auto Context::HasActiveFrame() const noexcept -> bool {
    return (_impl != nullptr) && _impl->inLayout;
}

auto Context::BeginFrame(const UIInput& input) noexcept -> Frame {
    assert(_impl != nullptr && "GUI::Context::BeginFrame called on moved-from Context");
    assert(!_impl->inLayout && "GUI::Context::BeginFrame called while a GUI::Frame is already active");

    Clay_Context* const prevClay = Clay_GetCurrentContext();

    _impl->currentFrame++;
    _impl->stringArena.clear();
    _impl->scrollStack.clear();
    _impl->uiPositions.clear();
    _impl->uiSurfaces.clear();
    _impl->uiBatches.clear();
    _impl->lastItemHovered    = false;
    _impl->lastItemActive     = false;
    _impl->frameCapturedMouse = false;

    _impl->viewport    = input.viewport;
    _impl->lastDt      = input.dt;
    _impl->mouseX      = input.mouseX;
    _impl->mouseY      = input.mouseY;
    _impl->mouseScroll = input.mouseScroll;
    _impl->mouseDown   = {input.mouseDown[0], input.mouseDown[1], input.mouseDown[2]};
    _impl->modifiers   = input.modifiers;

    if (input.clipboard.get != nullptr || input.clipboard.set != nullptr) {
        _impl->activeClipboard = input.clipboard;
    } else {
        _impl->activeClipboard = _impl->persistentClipboard;
    }

    _impl->ClearPendingEvents();
    for (const KeyCode key: input.pressedKeys) {
        _impl->QueueEvent(Impl::PendingEvent {.isChar = false, .key = static_cast<uint32_t>(key), .codepoint = 0});
    }
    for (const uint32_t cp: input.characterStream) {
        _impl->QueueEvent(Impl::PendingEvent {.isChar = true, .key = 0, .codepoint = cp});
    }

    if (_impl->clayContext == nullptr) {
        Clay_SetCurrentContext(nullptr);
        Clay_SetMaxElementCount(8192);
        Clay_SetMaxMeasureTextCacheWordCount(8192);
        const uint64_t memSize = Clay_MinMemorySize();
        _impl->arenaMemory.resize(memSize);
        _impl->clayArena = Clay_CreateArenaWithCapacityAndMemory(memSize, _impl->arenaMemory.data());
        _impl->clayContext =
            Clay_Initialize(_impl->clayArena, {static_cast<float>(input.viewport.width), static_cast<float>(input.viewport.height)}, {});
        Clay_SetMeasureTextFunction(Impl::MeasureText, _impl.get());
    }

    Clay_SetCurrentContext(_impl->clayContext);
    Clay_SetLayoutDimensions({static_cast<float>(input.viewport.width), static_cast<float>(input.viewport.height)});
    Clay_SetPointerState(Clay_Vector2 {_impl->mouseX, _impl->mouseY}, _impl->mouseDown[0]);
    Clay_UpdateScrollContainers(false, Clay_Vector2 {0.0f, _impl->mouseScroll * 30.0f}, _impl->lastDt);

    _impl->PruneStaleStates(_impl->currentFrame);
    Clay_BeginLayout();
    _impl->inLayout = true;

    return Frame(_impl.get(), prevClay);
}

// ===========================================================================
// Frame
// ===========================================================================

Frame::Frame(Context::Impl* impl, void* prevClayContext) noexcept: _impl(impl), _prevClayContext(prevClayContext) {}

Frame::Frame(Frame&& other) noexcept
    : _impl(std::exchange(other._impl, nullptr)), _prevClayContext(std::exchange(other._prevClayContext, nullptr)) {}

Frame::~Frame() noexcept {
    if (_impl != nullptr) {
        Cancel();
    }
}

void Frame::Cancel() noexcept {
    if (_impl == nullptr) {
        return;
    }
    Context::Impl* const impl = std::exchange(_impl, nullptr);
    auto* const          prev = static_cast<Clay_Context*>(std::exchange(_prevClayContext, nullptr));

    if (impl->clayContext != nullptr && impl->inLayout) {
        Clay_SetCurrentContext(impl->clayContext);
        while (impl->clayContext->openLayoutElementStack.length > 1) {
            Clay__CloseElement();
        }
        (void)Clay_EndLayout(impl->lastDt);
        impl->inLayout = false;
    }
    impl->scrollStack.clear();
    impl->ClearPendingEvents();
    impl->wantsCaptureMouse = false;
    Clay_SetCurrentContext(prev);
}

auto Frame::End() && noexcept -> UIDrawData {
    if (_impl == nullptr) {
        return {};
    }
    Context::Impl* const impl = std::exchange(_impl, nullptr);
    auto* const          prev = static_cast<Clay_Context*>(std::exchange(_prevClayContext, nullptr));
    ZHLN::defer          _([prev]() noexcept { Clay_SetCurrentContext(prev); });

    if ((impl->clayContext == nullptr) || !impl->inLayout) {
        return {};
    }

    Clay_SetCurrentContext(impl->clayContext);
    const Clay_RenderCommandArray commands = Clay_EndLayout(impl->lastDt);
    impl->inLayout                         = false;
    impl->scrollStack.clear();
    impl->ClearPendingEvents();

    const bool wantsCaptureMouse    = impl->frameCapturedMouse;
    const bool wantsCaptureKeyboard = (impl->focusedTextInput != 0) || (impl->openDropdown != 0);
    impl->wantsCaptureMouse         = wantsCaptureMouse;

    if (commands.length == 0 || impl->fontAtlas.glyphCount == 0) {
        return UIDrawData {
            .wantsCaptureMouse    = wantsCaptureMouse,
            .wantsCaptureKeyboard = wantsCaptureKeyboard,
        };
    }

    const FontAtlas& font      = impl->fontAtlas;
    auto&            positions = impl->uiPositions;
    auto&            surfaces  = impl->uiSurfaces;
    auto&            batches   = impl->uiBatches;

    positions.reserve(static_cast<size_t>(commands.length) * 6);
    surfaces.reserve(static_cast<size_t>(commands.length) * 6);

    auto EmitQuad = [&](float x0, float y0, float x1, float y1, const JPH::Vec4& color) -> void {
        const PackedRGBA8 c = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

        positions.push_back({{x0, y0, 0.0f}});
        positions.push_back({{x0, y1, 0.0f}});
        positions.push_back({{x1, y0, 0.0f}});
        positions.push_back({{x1, y0, 0.0f}});
        positions.push_back({{x0, y1, 0.0f}});
        positions.push_back({{x1, y1, 0.0f}});

        surfaces.push_back({.uv = Math::PackUV(0, 0), .color = c, .uv1 = {}});
        surfaces.push_back({.uv = Math::PackUV(0, 1), .color = c, .uv1 = {}});
        surfaces.push_back({.uv = Math::PackUV(1, 0), .color = c, .uv1 = {}});
        surfaces.push_back({.uv = Math::PackUV(1, 0), .color = c, .uv1 = {}});
        surfaces.push_back({.uv = Math::PackUV(0, 1), .color = c, .uv1 = {}});
        surfaces.push_back({.uv = Math::PackUV(1, 1), .color = c, .uv1 = {}});
    };

    ScissorRect activeScissor = {};
    bool        useScissor    = false;

    for (int32_t i = 0; i < commands.length; ++i) {
        Clay_RenderCommand* cmd = &commands.internalArray[i];
        const auto&         bb  = cmd->boundingBox;

        switch (cmd->commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                const auto      c = cmd->renderData.rectangle.backgroundColor;
                const JPH::Vec4 color(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);

                const auto startIdx = static_cast<uint32_t>(positions.size());
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
                const auto      tc = cmd->renderData.text.textColor;
                const JPH::Vec4 color(tc.r / 255.0f, tc.g / 255.0f, tc.b / 255.0f, tc.a / 255.0f);
                const float     scale = font.ScaleFor(static_cast<float>(cmd->renderData.text.fontSize));

                const std::string_view text(cmd->renderData.text.stringContents.chars, static_cast<size_t>(cmd->renderData.text.stringContents.length));
                const uint32_t         maxVerts = static_cast<uint32_t>(text.size()) * 6;

                const size_t startIdx = positions.size();
                positions.resize(startIdx + maxVerts);
                surfaces.resize(startIdx + maxVerts);

                const uint32_t written = AppendTextVertices(&positions[startIdx], &surfaces[startIdx], font, text, bb.x, bb.y, scale, color);
                positions.resize(startIdx + written);
                surfaces.resize(startIdx + written);

                batches.push_back(
                    {.texture     = font.texture,
                     .vertexStart = static_cast<uint32_t>(startIdx),
                     .vertexCount = written,
                     .useScissor  = useScissor,
                     .isSDF       = (font.texture != TextureHandle::Invalid) && font.isSDF,
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

    return UIDrawData {
        .batches              = std::span<const UIBatch>(batches.data(), batches.size()),
        .positions            = std::span<const VertexPosition>(positions.data(), positions.size()),
        .surfaces             = std::span<const VertexSurface>(surfaces.data(), surfaces.size()),
        .wantsCaptureMouse    = wantsCaptureMouse,
        .wantsCaptureKeyboard = wantsCaptureKeyboard,
    };
}

void Frame::BeginBox(std::string_view id, const BoxConfig& cfg) noexcept {
    assert(_impl != nullptr && _impl->inLayout);
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
        auto           numId  = static_cast<uint32_t>(Hash32(id));
        Clay_ElementId elemId = Clay_GetElementIdWithIndex(_impl->Intern(id), numId);
        Clay__OpenElementWithId(elemId);
        if (cfg.color.GetW() > 0.01f) {
            const Clay_ElementData elemData = Clay_GetElementData(elemId);
            const float            mx       = _impl->mouseX;
            const float            my       = _impl->mouseY;
            if (Clay_Hovered() || Clay_PointerOver(elemId) ||
                (elemData.found && elemData.boundingBox.width > 0.0f && mx >= elemData.boundingBox.x &&
                 mx <= (elemData.boundingBox.x + elemData.boundingBox.width) && my >= elemData.boundingBox.y &&
                 my <= (elemData.boundingBox.y + elemData.boundingBox.height))) {
                _impl->frameCapturedMouse = true;
            }
        }
    } else {
        Clay__OpenElement();
        if (cfg.color.GetW() > 0.01f && Clay_Hovered()) {
            _impl->frameCapturedMouse = true;
        }
    }

    if (cfg.clipVertical) {
        decl.clip.vertical    = true;
        decl.clip.childOffset = Clay_GetScrollOffset();
    }

    Clay__ConfigureOpenElement(decl);
}

void Frame::EndBox() noexcept {
    assert(_impl != nullptr && _impl->inLayout);
    Clay_SetCurrentContext(_impl->clayContext);
    Clay__CloseElement();
}

void Frame::BeginScrollArea(std::string_view id, const BoxConfig& cfg, const ScrollBarStyle& style) noexcept {
    if (_impl == nullptr) {
        return;
    }

    BoxConfig scrolled    = cfg;
    scrolled.clipVertical = true;

    _impl->scrollStack.push_back(
        Context::Impl::ScrollFrame {.id = std::string(id), .key = (static_cast<uint64_t>(Hash32(id)) << 32) | 0x0005u, .style = style}
    );

    BeginBox(id, scrolled);
}

void Frame::EndScrollArea() noexcept {
    if ((_impl == nullptr) || (_impl->clayContext == nullptr)) {
        return;
    }
    if (_impl->scrollStack.empty()) {
        EndBox();
        return;
    }

    const Context::Impl::ScrollFrame frame = _impl->scrollStack.back();
    _impl->scrollStack.pop_back();

    if (!frame.id.empty()) {
        Clay_SetCurrentContext(_impl->clayContext);

        Clay_String clayId {};
        clayId.length               = static_cast<int32_t>(frame.id.size());
        clayId.chars                = frame.id.data();
        const Clay_ElementId elemId = Clay_GetElementIdWithIndex(clayId, static_cast<uint32_t>(Hash32(frame.id)));

        const Clay_ScrollContainerData scroll     = Clay_GetScrollContainerData(elemId);
        const float                    contentH   = scroll.contentDimensions.height;
        const float                    containerH = scroll.scrollContainerDimensions.height;
        const float                    overflow   = contentH - containerH;

        if (scroll.found && scroll.config.vertical && (scroll.scrollPosition != nullptr) && (overflow > 0.5f) && (containerH > 1.0f)) {
            const ScrollBarStyle& bar    = frame.style;
            const float           trackH = std::max(containerH - (2.0f * bar.margin), 1.0f);
            const float           thumbH = std::min(trackH, std::max(bar.minThumb, trackH * (containerH / contentH)));
            const float           travel = trackH - thumbH;

            float* const scrollY = &scroll.scrollPosition->y;

            const float mx   = _impl->mouseX;
            const float my   = _impl->mouseY;
            const bool  down = _impl->mouseDown[0];

            auto& drag = _impl->GetState(frame.key, _impl->currentFrame);

            const Clay_ElementData containerBox = Clay_GetElementData(elemId);
            const float            fraction     = std::clamp(-*scrollY / overflow, 0.0f, 1.0f);
            const float            thumbX       = containerBox.boundingBox.x + containerBox.boundingBox.width - bar.margin - bar.width;
            const float            thumbY       = containerBox.boundingBox.y + bar.margin + (fraction * travel);
            const bool             overThumb    = (mx >= thumbX) && (mx <= (thumbX + bar.width)) && (my >= thumbY) && (my <= (thumbY + thumbH));

            if (!down) {
                drag.isDragging = false;
            } else if (drag.isDragging && (travel > 0.0f)) {
                const float dy = my - drag.dragOriginY;
                *scrollY       = std::clamp(drag.dragOriginScroll - (dy * (overflow / travel)), -overflow, 0.0f);
            } else if (overThumb && !drag.isPressed) {
                drag.isDragging       = true;
                drag.dragOriginY      = my;
                drag.dragOriginScroll = *scrollY;
            }
            drag.isPressed = down;

            const bool hot = drag.isDragging || overThumb;
            if (hot) {
                _impl->frameCapturedMouse = true;
            }
            const float drawY = std::clamp(-*scrollY / overflow, 0.0f, 1.0f) * travel;

            const auto EmitBar = [&](float y, float h, const JPH::Vec4& color) -> void {
                Clay_ElementDeclaration decl = {
                    .layout          = {.sizing = {.width = CLAY_SIZING_FIXED(bar.width), .height = CLAY_SIZING_FIXED(h)}},
                    .backgroundColor = ToClayColor(color),
                    .cornerRadius    = {bar.width * 0.5f, bar.width * 0.5f, bar.width * 0.5f, bar.width * 0.5f}
                };
                decl.floating = {
                    .offset       = {-(bar.margin + bar.width), bar.margin + y},
                    .zIndex       = 2,
                    .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_TOP, .parent = CLAY_ATTACH_POINT_RIGHT_TOP},
                    .attachTo     = CLAY_ATTACH_TO_PARENT,
                    .clipTo       = CLAY_CLIP_TO_ATTACHED_PARENT
                };
                Clay__OpenElement();
                Clay__ConfigureOpenElement(decl);
                Clay__CloseElement();
            };

            EmitBar(0.0f, trackH, bar.trackColor);
            EmitBar(drawY, thumbH, hot ? bar.thumbHoverColor : bar.thumbColor);
        }
    }

    EndBox();
}

void Frame::BeginRow(float gap, float padding) noexcept {
    BeginBox("", {.width = {.grow = 1.0f}, .padding = padding, .gap = gap, .direction = Direction::Row, .alignCross = Alignment::Center});
}

void Frame::EndRow() noexcept {
    EndBox();
}

void Frame::BeginColumn(float gap, float padding) noexcept {
    BeginBox("", {.height = {.grow = 1.0f}, .padding = padding, .gap = gap, .direction = Direction::Column});
}

void Frame::EndColumn() noexcept {
    EndBox();
}

void Frame::Text(std::string_view text, float fontSize, const JPH::Vec4& color) noexcept {
    assert(_impl != nullptr && _impl->inLayout);
    Clay_SetCurrentContext(_impl->clayContext);
    Clay_TextElementConfig config = {.textColor = ToClayColor(color), .fontSize = static_cast<uint16_t>(fontSize)};
    Clay__OpenTextElement(_impl->Intern(text), config);
}

auto Frame::Button(std::string_view label, const JPH::Vec4& color, const Sizing& width, std::string_view id) noexcept -> bool {
    assert(_impl != nullptr && _impl->inLayout);
    Clay_SetCurrentContext(_impl->clayContext);
    bool                   clicked = false;
    const std::string_view key     = id.empty() ? label : id;
    auto                   idNum   = static_cast<uint32_t>(Hash32(key));
    Clay_ElementId         elemId  = Clay_GetElementIdWithIndex(_impl->Intern(key), idNum);
    auto&                  state   = _impl->GetState((static_cast<uint64_t>(idNum) << 32) | 0xB007, _impl->currentFrame);

    const float mx          = _impl->mouseX;
    const float my          = _impl->mouseY;
    const bool  isMouseDown = _impl->mouseDown[0];

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
    if (isHovered) {
        _impl->frameCapturedMouse = true;
    }

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

auto Frame::Checkbox(std::string_view label, bool& checked, std::string_view id) noexcept -> bool {
    assert(_impl != nullptr && _impl->inLayout);
    Clay_SetCurrentContext(_impl->clayContext);
    bool                   changed = false;
    const std::string_view key     = id.empty() ? label : id;
    auto                   idNum   = static_cast<uint32_t>(Hash32(key));
    Clay_ElementId         elemId  = Clay_GetElementIdWithIndex(_impl->Intern(key), idNum);
    auto&                  state   = _impl->GetState((static_cast<uint64_t>(idNum) << 32) | 0x00CB, _impl->currentFrame);

    const float mx          = _impl->mouseX;
    const float my          = _impl->mouseY;
    const bool  isMouseDown = _impl->mouseDown[0];

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
    if (isHovered) {
        _impl->frameCapturedMouse = true;
    }

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

auto Frame::Slider(std::string_view label, float& value, float minVal, float maxVal, std::string_view id) noexcept -> bool {
    assert(_impl != nullptr && _impl->inLayout);
    Clay_SetCurrentContext(_impl->clayContext);
    bool                   changed = false;
    const std::string_view key     = id.empty() ? label : id;
    auto                   idNum   = static_cast<uint32_t>(Hash32(key));
    Clay_ElementId         elemId  = Clay_GetElementIdWithIndex(_impl->Intern(key), idNum);

    uint64_t stateKey = (static_cast<uint64_t>(idNum) << 32) | 0x511D;
    auto&    state    = _impl->GetState(stateKey, _impl->currentFrame);

    const float mx          = _impl->mouseX;
    const float my          = _impl->mouseY;
    const bool  isMouseDown = _impl->mouseDown[0];

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
    if (isHovered || state.isDragging) {
        _impl->frameCapturedMouse = true;
    }

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

    Clay__CloseElement();

    std::array<char, 32> valBuf {};
    ZHLN::FormatTo(valBuf, "{.2f}", value);

    BeginBox("", {.width = {.fixed = 48.0f}, .height = {.fixed = 22.0f}, .alignMain = Alignment::Center, .alignCross = Alignment::End});
    Text(valBuf.data(), 14.0f, {0.7f, 0.7f, 0.7f, 1.0f});
    EndBox();

    EndRow();
    return changed;
}

auto Frame::TextInputImpl(std::string_view label, std::string& value, size_t maxTextLength, const Sizing& width, std::string_view id) noexcept -> bool {
    assert(_impl != nullptr && _impl->inLayout);
    Clay_SetCurrentContext(_impl->clayContext);

    const std::string_view key      = id.empty() ? label : id;
    auto                   idNum    = static_cast<uint32_t>(Hash32(key));
    Clay_ElementId         elemId   = Clay_GetElementIdWithIndex(_impl->Intern(key), idNum);
    const uint64_t         stateKey = (static_cast<uint64_t>(idNum) << 32) | 0x7E17;
    auto&                  state    = _impl->GetState(stateKey, _impl->currentFrame);

    const float mx = _impl->mouseX;
    const float my = _impl->mouseY;

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
        if (isHovered) {
            isFocused               = true;
            _impl->focusedTextInput = stateKey;
            state.caret.selectAll   = false;
            if (elemData.found && _impl->fontAtlas.glyphCount > 0) {
                const float scale       = _impl->fontAtlas.ScaleFor(kTextInputFontSize);
                const float localX      = std::max(0.0f, mx - (elemData.boundingBox.x + kTextInputPadding));
                state.caret.cursorIndex = static_cast<uint32_t>(CaretIndexAtX(_impl->fontAtlas, std::string_view(value), localX, scale));
            }
            state.caret.ClearSelection();
        } else {
            isFocused = false;
            if (_impl->focusedTextInput == stateKey) {
                _impl->focusedTextInput = 0;
            }
        }
    }

    if (isFocused && _impl->pendingEventCount > 0) {
        const TextEdit::Modifiers mods {
            .shift = (_impl->modifiers & KeyModifiers::Shift) != KeyModifiers::None,
            .ctrl  = (_impl->modifiers & KeyModifiers::Ctrl) != KeyModifiers::None,
        };
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
            const auto result = TextEdit::HandleKey(buf, state.caret, static_cast<KeyCode>(ev.key), mods, _impl->activeClipboard);
            if (result == TextEdit::KeyResult::Edited) {
                changed = true;
            }
            if (result == TextEdit::KeyResult::Committed) {
                isFocused               = false;
                _impl->focusedTextInput = 0;
                break;
            }
        }
    }

    _impl->lastItemHovered = isHovered;
    _impl->lastItemActive  = isFocused;
    if (isHovered) {
        _impl->frameCapturedMouse = true;
    }

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
    drawCaret();

    Clay__CloseElement();
    EndRow();
    return changed;
}

auto Frame::TextInput(std::string_view label, std::string& value, const Sizing& width, std::string_view id) noexcept -> bool {
    return TextInputImpl(label, value, std::numeric_limits<size_t>::max(), width, id);
}

auto Frame::Dropdown(std::string_view label, std::span<const std::string_view> options, int& selected, const Sizing& width) noexcept -> bool {
    assert(_impl != nullptr && _impl->inLayout);
    Clay_SetCurrentContext(_impl->clayContext);

    const int optionCount = static_cast<int>(options.size());

    BeginRow(8.0f);
    Text(label, 15.0f, {0.9f, 0.9f, 0.9f, 1.0f});

    if (optionCount == 0) {
        Text("(no options)", 14.0f, {0.5f, 0.5f, 0.5f, 1.0f});
        EndRow();
        _impl->lastItemHovered = false;
        _impl->lastItemActive  = false;
        return false;
    }

    auto           idNum    = static_cast<uint32_t>(Hash32(label));
    Clay_ElementId elemId   = Clay_GetElementIdWithIndex(_impl->Intern(label), idNum);
    const uint64_t stateKey = (static_cast<uint64_t>(idNum) << 32) | 0xD209;
    auto&          state    = _impl->GetState(stateKey, _impl->currentFrame);

    const float mx = _impl->mouseX;
    const float my = _impl->mouseY;

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

    const int visibleCount = std::min(optionCount, kDropdownMaxVisible);
    const int windowStart  = (optionCount <= kDropdownMaxVisible) ? 0 :
                                                                    std::clamp(state.highlightIndex - kDropdownMaxVisible / 2, 0, optionCount - visibleCount);

    const float listX      = elemData.boundingBox.x;
    const float listHeight = static_cast<float>(visibleCount) * kDropdownRowHeight;
    const float spaceBelow = Clay_GetLayoutDimensions().height - (elemData.boundingBox.y + elemData.boundingBox.height);
    const bool  openUpward = spaceBelow < listHeight + kDropdownListOffset && elemData.boundingBox.y >= spaceBelow;
    const float listY      = openUpward ? elemData.boundingBox.y - kDropdownListOffset - listHeight :
                                          elemData.boundingBox.y + elemData.boundingBox.height + kDropdownListOffset;

    auto rowUnderPointer = [&](int optionIndex) -> bool {
        if (!elemData.found) {
            return false;
        }
        const float top = listY + static_cast<float>(optionIndex - windowStart) * kDropdownRowHeight;
        return mx >= listX && mx <= listX + fieldWidth && my >= top && my <= top + kDropdownRowHeight;
    };

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
    if (isHovered || isOpen) {
        _impl->frameCapturedMouse = true;
    }

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
        Clay__OpenElement();
        Clay_ElementDeclaration listDecl = {
            .layout =
                {.sizing          = {.width = CLAY_SIZING_FIXED(fieldWidth), .height = CLAY_SIZING_FIXED(static_cast<float>(visibleCount) * kDropdownRowHeight)},
                 .childGap        = 0,
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
            const bool ptrRow = rowUnderPointer(i);
            if (ptrRow) {
                _impl->frameCapturedMouse = true;
            }
            const bool hot = (i == hitOption) || ptrRow || (i == state.highlightIndex);
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

        Clay__CloseElement();
    }

    Clay__CloseElement();
    EndRow();
    return changed;
}

auto Frame::BeginCollapsingHeader(std::string_view label, bool defaultOpen) noexcept -> bool {
    assert(_impl != nullptr && _impl->inLayout);
    Clay_SetCurrentContext(_impl->clayContext);
    auto           idNum  = static_cast<uint32_t>(Hash32(label));
    Clay_ElementId elemId = Clay_GetElementIdWithIndex(_impl->Intern(label), idNum);

    uint64_t stateKey = (static_cast<uint64_t>(idNum) << 32) | 0xC011;
    auto&    state    = _impl->GetState(stateKey, _impl->currentFrame);
    if (!state.isInitialized) {
        state.isOpen        = defaultOpen;
        state.isInitialized = true;
    }

    const float mx          = _impl->mouseX;
    const float my          = _impl->mouseY;
    const bool  isMouseDown = _impl->mouseDown[0];

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
    if (isHovered) {
        _impl->frameCapturedMouse = true;
    }

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

    Clay__CloseElement();

    if (state.isOpen) {
        BeginBox("", {.width = {.grow = 1.0f}, .padding = 8.0f, .gap = 4.0f, .direction = Direction::Column});
        return true;
    }

    EndColumn();
    return false;
}

void Frame::EndCollapsingHeader() noexcept {
    EndBox();
    EndColumn();
}

auto Frame::IsItemHovered() const noexcept -> bool {
    return (_impl != nullptr) ? _impl->lastItemHovered : false;
}

auto Frame::IsItemActive() const noexcept -> bool {
    return (_impl != nullptr) ? _impl->lastItemActive : false;
}

auto Frame::IsPointerPressedThisFrame() const noexcept -> bool {
    if ((_impl == nullptr) || (_impl->clayContext == nullptr)) {
        return false;
    }
    Clay_SetCurrentContext(_impl->clayContext);
    return Clay_GetPointerState().state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME;
}

auto Frame::Viewport() const noexcept -> Extent2D {
    return (_impl != nullptr) ? _impl->viewport : Extent2D {};
}

auto Frame::DeltaTime() const noexcept -> float {
    return (_impl != nullptr) ? _impl->lastDt : 0.0f;
}

auto Frame::GetFontAtlas() const noexcept -> const FontAtlas& {
    assert(_impl != nullptr);
    return _impl->fontAtlas;
}

auto Frame::GetLastFrameRect(std::string_view id) const noexcept -> std::optional<ElementRect> {
    if ((_impl == nullptr) || (_impl->clayContext == nullptr) || id.empty()) {
        return std::nullopt;
    }
    Clay_SetCurrentContext(_impl->clayContext);

    Clay_String cs {};
    cs.length                   = static_cast<int32_t>(id.size());
    cs.chars                    = id.data();
    const Clay_ElementData data = Clay_GetElementData(Clay_GetElementIdWithIndex(cs, static_cast<uint32_t>(Hash32(id))));
    if (!data.found) {
        return std::nullopt;
    }
    return ElementRect {.x = data.boundingBox.x, .y = data.boundingBox.y, .width = data.boundingBox.width, .height = data.boundingBox.height};
}

auto Frame::GetScrollState(std::string_view id) const noexcept -> ScrollState {
    ScrollState state;
    if ((_impl == nullptr) || (_impl->clayContext == nullptr) || id.empty()) {
        return state;
    }
    Clay_SetCurrentContext(_impl->clayContext);

    Clay_String clayId {};
    clayId.length = static_cast<int32_t>(id.size());
    clayId.chars  = id.data();
    const Clay_ScrollContainerData scroll =
        Clay_GetScrollContainerData(Clay_GetElementIdWithIndex(clayId, static_cast<uint32_t>(Hash32(id))));

    if (!scroll.found) {
        return state;
    }
    state.scrollY    = (scroll.scrollPosition != nullptr) ? scroll.scrollPosition->y : 0.0f;
    state.contentH   = scroll.contentDimensions.height;
    state.containerH = scroll.scrollContainerDimensions.height;
    return state;
}

auto Frame::IsPointerOver(std::string_view id) const noexcept -> bool {
    const auto rect = GetLastFrameRect(id);
    if (!rect || (_impl == nullptr)) {
        return false;
    }
    const float mx = _impl->mouseX;
    const float my = _impl->mouseY;
    return mx >= rect->x && mx <= (rect->x + rect->width) && my >= rect->y && my <= (rect->y + rect->height);
}

auto Frame::IsTextInputFocused() const noexcept -> bool {
    return (_impl != nullptr) && (_impl->focusedTextInput != 0);
}

} // namespace ZHLN::GUI
