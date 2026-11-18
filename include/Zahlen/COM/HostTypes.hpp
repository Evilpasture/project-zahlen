// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/Types.hpp>
#include <Zahlen/GUI/UIData.hpp>
#include <Zahlen/Window/WindowInput.hpp>

namespace ZHLN::COM {
// Read-only physical UI input, per window. No router/injection/evaluation API.
struct HostInputInfo {
    float mouseX = 0, mouseY = 0, mouseWheel = 0;
    KeyModifiers modifiers = KeyModifiers::None;
    bool keyDown[kKeyCodeCount] {};
    KeyCode pressedKeys[64] {};
    uint32_t characters[64] {};
    uint32_t pressedKeyCount = 0, characterCount = 0;
    [[nodiscard]] bool IsKeyDown(KeyCode key) const noexcept {
        const auto i = static_cast<uint32_t>(key);
        return i < kKeyCodeCount && keyDown[i];
    }
    [[nodiscard]] bool IsMouseButtonDown(MouseButton button) const noexcept { return IsKeyDown(ToKeyCode(button)); }
};
static_assert(std::is_standard_layout_v<HostInputInfo> && std::is_trivially_copyable_v<HostInputInfo>);
struct WindowCreateInfo {
    StringView title {};
    uint32_t width = 1280;
    uint32_t height = 720;
    bool fullscreen = false;
};
struct FileDropView {
    StringView format {};
    StringView fileName {};
    StringView sourcePath {};
    Span<const uint8_t> data {};
    uint64_t byteSize = 0;
};
using FileDropCallback = void (*)(void*, WindowID, Span<const FileDropView>);
// Geometry is copied during SubmitUI. Caller spans need live only for the call.
struct UISubmitInfo {
    Span<const UIBatch> batches {};
    Span<const VertexPosition> positions {};
    Span<const VertexSurface> surfaces {};
    bool wantsCaptureMouse = false;
    bool wantsCaptureKeyboard = false;
    bool append = false;
};
static_assert(std::is_standard_layout_v<WindowCreateInfo> && std::is_trivially_copyable_v<WindowCreateInfo>);
static_assert(std::is_standard_layout_v<FileDropView> && std::is_trivially_copyable_v<FileDropView>);
static_assert(std::is_standard_layout_v<UISubmitInfo> && std::is_trivially_copyable_v<UISubmitInfo>);
}
