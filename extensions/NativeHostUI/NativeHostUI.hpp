// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/GUI/GUI.hpp>
#include <Zahlen/GUI/FontLoader.hpp>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace ZHLN {
class Engine;

// Host-owned UI state. Must not outlive its Engine. Engine never owns this
// object, a GUI context, a font atlas, or a UI callback. Bespoke hosts may use
// the individual GUI/value APIs instead of this convenience composition.
class NativeHostUI {
public:
    explicit NativeHostUI(Engine& engine): _engine(engine) {}
    NativeHostUI(const NativeHostUI&) = delete;
    NativeHostUI& operator=(const NativeHostUI&) = delete;
    auto GetFontAtlas() -> const FontAtlas&;
    auto Context() -> GUI::Context& { _gui.SetFontAtlas(GetFontAtlas()); return _gui; }
    auto ConsumeInput(float dt, WindowID window = WindowID::Primary) -> GUI::UIInput;
    void SubmitInput(GUI::UIInput input);
    void Submit(UIDrawData data, WindowID window = WindowID::Primary);
    using Callback = std::function<void(Engine&, WindowID)>;
    void SetCallback(Callback callback) { SetCallback(WindowID::Primary, std::move(callback)); }
    void SetCallback(WindowID window, Callback callback);
    void Update(float dt);
private:
    Engine& _engine;
    FontAtlas _font {};
    GUI::Context _gui;
    uint64_t _fontGeneration = ~uint64_t {0};
    std::optional<GUI::UIInput> _supplied;
    std::vector<KeyCode> _suppliedKeys;
    std::vector<uint32_t> _suppliedChars;
    std::vector<KeyCode> _keys;
    std::vector<uint32_t> _chars;
    std::map<WindowID, Callback> _callbacks;
};
}
