// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/GUI/GUI.hpp>
#include <Zahlen/GUI/FontLoader.hpp>
#include <optional>
#include <vector>

namespace ZHLN {
// Owned by Application (or an explicit external host), never the engine.
// No callbacks: the host composes each window and submits before Tick.
class HostUI {
public:
    explicit HostUI(COM::IEngine& engine): _engine(engine) {}
    ~HostUI();
    HostUI(const HostUI&) = delete;
    HostUI& operator=(const HostUI&) = delete;
    auto GetFontAtlas() -> const FontAtlas&;
    void SetBakedFont(GUI::BakedFontAsset asset);
    auto Context() -> GUI::Context& { _gui.SetFontAtlas(GetFontAtlas()); return _gui; }
    auto ConsumeInput(float dt, WindowID window = WindowID::Primary) -> GUI::UIInput;
    void SubmitInput(GUI::UIInput input);
    [[nodiscard]] auto Submit(UIDrawData data, WindowID window = WindowID::Primary) -> COM::Result<void>;
    [[nodiscard]] auto BeginFrame() -> COM::Result<void>;
private:
    COM::IEngine& _engine; // borrowed from the owning Application's Ref
    FontAtlas _font {};
    GUI::BakedFontAsset _baked;
    TextureID _texture = TextureID::Invalid;
    GUI::Context _gui;
    uint64_t _fontGeneration = ~uint64_t {0};
    std::optional<GUI::UIInput> _supplied;
    std::vector<KeyCode> _suppliedKeys, _keys;
    std::vector<uint32_t> _suppliedChars, _chars;
};
}
