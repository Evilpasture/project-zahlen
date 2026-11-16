// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include <NativeHostUI/NativeHostUI.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/HostWindow.hpp>
#include <Zahlen/Engine/Capabilities/InputActions.hpp>
#include <Zahlen/Engine/Capabilities/InputReader.hpp>
#include <algorithm>
#include <limits>

namespace ZHLN {
auto NativeHostUI::GetFontAtlas() -> const FontAtlas& {
    const auto generation = _engine.DeviceGeneration();
    if (_fontGeneration == generation) return _font;
    GUI::BakedFontAsset asset;
    if (!GUI::LoadBakedFont(asset)) asset = GUI::GetDefaultBakedFont();
    _font = GUI::BuildFontAtlas(asset);
    if (asset.atlasWidth && asset.atlasHeight && !asset.coverage.empty()) {
        std::vector<uint32_t> rgba(asset.coverage.size());
        for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = (uint32_t(asset.coverage[i]) << 24) | 0x00ffffffu;
        auto texture = _engine.Graphics().CreateTexture("HostFontAtlas", std::as_bytes(std::span(rgba)),
                                                       {asset.atlasWidth, asset.atlasHeight}, false, false);
        if (texture) _font = GUI::BuildFontAtlas(asset, *texture);
    }
    _fontGeneration = generation;
    return _font;
}
void NativeHostUI::SubmitInput(GUI::UIInput input) {
    _suppliedKeys.assign(input.pressedKeys.begin(), input.pressedKeys.end());
    _suppliedChars.assign(input.characterStream.begin(), input.characterStream.end());
    input.pressedKeys = _suppliedKeys;
    input.characterStream = _suppliedChars;
    _supplied = input;
}
auto NativeHostUI::ConsumeInput(float dt, WindowID window) -> GUI::UIInput {
    if (_supplied && window == WindowID::Primary) {
        auto out = *_supplied;
        out.dt = dt;
        out.viewport = _engine.Host(window).Extent();
        out.pressedKeys = _suppliedKeys;
        out.characterStream = _suppliedChars;
        _supplied.reset();
        return out;
    }
    const auto state = _engine.Input(window).ConsumePhysicalInput();
    _keys.clear(); _chars.clear();
    for (size_t i = 0; i < state.queuedInputCount; ++i) {
        const auto& event = state.queuedInput[i];
        if (event.isChar) _chars.push_back(event.codepoint); else _keys.push_back(event.key);
    }
    return GUI::UIInput {
        .viewport = _engine.Host(window).Extent(), .dt = dt,
        .mouseX = state.mouseX, .mouseY = state.mouseY, .mouseScroll = state.mouseWheel,
        .mouseDown = {state.IsMouseButtonDown(MouseButton::Left), state.IsMouseButtonDown(MouseButton::Right), state.IsMouseButtonDown(MouseButton::Middle)},
        .modifiers = state.Modifiers(), .pressedKeys = _keys, .characterStream = _chars,
        .clipboard = _engine.Host(window).ClipboardSink(),
    };
}
void NativeHostUI::Submit(UIDrawData data, WindowID window) {
    _engine.Input(window).SetUICapture(data.wantsCaptureKeyboard, data.wantsCaptureMouse);
    _engine.Graphics().SubmitUI(window, data);
    const auto host = _engine.Host(window);
    const auto size = host.Extent();
    if (host.CustomCursor() && size.width && size.height) {
        const auto pos = std::as_const(_engine).Input(window).MousePosition();
        const CustomCursor cursor(std::clamp(pos.x, 0.0f, float(size.width - 1)), std::clamp(pos.y, 0.0f, float(size.height - 1)));
        _engine.Graphics().SubmitUI(window, cursor.DrawData(), true);
    }
}
void NativeHostUI::SetCallback(WindowID window, Callback callback) {
    if (callback) _callbacks[window] = std::move(callback); else _callbacks.erase(window);
}
void NativeHostUI::Update(float) {
    // Host policy, executed explicitly before Tick, never a core frame step.
    const auto callbacks = _callbacks;
    for (const auto id: _engine.WindowIDs()) _engine.Input(id).SetUICapture(false, false);
    for (const auto& [id, callback]: callbacks) {
        if (_engine.Host(id).Running()) callback(_engine, id);
    }
}
}
