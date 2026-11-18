// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Zahlen/HostUI.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <algorithm>
#include <span>
#include <string>
#include <utility>

namespace ZHLN {
HostUI::~HostUI() {
    if (_texture != TextureID::Invalid && _fontGeneration == _engine.DeviceGeneration()) _engine.Graphics()->UnloadTexture(_texture);
}
void HostUI::SetBakedFont(GUI::BakedFontAsset asset) {
    if (_texture != TextureID::Invalid && _fontGeneration == _engine.DeviceGeneration()) _engine.Graphics()->UnloadTexture(_texture);
    _texture = TextureID::Invalid;
    _baked = std::move(asset);
    _fontGeneration = ~uint64_t {0};
}
auto HostUI::GetFontAtlas() -> const FontAtlas& {
    const auto generation = _engine.DeviceGeneration();
    if (_fontGeneration == generation) return _font;
    if (_baked.coverage.empty()) _baked = GUI::GetDefaultBakedFont();
    _texture = TextureID::Invalid; // old-device handles must never be unloaded on the new device
    _font = GUI::BuildFontAtlas(_baked);
    if (_baked.atlasWidth && _baked.atlasHeight && !_baked.coverage.empty()) {
        std::vector<uint32_t> rgba(_baked.coverage.size());
        for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = (uint32_t(_baked.coverage[i]) << 24) | 0x00ffffffu;
        auto texture = _engine.Graphics()->CreateTexture("HostFontAtlas", std::as_bytes(std::span(rgba)), {_baked.atlasWidth, _baked.atlasHeight}, false);
        if (texture.HasValue()) { _texture = texture.value; _font = GUI::BuildFontAtlas(_baked, _texture); }
    }
    _fontGeneration = generation;
    return _font;
}
void HostUI::SubmitInput(GUI::UIInput input) {
    _suppliedKeys.assign(input.pressedKeys.begin(), input.pressedKeys.end());
    _suppliedChars.assign(input.characterStream.begin(), input.characterStream.end());
    input.pressedKeys = _suppliedKeys; input.characterStream = _suppliedChars;
    _supplied = input;
}
auto HostUI::ConsumeInput(float dt, WindowID window) -> GUI::UIInput {
    if (_supplied && window == WindowID::Primary) {
        auto out = *_supplied; out.dt = dt; out.viewport = _engine.WindowExtent(window);
        out.pressedKeys = _suppliedKeys; out.characterStream = _suppliedChars;
        _supplied.reset(); return out;
    }
    COM::HostInputInfo state;
    (void)_engine.ReadHostInput(window, COM::OutChain(state), true);
    _keys.assign(state.pressedKeys, state.pressedKeys + state.pressedKeyCount);
    _chars.assign(state.characters, state.characters + state.characterCount);
    return GUI::UIInput {
        .viewport = _engine.WindowExtent(window), .dt = dt,
        .mouseX = state.mouseX, .mouseY = state.mouseY, .mouseScroll = state.mouseWheel,
        .mouseDown = {state.IsMouseButtonDown(MouseButton::Left), state.IsMouseButtonDown(MouseButton::Right), state.IsMouseButtonDown(MouseButton::Middle)},
        .modifiers = state.modifiers, .pressedKeys = _keys, .characterStream = _chars,
        .clipboard = {
            .userdata = &_engine,
            .set = +[](void* data, std::string_view text) { (void)static_cast<COM::IEngine*>(data)->SetClipboardText(text); },
            .get = +[](void* data) -> std::string {
                auto& engine = *static_cast<COM::IEngine*>(data);
                auto size = engine.ClipboardText(nullptr, 0);
                if (!size.HasValue()) return {};
                std::string text(size.value, '\0');
                auto copied = engine.ClipboardText(text.data(), static_cast<uint32_t>(text.size()));
                if (!copied.HasValue() || copied.value > text.size()) return {};
                text.resize(copied.value); return text;
            },
        },
    };
}
auto HostUI::BeginFrame() -> COM::Result<void> {
    std::vector<WindowID> windows(_engine.Windows(nullptr, 0));
    const auto total = _engine.Windows(windows.data(), static_cast<uint32_t>(windows.size()));
    windows.resize(std::min<size_t>(total, windows.size()));
    for (const auto id: windows) {
        auto cleared = _engine.SubmitUI(id, COM::UISubmitInfo {});
        if (!cleared.HasValue()) return cleared;
    }
    return {};
}
auto HostUI::Submit(UIDrawData data, WindowID window) -> COM::Result<void> {
    const auto result = _engine.SubmitUI(window, COM::UISubmitInfo {
        .batches = data.batches, .positions = data.positions, .surfaces = data.surfaces,
        .wantsCaptureMouse = data.wantsCaptureMouse, .wantsCaptureKeyboard = data.wantsCaptureKeyboard});
    if (!result.HasValue()) return result;
    const auto size = _engine.WindowExtent(window);
    if (_engine.GetCursorMode(window) == CursorMode::Custom && !_engine.IsMouseCaptured(window) && size.width && size.height) {
        COM::HostInputInfo state;
        const auto read = _engine.ReadHostInput(window, COM::OutChain(state), false);
        if (!read.HasValue()) return read;
        const CustomCursor cursor(std::clamp(state.mouseX, 0.0f, float(size.width - 1)), std::clamp(state.mouseY, 0.0f, float(size.height - 1)));
        const auto draw = cursor.DrawData();
        return _engine.SubmitUI(window, COM::UISubmitInfo {.batches = draw.batches, .positions = draw.positions, .surfaces = draw.surfaces,
            .wantsCaptureMouse = data.wantsCaptureMouse, .wantsCaptureKeyboard = data.wantsCaptureKeyboard, .append = true});
    }
    return {};
}
}
