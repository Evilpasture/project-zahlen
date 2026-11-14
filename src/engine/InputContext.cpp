// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/EnumFlags.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include "InputContext.hpp"
#include <Zahlen/ECS/EventBus.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numeric>

namespace ZHLN {

static_assert(!Reflect::ReflectionAvailable || Reflect::EnumCount<KeyCode>() == kKeyCodeCount, "KeyCode enumerators must stay contiguous with kKeyCodeCount");
static_assert(!Reflect::ReflectionAvailable || Reflect::EnumCount<GamepadButton>() == kGamepadButtonCount, "GamepadButton enumerators must stay contiguous");
static_assert(!Reflect::ReflectionAvailable || Reflect::EnumCount<GamepadAxis>() == kGamepadAxisCount, "GamepadAxis enumerators must stay contiguous");

namespace {

[[nodiscard]] constexpr auto IsModifierKeyCode(KeyCode key) noexcept -> bool {
    switch (key) {
        case KeyCode::LShift:
        case KeyCode::RShift:
        case KeyCode::LControl:
        case KeyCode::RControl:
        case KeyCode::LAlt:
        case KeyCode::RAlt:
        case KeyCode::LSuper:
        case KeyCode::RSuper:
            return true;
        default:
            return false;
    }
}

struct SeatConsumptionMask {
    std::bitset<kKeyCodeCount>           keys {};
    std::bitset<kGamepadButtonCount>     gamepadButtons {};
    EnumBitMask<GamepadAxis>             gamepadAxes {};
    bool                                 mouseMotionX = false;
    bool                                 mouseMotionY = false;
    bool                                 mouseWheel   = false;

    [[nodiscard]] auto IsKeyConsumed(KeyCode key) const noexcept -> bool {
        const size_t idx = EnumIndex(key);
        return key != KeyCode::Unknown && idx < kKeyCodeCount && keys.test(idx);
    }

    void MarkKey(KeyCode key) noexcept {
        const size_t idx = EnumIndex(key);
        if (key != KeyCode::Unknown && idx < kKeyCodeCount) {
            keys.set(idx);
        }
    }

    [[nodiscard]] auto IsGamepadButtonConsumed(GamepadButton button) const noexcept -> bool {
        const size_t idx = EnumIndex(button);
        return idx < kGamepadButtonCount && gamepadButtons.test(idx);
    }

    void MarkGamepadButton(GamepadButton button) noexcept {
        const size_t idx = EnumIndex(button);
        if (idx < kGamepadButtonCount) {
            gamepadButtons.set(idx);
        }
    }
};

struct BindingEvalSample {
    float           x         = 0.0f;
    float           y         = 0.0f;
    bool            active    = false;
    InputDeviceKind device    = InputDeviceKind::KeyboardMouse;
};

[[nodiscard]] auto CheckBindingModifiers(
    const InputState& kbm,
    KeyModifiers      required,
    bool              strict,
    UiCaptureState    uiCapture,
    bool              bypassUiCapture
) noexcept -> bool {
    const UiCaptureState effective = bypassUiCapture ? UiCaptureState {} : uiCapture;
    const KeyModifiers   current   = kbm.Modifiers(effective);
    if (strict) {
        return current == required;
    }
    return (current & required) == required;
}

[[nodiscard]] constexpr auto IsValidKeyCode(KeyCode key) noexcept -> bool {
    return key != KeyCode::Unknown && EnumIndex(key) < kKeyCodeCount;
}

[[nodiscard]] constexpr auto IsValidGamepadButton(GamepadButton button) noexcept -> bool {
    return EnumIndex(button) < kGamepadButtonCount;
}

[[nodiscard]] constexpr auto IsValidGamepadAxis(GamepadAxis axis) noexcept -> bool {
    return EnumIndex(axis) < kGamepadAxisCount;
}

[[nodiscard]] auto KeyDownForBinding(
    const InputState& kbm,
    KeyCode           key,
    UiCaptureState    uiCapture,
    bool              bypassUiCapture
) noexcept -> bool {
    if (!IsValidKeyCode(key)) {
        return false;
    }
    const UiCaptureState effective = bypassUiCapture ? UiCaptureState {} : uiCapture;
    if (const auto mb = MouseButtonFromKeyCode(key); mb.has_value()) {
        return kbm.IsMouseButtonDown(*mb, effective);
    }
    return kbm.IsKeyDown(key, effective);
}

[[nodiscard]] auto KeyPressedEdgeForBinding(
    const InputState& kbm,
    KeyCode           key,
    UiCaptureState    uiCapture,
    bool              bypassUiCapture
) noexcept -> bool {
    if (!IsValidKeyCode(key)) {
        return false;
    }
    const UiCaptureState effective = bypassUiCapture ? UiCaptureState {} : uiCapture;
    if (const auto mb = MouseButtonFromKeyCode(key); mb.has_value()) {
        return kbm.IsMouseButtonPressed(*mb, effective);
    }
    return kbm.IsKeyPressed(key, effective);
}

[[nodiscard]] auto EvaluateSingleBinding(
    const InputBinding&                  binding,
    ActionKind                           actionKind,
    float                                actionThreshold,
    const InputState*                    kbm,
    UiCaptureState                       uiCapture,
    std::span<const GamepadState* const> seatGamepads,
    const SeatConsumptionMask&           consumed
) noexcept -> BindingEvalSample {
    BindingEvalSample out {};

    const auto evalKeyLike =
        [&](KeyCode keyCode, KeyModifiers mods, bool strictMods, bool bypassUiCapture, float scale) noexcept {
        if (kbm == nullptr || !IsValidKeyCode(keyCode) || consumed.IsKeyConsumed(keyCode)) {
            return;
        }
        if (!CheckBindingModifiers(*kbm, mods, strictMods, uiCapture, bypassUiCapture)) {
            return;
        }
        const bool isDown    = KeyDownForBinding(*kbm, keyCode, uiCapture, bypassUiCapture);
        const bool tapRising = KeyPressedEdgeForBinding(*kbm, keyCode, uiCapture, bypassUiCapture);
        if (isDown || tapRising) {
            out.active = true;
            out.x      = scale;
            out.device = InputDeviceKind::KeyboardMouse;
        }
    };

    switch (binding.kind) {
        case InputBindingKind::None:
            break;

        case InputBindingKind::Key:
            evalKeyLike(binding.keys[0], binding.modifiers, binding.strictModifiers, binding.bypassUiCapture, binding.scaleX);
            break;

        case InputBindingKind::MouseButton:
            evalKeyLike(ToKeyCode(binding.mouseButton), binding.modifiers, binding.strictModifiers, binding.bypassUiCapture, binding.scaleX);
            break;

        case InputBindingKind::KeyChord: {
            const KeyCode k0 = binding.keys[0];
            const KeyCode k1 = binding.keys[1];
            if (kbm == nullptr || !IsValidKeyCode(k0) || !IsValidKeyCode(k1)) {
                break;
            }
            if (consumed.IsKeyConsumed(k0) || consumed.IsKeyConsumed(k1)) {
                break;
            }
            if (!CheckBindingModifiers(*kbm, binding.modifiers, binding.strictModifiers, uiCapture, binding.bypassUiCapture)) {
                break;
            }
            const bool d0 = KeyDownForBinding(*kbm, k0, uiCapture, binding.bypassUiCapture) ||
                            KeyPressedEdgeForBinding(*kbm, k0, uiCapture, binding.bypassUiCapture);
            const bool d1 = KeyDownForBinding(*kbm, k1, uiCapture, binding.bypassUiCapture) ||
                            KeyPressedEdgeForBinding(*kbm, k1, uiCapture, binding.bypassUiCapture);
            if (d0 && d1) {
                out.active = true;
                out.x      = binding.scaleX;
                out.device = InputDeviceKind::KeyboardMouse;
            }
            break;
        }

        case InputBindingKind::MouseDeltaX: {
            if (kbm == nullptr || consumed.mouseMotionX) {
                break;
            }
            const float dx = kbm->GetMouseDeltaX(binding.bypassUiCapture ? UiCaptureState {} : uiCapture);
            if (std::abs(dx) > 1e-6f) {
                out.x      = dx * binding.scaleX;
                out.active = std::abs(out.x) >= actionThreshold || actionKind != ActionKind::Digital;
                out.device = InputDeviceKind::KeyboardMouse;
            }
            break;
        }

        case InputBindingKind::MouseDeltaY: {
            if (kbm == nullptr || consumed.mouseMotionY) {
                break;
            }
            const float dy = kbm->GetMouseDeltaY(binding.bypassUiCapture ? UiCaptureState {} : uiCapture);
            if (std::abs(dy) > 1e-6f) {
                out.x      = dy * binding.scaleX;
                out.active = std::abs(out.x) >= actionThreshold || actionKind != ActionKind::Digital;
                out.device = InputDeviceKind::KeyboardMouse;
            }
            break;
        }

        case InputBindingKind::MouseDelta2D: {
            if (kbm == nullptr || (consumed.mouseMotionX && consumed.mouseMotionY)) {
                break;
            }
            const float dx = consumed.mouseMotionX ? 0.0f : kbm->GetMouseDeltaX(binding.bypassUiCapture ? UiCaptureState {} : uiCapture);
            const float dy = consumed.mouseMotionY ? 0.0f : kbm->GetMouseDeltaY(binding.bypassUiCapture ? UiCaptureState {} : uiCapture);
            if (std::abs(dx) > 1e-6f || std::abs(dy) > 1e-6f) {
                out.x      = dx * binding.scaleX;
                out.y      = dy * binding.scaleY;
                out.active = true;
                out.device = InputDeviceKind::KeyboardMouse;
            }
            break;
        }

        case InputBindingKind::MouseWheel: {
            if (kbm == nullptr || consumed.mouseWheel) {
                break;
            }
            const float w = kbm->GetMouseWheel(binding.bypassUiCapture ? UiCaptureState {} : uiCapture);
            if (std::abs(w) > 1e-6f) {
                out.x      = w * binding.scaleX;
                out.active = (actionKind == ActionKind::Digital) ? (std::abs(out.x) >= 0.01f) : true;
                out.device = InputDeviceKind::KeyboardMouse;
            }
            break;
        }

        case InputBindingKind::GamepadButton: {
            const GamepadButton btn = binding.gamepadButtons[0];
            if (!IsValidGamepadButton(btn) || consumed.IsGamepadButtonConsumed(btn)) {
                break;
            }
            for (const GamepadState* gp : seatGamepads) {
                if (gp != nullptr && (gp->IsButtonDown(btn) || gp->IsButtonPressed(btn))) {
                    out.active = true;
                    out.x      = binding.scaleX;
                    out.device = InputDeviceKind::Gamepad;
                    break;
                }
            }
            break;
        }

        case InputBindingKind::GamepadChord: {
            const GamepadButton b0 = binding.gamepadButtons[0];
            const GamepadButton b1 = binding.gamepadButtons[1];
            if (!IsValidGamepadButton(b0) || !IsValidGamepadButton(b1)) {
                break;
            }
            if (consumed.IsGamepadButtonConsumed(b0) || consumed.IsGamepadButtonConsumed(b1)) {
                break;
            }
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                const bool d0 = gp->IsButtonDown(b0) || gp->IsButtonPressed(b0);
                const bool d1 = gp->IsButtonDown(b1) || gp->IsButtonPressed(b1);
                if (d0 && d1) {
                    out.active = true;
                    out.x      = binding.scaleX;
                    out.device = InputDeviceKind::Gamepad;
                    break;
                }
            }
            break;
        }

        case InputBindingKind::GamepadAxis1D: {
            const GamepadAxis axis = binding.gamepadAxes[0];
            if (!IsValidGamepadAxis(axis) || consumed.gamepadAxes.Contains(axis)) {
                break;
            }
            const float thresh = (binding.threshold > 0.0f) ? binding.threshold : actionThreshold;
            float       best   = 0.0f;
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                const float v = gp->Axis(axis) * binding.scaleX;
                if (std::abs(v) > std::abs(best)) {
                    best = v;
                }
            }
            if (actionKind == ActionKind::Digital) {
                if (std::abs(best) >= thresh) {
                    out.active = true;
                    out.x      = best;
                    out.device = InputDeviceKind::Gamepad;
                }
            } else if (std::abs(best) > 1e-5f) {
                out.active = std::abs(best) >= thresh;
                out.x      = best;
                out.device = InputDeviceKind::Gamepad;
            }
            break;
        }

        case InputBindingKind::Composite1DKeys: {
            if (kbm == nullptr) {
                break;
            }
            float sum = 0.0f;
            if (!consumed.IsKeyConsumed(binding.keys[0]) && KeyDownForBinding(*kbm, binding.keys[0], uiCapture, binding.bypassUiCapture)) {
                sum -= 1.0f;
            }
            if (!consumed.IsKeyConsumed(binding.keys[1]) && KeyDownForBinding(*kbm, binding.keys[1], uiCapture, binding.bypassUiCapture)) {
                sum += 1.0f;
            }
            if (std::abs(sum) > 1e-5f) {
                out.x      = sum * binding.scaleX;
                out.active = true;
                out.device = InputDeviceKind::KeyboardMouse;
            }
            break;
        }

        case InputBindingKind::Composite1DGamepad: {
            const GamepadButton neg = binding.gamepadButtons[0];
            const GamepadButton pos = binding.gamepadButtons[1];
            float               best = 0.0f;
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                float sum = 0.0f;
                if (IsValidGamepadButton(neg) && !consumed.IsGamepadButtonConsumed(neg) && gp->IsButtonDown(neg)) {
                    sum -= 1.0f;
                }
                if (IsValidGamepadButton(pos) && !consumed.IsGamepadButtonConsumed(pos) && gp->IsButtonDown(pos)) {
                    sum += 1.0f;
                }
                if (std::abs(sum) > std::abs(best)) {
                    best = sum;
                }
            }
            if (std::abs(best) > 1e-5f) {
                out.x      = best * binding.scaleX;
                out.active = true;
                out.device = InputDeviceKind::Gamepad;
            }
            break;
        }

        case InputBindingKind::Composite2DKeys: {
            if (kbm == nullptr) {
                break;
            }
            float x = 0.0f;
            float y = 0.0f;
            if (!consumed.IsKeyConsumed(binding.keys[0]) && KeyDownForBinding(*kbm, binding.keys[0], uiCapture, binding.bypassUiCapture)) {
                y += 1.0f;
            }
            if (!consumed.IsKeyConsumed(binding.keys[1]) && KeyDownForBinding(*kbm, binding.keys[1], uiCapture, binding.bypassUiCapture)) {
                y -= 1.0f;
            }
            if (!consumed.IsKeyConsumed(binding.keys[2]) && KeyDownForBinding(*kbm, binding.keys[2], uiCapture, binding.bypassUiCapture)) {
                x -= 1.0f;
            }
            if (!consumed.IsKeyConsumed(binding.keys[3]) && KeyDownForBinding(*kbm, binding.keys[3], uiCapture, binding.bypassUiCapture)) {
                x += 1.0f;
            }
            if (std::abs(x) > 1e-5f || std::abs(y) > 1e-5f) {
                out.x      = x * binding.scaleX;
                out.y      = y * binding.scaleY;
                out.active = true;
                out.device = InputDeviceKind::KeyboardMouse;
            }
            break;
        }

        case InputBindingKind::Composite2DGamepad: {
            const GamepadButton up    = binding.gamepadButtons[0];
            const GamepadButton down  = binding.gamepadButtons[1];
            const GamepadButton left  = binding.gamepadButtons[2];
            const GamepadButton right = binding.gamepadButtons[3];
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                float x = 0.0f;
                float y = 0.0f;
                if (IsValidGamepadButton(up) && !consumed.IsGamepadButtonConsumed(up) && gp->IsButtonDown(up)) {
                    y += 1.0f;
                }
                if (IsValidGamepadButton(down) && !consumed.IsGamepadButtonConsumed(down) && gp->IsButtonDown(down)) {
                    y -= 1.0f;
                }
                if (IsValidGamepadButton(left) && !consumed.IsGamepadButtonConsumed(left) && gp->IsButtonDown(left)) {
                    x -= 1.0f;
                }
                if (IsValidGamepadButton(right) && !consumed.IsGamepadButtonConsumed(right) && gp->IsButtonDown(right)) {
                    x += 1.0f;
                }
                if (std::abs(x) > 1e-5f || std::abs(y) > 1e-5f) {
                    out.x      = x * binding.scaleX;
                    out.y      = y * binding.scaleY;
                    out.active = true;
                    out.device = InputDeviceKind::Gamepad;
                    break;
                }
            }
            break;
        }

        case InputBindingKind::GamepadStick2D: {
            const GamepadAxis xAxis = binding.gamepadAxes[0];
            const GamepadAxis yAxis = binding.gamepadAxes[1];
            if (!IsValidGamepadAxis(xAxis) || !IsValidGamepadAxis(yAxis)) {
                break;
            }
            if (consumed.gamepadAxes.Contains(xAxis) || consumed.gamepadAxes.Contains(yAxis)) {
                break;
            }
            float bestLenSq = 0.0f;
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                float sx = 0.0f;
                float sy = 0.0f;
                if (xAxis == GamepadAxis::LeftX && yAxis == GamepadAxis::LeftY) {
                    const StickSample stick = gp->LeftStick();
                    sx                      = stick.x;
                    sy                      = stick.y;
                } else if (xAxis == GamepadAxis::RightX && yAxis == GamepadAxis::RightY) {
                    const StickSample stick = gp->RightStick();
                    sx                      = stick.x;
                    sy                      = stick.y;
                } else {
                    sx = gp->Axis(xAxis);
                    sy = gp->Axis(yAxis);
                }
                sx *= binding.scaleX;
                sy *= binding.scaleY;
                const float lenSq = (sx * sx) + (sy * sy);
                if (lenSq > bestLenSq) {
                    bestLenSq  = lenSq;
                    out.x      = sx;
                    out.y      = sy;
                    out.active = lenSq > 1e-5f;
                    out.device = InputDeviceKind::Gamepad;
                }
            }
            break;
        }
    }

    return out;
}

void MarkBindingConsumed(
    const InputBinding&                  binding,
    const InputState*                    kbm,
    UiCaptureState                       uiCapture,
    std::span<const GamepadState* const> seatGamepads,
    SeatConsumptionMask&                 consumed
) noexcept {
    switch (binding.kind) {
        case InputBindingKind::None:
            break;
        case InputBindingKind::Key:
            consumed.MarkKey(binding.keys[0]);
            break;
        case InputBindingKind::MouseButton:
            consumed.MarkKey(ToKeyCode(binding.mouseButton));
            break;
        case InputBindingKind::KeyChord:
            consumed.MarkKey(binding.keys[0]);
            consumed.MarkKey(binding.keys[1]);
            break;
        case InputBindingKind::MouseDeltaX:
            consumed.mouseMotionX = true;
            break;
        case InputBindingKind::MouseDeltaY:
            consumed.mouseMotionY = true;
            break;
        case InputBindingKind::MouseDelta2D:
            consumed.mouseMotionX = true;
            consumed.mouseMotionY = true;
            break;
        case InputBindingKind::MouseWheel:
            consumed.mouseWheel = true;
            break;
        case InputBindingKind::GamepadButton:
            consumed.MarkGamepadButton(binding.gamepadButtons[0]);
            break;
        case InputBindingKind::GamepadChord:
            consumed.MarkGamepadButton(binding.gamepadButtons[0]);
            consumed.MarkGamepadButton(binding.gamepadButtons[1]);
            break;
        case InputBindingKind::GamepadAxis1D:
            if (IsValidGamepadAxis(binding.gamepadAxes[0])) {
                consumed.gamepadAxes.Set(binding.gamepadAxes[0]);
            }
            break;
        case InputBindingKind::Composite1DKeys:
            if (kbm != nullptr) {
                for (const KeyCode k : {binding.keys[0], binding.keys[1]}) {
                    if (KeyDownForBinding(*kbm, k, uiCapture, binding.bypassUiCapture)) {
                        consumed.MarkKey(k);
                    }
                }
            }
            break;
        case InputBindingKind::Composite1DGamepad:
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                for (const GamepadButton b : {binding.gamepadButtons[0], binding.gamepadButtons[1]}) {
                    if (IsValidGamepadButton(b) && gp->IsButtonDown(b)) {
                        consumed.MarkGamepadButton(b);
                    }
                }
            }
            break;
        case InputBindingKind::Composite2DKeys:
            if (kbm != nullptr) {
                for (const KeyCode k : binding.keys) {
                    if (KeyDownForBinding(*kbm, k, uiCapture, binding.bypassUiCapture)) {
                        consumed.MarkKey(k);
                    }
                }
            }
            break;
        case InputBindingKind::Composite2DGamepad:
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                for (const GamepadButton b : binding.gamepadButtons) {
                    if (IsValidGamepadButton(b) && gp->IsButtonDown(b)) {
                        consumed.MarkGamepadButton(b);
                    }
                }
            }
            break;
        case InputBindingKind::GamepadStick2D:
            if (IsValidGamepadAxis(binding.gamepadAxes[0]) && IsValidGamepadAxis(binding.gamepadAxes[1])) {
                consumed.gamepadAxes.Set(binding.gamepadAxes[0]);
                consumed.gamepadAxes.Set(binding.gamepadAxes[1]);
            }
            break;
    }
}

[[nodiscard]] auto BindingsOverlapPhysically(const InputBinding& a, const InputBinding& b) noexcept -> bool {
    if (a.kind == InputBindingKind::None || b.kind == InputBindingKind::None) {
        return false;
    }
    if (a == b) {
        return true;
    }
    if (a.kind != b.kind) {
        // MouseButton(Left) overlaps Key(LButton)
        if (a.kind == InputBindingKind::MouseButton && b.kind == InputBindingKind::Key) {
            return ToKeyCode(a.mouseButton) == b.keys[0] && a.modifiers == b.modifiers;
        }
        if (b.kind == InputBindingKind::MouseButton && a.kind == InputBindingKind::Key) {
            return ToKeyCode(b.mouseButton) == a.keys[0] && a.modifiers == b.modifiers;
        }
        return false;
    }
    switch (a.kind) {
        case InputBindingKind::Key:
            return a.keys[0] == b.keys[0] && a.modifiers == b.modifiers;
        case InputBindingKind::MouseButton:
            return a.mouseButton == b.mouseButton && a.modifiers == b.modifiers;
        case InputBindingKind::KeyChord: {
            const bool samePair = (a.keys[0] == b.keys[0] && a.keys[1] == b.keys[1]) ||
                                  (a.keys[0] == b.keys[1] && a.keys[1] == b.keys[0]);
            return samePair && a.modifiers == b.modifiers;
        }
        case InputBindingKind::GamepadButton:
            return a.gamepadButtons[0] == b.gamepadButtons[0];
        case InputBindingKind::GamepadAxis1D:
            return a.gamepadAxes[0] == b.gamepadAxes[0];
        case InputBindingKind::GamepadChord:
            return (a.gamepadButtons[0] == b.gamepadButtons[0] && a.gamepadButtons[1] == b.gamepadButtons[1]) ||
                   (a.gamepadButtons[0] == b.gamepadButtons[1] && a.gamepadButtons[1] == b.gamepadButtons[0]);
        case InputBindingKind::MouseDeltaX:
        case InputBindingKind::MouseDeltaY:
        case InputBindingKind::MouseDelta2D:
        case InputBindingKind::MouseWheel:
            return true;
        case InputBindingKind::Composite1DKeys:
            return a.keys[0] == b.keys[0] || a.keys[1] == b.keys[1] || a.keys[0] == b.keys[1] || a.keys[1] == b.keys[0];
        case InputBindingKind::Composite1DGamepad:
            return a.gamepadButtons[0] == b.gamepadButtons[0] || a.gamepadButtons[1] == b.gamepadButtons[1] ||
                   a.gamepadButtons[0] == b.gamepadButtons[1] || a.gamepadButtons[1] == b.gamepadButtons[0];
        case InputBindingKind::GamepadStick2D:
            return a.gamepadAxes[0] == b.gamepadAxes[0] || a.gamepadAxes[1] == b.gamepadAxes[1] ||
                   a.gamepadAxes[0] == b.gamepadAxes[1] || a.gamepadAxes[1] == b.gamepadAxes[0];
        case InputBindingKind::Composite2DKeys:
            for (const KeyCode x : a.keys) {
                for (const KeyCode y : b.keys) {
                    if (x != KeyCode::Unknown && x == y) {
                        return true;
                    }
                }
            }
            return false;
        case InputBindingKind::Composite2DGamepad:
            for (const GamepadButton x : a.gamepadButtons) {
                for (const GamepadButton y : b.gamepadButtons) {
                    if (x == y) {
                        return true;
                    }
                }
            }
            return false;
        case InputBindingKind::None:
            return false;
    }
    return false;
}

[[nodiscard]] constexpr auto TrimWhitespace(std::string_view sv) noexcept -> std::string_view {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
        sv.remove_suffix(1);
    }
    return sv;
}

[[nodiscard]] auto FormatModifiers(KeyModifiers mods) -> std::string {
    if (mods == KeyModifiers::None) {
        return "None";
    }
    std::string out;
    const auto  appendFlag = [&](KeyModifiers bit, std::string_view label) {
        if ((mods & bit) != KeyModifiers::None) {
            if (!out.empty()) {
                out.push_back('+');
            }
            out.append(label);
        }
    };
    appendFlag(KeyModifiers::Shift, "Shift");
    appendFlag(KeyModifiers::Ctrl, "Ctrl");
    appendFlag(KeyModifiers::Alt, "Alt");
    appendFlag(KeyModifiers::Super, "Super");
    return out;
}

[[nodiscard]] auto ParseModifiers(std::string_view token) noexcept -> Optional<KeyModifiers> {
    if (token.empty() || token == "None") {
        return KeyModifiers::None;
    }
    KeyModifiers mods = KeyModifiers::None;
    size_t       pos  = 0;
    while (pos < token.size()) {
        const size_t           plus = token.find('+', pos);
        const std::string_view part = (plus == std::string_view::npos) ? token.substr(pos) : token.substr(pos, plus - pos);
        if (part == "Shift") {
            mods |= KeyModifiers::Shift;
        } else if (part == "Ctrl") {
            mods |= KeyModifiers::Ctrl;
        } else if (part == "Alt") {
            mods |= KeyModifiers::Alt;
        } else if (part == "Super") {
            mods |= KeyModifiers::Super;
        } else {
            return Optional<KeyModifiers> {};
        }
        if (plus == std::string_view::npos) {
            break;
        }
        pos = plus + 1;
    }
    return mods;
}

[[nodiscard]] auto ParseFloatToken(std::string_view token) noexcept -> Optional<float> {
    token = TrimWhitespace(token);
    if (token.empty()) {
        return Optional<float> {};
    }
    float value = 0.0f;
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec != std::errc {} || ptr != token.data() + token.size()) {
        return Optional<float> {};
    }
    return value;
}

[[nodiscard]] auto FormatBindingSlotToken(const InputBinding& binding, size_t index) -> std::string_view {
    switch (binding.kind) {
        case InputBindingKind::Key:
            return (index == 0) ? Reflect::EnumToString(binding.keys[0]) : "-";
        case InputBindingKind::KeyChord:
        case InputBindingKind::Composite1DKeys:
            return (index < 2) ? Reflect::EnumToString(binding.keys[index]) : "-";
        case InputBindingKind::Composite2DKeys:
            return (index < 4) ? Reflect::EnumToString(binding.keys[index]) : "-";
        case InputBindingKind::MouseButton:
            return (index == 0) ? Reflect::EnumToString(binding.mouseButton) : "-";
        case InputBindingKind::GamepadButton:
            return (index == 0) ? Reflect::EnumToString(binding.gamepadButtons[0]) : "-";
        case InputBindingKind::GamepadChord:
        case InputBindingKind::Composite1DGamepad:
            return (index < 2) ? Reflect::EnumToString(binding.gamepadButtons[index]) : "-";
        case InputBindingKind::Composite2DGamepad:
            return (index < 4) ? Reflect::EnumToString(binding.gamepadButtons[index]) : "-";
        case InputBindingKind::GamepadAxis1D:
            return (index == 0) ? Reflect::EnumToString(binding.gamepadAxes[0]) : "-";
        case InputBindingKind::GamepadStick2D:
            return (index < 2) ? Reflect::EnumToString(binding.gamepadAxes[index]) : "-";
        default:
            return "-";
    }
}

template <typename EnumT>
[[nodiscard]] auto ParseEnumSlotToken(std::string_view token, EnumT emptyValue = EnumT {}) noexcept -> Optional<EnumT> {
    if (token == "-") {
        return emptyValue;
    }
    return Reflect::StringToEnum<EnumT>(token);
}

[[nodiscard]] auto ParseBindingCodeTokens(InputBinding& binding, std::span<const std::string_view, 4> tokens) noexcept -> bool {
    switch (binding.kind) {
        case InputBindingKind::Key:
        case InputBindingKind::KeyChord:
        case InputBindingKind::Composite1DKeys:
        case InputBindingKind::Composite2DKeys:
            for (size_t i = 0; i < 4; ++i) {
                const auto parsed = ParseEnumSlotToken<KeyCode>(tokens[i], KeyCode::Unknown);
                if (!parsed) {
                    return false;
                }
                binding.keys[i] = *parsed;
            }
            return true;

        case InputBindingKind::MouseButton: {
            const auto mb = ParseEnumSlotToken<MouseButton>(tokens[0], MouseButton::Left);
            if (!mb) {
                return false;
            }
            binding.mouseButton = *mb;
            for (size_t i = 1; i < 4; ++i) {
                if (!ParseEnumSlotToken<MouseButton>(tokens[i], MouseButton::Left)) {
                    return false;
                }
            }
            return true;
        }

        case InputBindingKind::GamepadButton:
        case InputBindingKind::GamepadChord:
        case InputBindingKind::Composite1DGamepad:
        case InputBindingKind::Composite2DGamepad:
            for (size_t i = 0; i < 4; ++i) {
                const auto parsed = ParseEnumSlotToken<GamepadButton>(tokens[i], GamepadButton::South);
                if (!parsed) {
                    return false;
                }
                binding.gamepadButtons[i] = *parsed;
            }
            return true;

        case InputBindingKind::GamepadAxis1D:
        case InputBindingKind::GamepadStick2D:
            for (size_t i = 0; i < 2; ++i) {
                const auto parsed = ParseEnumSlotToken<GamepadAxis>(tokens[i], GamepadAxis::LeftX);
                if (!parsed) {
                    return false;
                }
                binding.gamepadAxes[i] = *parsed;
            }
            return tokens[2] == "-" && tokens[3] == "-";

        default:
            return true;
    }
}

} // namespace

// =============================================================================
// InputAction
// =============================================================================

InputAction::InputAction(std::string_view name, ActionKind kind) noexcept : _id(name), _name(name), _kind(kind) {}

auto InputAction::Bind(const InputBinding& binding) -> InputAction& {
    _bindings.push_back(binding);
    _defaultBindings = _bindings;
    return *this;
}

auto InputAction::SetClampUnitCircle(bool clamp) noexcept -> InputAction& {
    _clampUnitCircle = clamp;
    return *this;
}

auto InputAction::SetActuationThreshold(float threshold) noexcept -> InputAction& {
    _actuationThreshold = std::clamp(threshold, 0.01f, 1.0f);
    return *this;
}

void InputAction::ClearBindings() noexcept {
    _bindings.clear();
}

void InputAction::SnapshotDefaults() {
    _defaultBindings = _bindings;
}

void InputAction::ResetToDefaults() {
    _bindings = _defaultBindings;
}

auto InputAction::Rebind(size_t slotIndex, const InputBinding& binding) -> bool {
    if (slotIndex >= _bindings.size()) {
        return false;
    }
    _bindings[slotIndex] = binding;
    return true;
}

void InputAction::AppendBinding(const InputBinding& binding) {
    _bindings.push_back(binding);
}

void InputAction::ReplaceBindings(std::span<const InputBinding> bindings) {
    _bindings.assign(bindings.begin(), bindings.end());
}

auto InputAction::RemoveBinding(size_t slotIndex) -> bool {
    if (slotIndex >= _bindings.size()) {
        return false;
    }
    _bindings.erase(_bindings.begin() + static_cast<std::ptrdiff_t>(slotIndex));
    return true;
}

// =============================================================================
// InputActionMap
// =============================================================================

InputActionMap::InputActionMap(std::string_view name, int32_t priority) noexcept
    : _id(Hash64(name)), _name(name), _priority(priority) {}

auto InputActionMap::AddAction(std::string_view name, ActionKind kind) -> InputAction& {
    const ActionID id(name);
    for (InputAction& existing : _actions) {
        if (existing.Id() == id) {
            return existing;
        }
    }
    _actions.emplace_back(name, kind);
    return _actions.back();
}

auto InputActionMap::FindAction(ActionID id) noexcept -> Optional<InputAction&> {
    for (InputAction& action : _actions) {
        if (action.Id() == id) {
            return action;
        }
    }
    return {};
}

auto InputActionMap::FindAction(ActionID id) const noexcept -> Optional<const InputAction&> {
    for (const InputAction& action : _actions) {
        if (action.Id() == id) {
            return action;
        }
    }
    return {};
}

void InputActionMap::SnapshotDefaults() {
    for (InputAction& action : _actions) {
        action.SnapshotDefaults();
    }
}

void InputActionMap::ResetToDefaults() {
    for (InputAction& action : _actions) {
        action.ResetToDefaults();
    }
}

// =============================================================================
// InputContext
// =============================================================================

InputContext::InputContext() {
    for (size_t i = 0; i < kMaxPlayerSeats; ++i) {
        const PlayerSeat seat {static_cast<uint8_t>(i)};
        _seats[i].kbm.seat              = seat;
        _seats[i].receivesKeyboardMouse = (seat == kPrimarySeat);
    }
}

void InputContext::BeginFrame() noexcept {
    for (SeatRuntime& seat : _seats) {
        seat.kbm.BeginFrame();
        for (EvaluatedActionEntry& entry : seat.actions) {
            entry.previous = entry.current;
        }
    }
    for (GamepadState& gp : _gamepads) {
        gp.BeginFrame();
    }
}

void InputContext::InjectKey(KeyCode key, bool down, PlayerSeat targetSeat) noexcept {
    if (!targetSeat.IsValid() || key == KeyCode::Unknown) {
        return;
    }
    SeatRuntime& seat = _seats[targetSeat.Index()];
    seat.kbm.SetKey(key, down);
    if (down) {
        seat.kbm.QueueKeyPress(key);
        seat.lastActiveDevice = InputDeviceKind::KeyboardMouse;
    }
}

void InputContext::InjectMouseButton(MouseButton button, bool down, PlayerSeat targetSeat) noexcept {
    InjectKey(ToKeyCode(button), down, targetSeat);
}

void InputContext::InjectMouseMove(float x, float y, PlayerSeat targetSeat) noexcept {
    if (!targetSeat.IsValid()) {
        return;
    }
    SeatRuntime& seat   = _seats[targetSeat.Index()];
    const float  before = std::abs(seat.kbm.mouseDeltaX) + std::abs(seat.kbm.mouseDeltaY);
    seat.kbm.ApplyLocalMotion(x, y);
    const float after = std::abs(seat.kbm.mouseDeltaX) + std::abs(seat.kbm.mouseDeltaY);
    if (after > before + 0.5f) {
        seat.lastActiveDevice = InputDeviceKind::KeyboardMouse;
    }
}

void InputContext::InjectMouseRelative(float x, float y, float dx, float dy, PlayerSeat targetSeat) noexcept {
    if (!targetSeat.IsValid()) {
        return;
    }
    SeatRuntime& seat   = _seats[targetSeat.Index()];
    const float  before = std::abs(seat.kbm.mouseDeltaX) + std::abs(seat.kbm.mouseDeltaY);
    seat.kbm.ApplyRelativeMotion(x, y, dx, dy);
    const float after = std::abs(seat.kbm.mouseDeltaX) + std::abs(seat.kbm.mouseDeltaY);
    if (after > before + 0.5f) {
        seat.lastActiveDevice = InputDeviceKind::KeyboardMouse;
    }
}

void InputContext::InjectMouseScroll(float delta, PlayerSeat targetSeat) noexcept {
    if (!targetSeat.IsValid()) {
        return;
    }
    SeatRuntime& seat = _seats[targetSeat.Index()];
    seat.kbm.ApplyWheel(delta);
    if (std::abs(delta) > 1e-4f) {
        seat.lastActiveDevice = InputDeviceKind::KeyboardMouse;
    }
}

void InputContext::InjectChar(uint32_t codepoint, PlayerSeat targetSeat) noexcept {
    if (!targetSeat.IsValid()) {
        return;
    }
    _seats[targetSeat.Index()].kbm.QueueChar(codepoint);
}

void InputContext::InjectResize(Extent2D extent) noexcept {
    _pendingResize = extent;
}

auto InputContext::ConsumePendingResize() noexcept -> Optional<Extent2D> {
    return std::exchange(_pendingResize, Optional<Extent2D> {});
}

auto InputContext::HasPendingResize() const noexcept -> bool {
    return _pendingResize.has_value();
}

void InputContext::SyncCaptureFlags(bool wantCaptureKeyboard, bool wantCaptureMouse, PlayerSeat seat) noexcept {
    if (!seat.IsValid()) {
        return;
    }
    _seats[seat.Index()].uiCapture = UiCaptureState {.keyboard = wantCaptureKeyboard, .mouse = wantCaptureMouse};
}

auto InputContext::UiCapture(PlayerSeat seat) const noexcept -> UiCaptureState {
    return seat.IsValid() ? _seats[seat.Index()].uiCapture : UiCaptureState {};
}

auto InputContext::SelectAutoSeatForGamepad(GamepadSlot slot) const noexcept -> PlayerSeat {
    if (_routingPolicy == SeatRoutingPolicy::SharePrimarySeat) {
        return (slot.value < kMaxPlayerSeats) ? PlayerSeat {slot.value} : kUnassignedSeat;
    }

    // DedicatedGamepadSeats: prefer seats that do NOT already have KB/M or another connected gamepad.
    for (size_t si = 0; si < kMaxPlayerSeats; ++si) {
        const PlayerSeat s {static_cast<uint8_t>(si)};
        if (_seats[si].receivesKeyboardMouse) {
            continue;
        }
        bool occupied = false;
        for (size_t g = 0; g < kMaxGamepads; ++g) {
            if (GamepadSlot {static_cast<uint8_t>(g)} != slot && _gamepads[g].connected && _gamepads[g].assignedSeat == s) {
                occupied = true;
                break;
            }
        }
        if (!occupied) {
            return s;
        }
    }

    // Fallback: any seat without a connected gamepad.
    for (size_t si = 0; si < kMaxPlayerSeats; ++si) {
        const PlayerSeat s {static_cast<uint8_t>(si)};
        bool occupied = false;
        for (size_t g = 0; g < kMaxGamepads; ++g) {
            if (GamepadSlot {static_cast<uint8_t>(g)} != slot && _gamepads[g].connected && _gamepads[g].assignedSeat == s) {
                occupied = true;
                break;
            }
        }
        if (!occupied) {
            return s;
        }
    }
    return kUnassignedSeat;
}

void InputContext::InjectGamepadConnection(GamepadSlot slot, bool connected, std::string_view name) noexcept {
    if (!slot.IsValid()) {
        return;
    }
    GamepadState& gp = _gamepads[slot.Index()];
    if (gp.connected == connected) {
        if (connected && !name.empty()) {
            gp.name.assign(name);
        }
        return;
    }

    if (connected) {
        gp.connected = true;
        gp.name.assign(name.empty() ? std::string_view("Gamepad") : name);
        gp.ResetTransientState();
        if (gp.assignedSeat == kUnassignedSeat || !gp.assignedSeat.IsValid()) {
            gp.assignedSeat = SelectAutoSeatForGamepad(slot);
        }
        if (gp.assignedSeat.IsValid()) {
            _seats[gp.assignedSeat.Index()].lastActiveDevice = InputDeviceKind::Gamepad;
        }
    } else {
        StopRumble(slot);
        const PlayerSeat prevSeat = gp.assignedSeat;
        gp.connected              = false;
        gp.ResetTransientState();
        gp.assignedSeat = kUnassignedSeat;
        _pendingHotplugEvents.push_back(GamepadConnectionEvent {
            .gamepadSlot  = slot,
            .assignedSeat = prevSeat,
            .connected    = false,
            .name         = gp.name,
        });
        return;
    }

    _pendingHotplugEvents.push_back(GamepadConnectionEvent {
        .gamepadSlot  = slot,
        .assignedSeat = gp.assignedSeat,
        .connected    = true,
        .name         = gp.name,
    });
}

void InputContext::InjectGamepadSample(GamepadSlot slot, const GamepadHardwareSample& sample) noexcept {
    if (!slot.IsValid()) {
        return;
    }
    GamepadState& gp = _gamepads[slot.Index()];
    if (!gp.connected) {
        InjectGamepadConnection(slot, true, "Gamepad");
    }
    gp.ApplySample(sample);
    if (gp.assignedSeat.IsValid() && gp.HasAnyActivity()) {
        _seats[gp.assignedSeat.Index()].lastActiveDevice = InputDeviceKind::Gamepad;
    }
}

void InputContext::InjectGamepadButton(GamepadSlot slot, GamepadButton button, bool down) noexcept {
    if (!slot.IsValid()) {
        return;
    }
    GamepadState& gp = _gamepads[slot.Index()];
    if (!gp.connected) {
        return;
    }
    gp.SetButton(button, down);
    if (down && gp.assignedSeat.IsValid()) {
        _seats[gp.assignedSeat.Index()].lastActiveDevice = InputDeviceKind::Gamepad;
    }
}

void InputContext::InjectGamepadAxis(GamepadSlot slot, GamepadAxis axis, float value) noexcept {
    if (!slot.IsValid()) {
        return;
    }
    GamepadState& gp = _gamepads[slot.Index()];
    if (!gp.connected) {
        return;
    }
    gp.SetAxis(axis, value);
    if (gp.assignedSeat.IsValid() && std::abs(gp.Axis(axis)) > 0.05f) {
        _seats[gp.assignedSeat.Index()].lastActiveDevice = InputDeviceKind::Gamepad;
    }
}

void InputContext::FlushPendingHotplugEvents(ECS::EventBus* eventBus) {
    if (eventBus != nullptr && !_pendingHotplugEvents.empty()) {
        for (const GamepadConnectionEvent& ev : _pendingHotplugEvents) {
            eventBus->Push<GamepadConnectionEvent>(ev);
        }
    }
    _pendingHotplugEvents.clear();
}

void InputContext::UpdateCaptureSession() noexcept {
    if (_capture.status != BindingCaptureStatus::Waiting) {
        return;
    }
    if (!_capture.seat.IsValid()) {
        _capture.status = BindingCaptureStatus::Cancelled;
        return;
    }

    const SeatRuntime& seat = _seats[_capture.seat.Index()];
    if (seat.receivesKeyboardMouse) {
        const auto& kbm = seat.kbm;
        if (_capture.cancelKey != KeyCode::Unknown && kbm.IsKeyPressed(_capture.cancelKey)) {
            _capture.status     = BindingCaptureStatus::Cancelled;
            _capture.latchedMod = KeyCode::Unknown;
            return;
        }

        // Check non-modifier keys and mouse buttons first so e.g. Holding Ctrl + pressing R captures Key(R, Ctrl).
        for (size_t k = 1; k < kKeyCodeCount; ++k) {
            const auto kc = static_cast<KeyCode>(k);
            if (kc == _capture.cancelKey) {
                continue;
            }
            if (IsModifierKeyCode(kc)) {
                if (kbm.IsKeyPressed(kc)) {
                    _capture.latchedMod = kc;
                }
                continue;
            }
            if (const auto mb = MouseButtonFromKeyCode(kc); mb.has_value()) {
                if (kbm.IsMouseButtonPressed(*mb)) {
                    _capture.result     = InputBinding::Mouse(*mb, kbm.Modifiers());
                    _capture.status     = BindingCaptureStatus::Captured;
                    _capture.latchedMod = KeyCode::Unknown;
                    return;
                }
            } else if (kbm.IsKeyPressed(kc)) {
                _capture.result     = InputBinding::Key(kc, kbm.Modifiers());
                _capture.status     = BindingCaptureStatus::Captured;
                _capture.latchedMod = KeyCode::Unknown;
                return;
            }
        }

        // Standalone modifier key bound on clean release without any chorded key.
        if (_capture.latchedMod != KeyCode::Unknown && kbm.IsKeyReleased(_capture.latchedMod)) {
            _capture.result     = InputBinding::Key(_capture.latchedMod);
            _capture.status     = BindingCaptureStatus::Captured;
            _capture.latchedMod = KeyCode::Unknown;
            return;
        }
    }

    for (size_t slot = 0; slot < kMaxGamepads; ++slot) {
        const GamepadState& gp = _gamepads[slot];
        if (!gp.connected || gp.assignedSeat != _capture.seat) {
            continue;
        }
        // Check 2-button gamepad chord if one button is held while a second is pressed.
        for (size_t b = 0; b < kGamepadButtonCount; ++b) {
            const auto pressedBtn = static_cast<GamepadButton>(b);
            if (!gp.IsButtonPressed(pressedBtn)) {
                continue;
            }
            for (size_t heldIdx = 0; heldIdx < kGamepadButtonCount; ++heldIdx) {
                if (heldIdx == b) {
                    continue;
                }
                const auto heldBtn = static_cast<GamepadButton>(heldIdx);
                if (gp.IsButtonHeld(heldBtn)) {
                    _capture.result = InputBinding::GamepadChord(heldBtn, pressedBtn);
                    _capture.status = BindingCaptureStatus::Captured;
                    return;
                }
            }
            _capture.result = InputBinding::GamepadBtn(pressedBtn);
            _capture.status = BindingCaptureStatus::Captured;
            return;
        }

        if (_capture.captureAxes) {
            for (size_t a = 0; a < kGamepadAxisCount; ++a) {
                const auto  axis = static_cast<GamepadAxis>(a);
                const float val  = gp.Axis(axis);
                if (std::abs(val) >= 0.65f) {
                    _capture.result = InputBinding::GamepadAxis1D(axis, val < 0.0f ? -1.0f : 1.0f);
                    _capture.status = BindingCaptureStatus::Captured;
                    return;
                }
            }
        }
    }
}

void InputContext::RefreshActionsForCurrentFrame(ECS::EventBus* eventBus) {
    FlushPendingHotplugEvents(eventBus);
    UpdateCaptureSession();

    // Sort indices into `_actionMaps` by descending priority (preserving registration order on ties).
    std::vector<size_t> mapOrder(_actionMaps.size());
    std::iota(mapOrder.begin(), mapOrder.end(), size_t {0});
    std::stable_sort(mapOrder.begin(), mapOrder.end(), [&](size_t a, size_t b) noexcept {
        return _actionMaps[a].Priority() > _actionMaps[b].Priority();
    });

    for (size_t seatIdxRaw = 0; seatIdxRaw < kMaxPlayerSeats; ++seatIdxRaw) {
        const PlayerSeat seatIdx {static_cast<uint8_t>(seatIdxRaw)};
        SeatRuntime&     seat = _seats[seatIdxRaw];

        const InputState* kbm = seat.receivesKeyboardMouse ? &seat.kbm : nullptr;

        std::array<const GamepadState*, kMaxGamepads> seatPadBuf {};
        size_t                                        seatPadCount = 0;
        for (size_t g = 0; g < kMaxGamepads; ++g) {
            if (_gamepads[g].connected && _gamepads[g].assignedSeat == seatIdx) {
                seatPadBuf[seatPadCount++] = &_gamepads[g];
            }
        }
        const std::span<const GamepadState* const> seatPads(seatPadBuf.data(), seatPadCount);

        // Reset current frame values before re-evaluating against `entry.previous` (which holds Frame N-1).
        for (EvaluatedActionEntry& entry : seat.actions) {
            entry.current = ActionState {.activeDevice = seat.lastActiveDevice};
        }

        const auto findOrCreateEntry = [&](ActionID id) -> EvaluatedActionEntry& {
            for (EvaluatedActionEntry& e : seat.actions) {
                if (e.id == id) {
                    return e;
                }
            }
            seat.actions.push_back(EvaluatedActionEntry {.id = id});
            return seat.actions.back();
        };

        SeatConsumptionMask consumed {};
        bool                blockedByModalMap = false;

        for (size_t mapIdx : mapOrder) {
            const InputActionMap& map = _actionMaps[mapIdx];
            if (!map.EnabledForSeat(seatIdx)) {
                continue;
            }

            if (!blockedByModalMap) {
                const auto actions = map.Actions();

                // Evaluate higher-specificity actions (e.g., Ctrl+S or LB+RB chords) before plain
                // single-key actions within the same map so chords cleanly preempt overlapping keys.
                std::vector<size_t> actionOrder(actions.size());
                std::iota(actionOrder.begin(), actionOrder.end(), size_t {0});
                std::stable_sort(actionOrder.begin(), actionOrder.end(), [&](size_t lhs, size_t rhs) noexcept {
                    uint32_t maxL = 0;
                    for (const InputBinding& b : actions[lhs].Bindings()) {
                        maxL = std::max(maxL, b.Specificity());
                    }
                    uint32_t maxR = 0;
                    for (const InputBinding& b : actions[rhs].Bindings()) {
                        maxR = std::max(maxR, b.Specificity());
                    }
                    return maxL > maxR;
                });

                for (size_t actIdx : actionOrder) {
                    const InputAction&    action = actions[actIdx];
                    EvaluatedActionEntry& entry  = findOrCreateEntry(action.Id());

                    float           accumX      = 0.0f;
                    float           accumY      = 0.0f;
                    bool            anyActive   = false;
                    InputDeviceKind winningDev  = seat.lastActiveDevice;

                    for (const InputBinding& binding : action.Bindings()) {
                        const BindingEvalSample sample = EvaluateSingleBinding(
                            binding, action.Kind(), action.ActuationThreshold(), kbm, seat.uiCapture, seatPads, consumed
                        );
                        if (!sample.active && std::abs(sample.x) <= 1e-6f && std::abs(sample.y) <= 1e-6f) {
                            continue;
                        }

                        if (action.Kind() == ActionKind::Digital) {
                            if (sample.active) {
                                anyActive  = true;
                                accumX     = std::abs(sample.x) > std::abs(accumX) ? sample.x : (accumX == 0.0f ? 1.0f : accumX);
                                winningDev = sample.device;
                                if (map.ConsumePhysicalInputs()) {
                                    MarkBindingConsumed(binding, kbm, seat.uiCapture, seatPads, consumed);
                                }
                            }
                        } else if (action.Kind() == ActionKind::Axis1D) {
                            accumX += sample.x;
                            anyActive  = anyActive || sample.active || std::abs(sample.x) >= action.ActuationThreshold();
                            winningDev = sample.device;
                            if (map.ConsumePhysicalInputs() && std::abs(sample.x) > 1e-5f) {
                                MarkBindingConsumed(binding, kbm, seat.uiCapture, seatPads, consumed);
                            }
                        } else {
                            accumX += sample.x;
                            accumY += sample.y;
                            anyActive  = true;
                            winningDev = sample.device;
                            if (map.ConsumePhysicalInputs() && (std::abs(sample.x) > 1e-5f || std::abs(sample.y) > 1e-5f)) {
                                MarkBindingConsumed(binding, kbm, seat.uiCapture, seatPads, consumed);
                            }
                        }
                    }

                    if (action.Kind() == ActionKind::Axis1D && action.ClampUnitCircle()) {
                        accumX = std::clamp(accumX, -1.0f, 1.0f);
                    } else if (action.Kind() == ActionKind::Axis2D && action.ClampUnitCircle()) {
                        const float mag = std::sqrt((accumX * accumX) + (accumY * accumY));
                        if (mag > 1.0f) {
                            accumX /= mag;
                            accumY /= mag;
                        }
                    }

                    bool isDown = false;
                    if (action.Kind() == ActionKind::Digital) {
                        isDown = anyActive;
                        if (isDown && std::abs(accumX) <= 1e-6f) {
                            accumX = 1.0f;
                        }
                    } else if (action.Kind() == ActionKind::Axis1D) {
                        isDown = std::abs(accumX) >= action.ActuationThreshold();
                    } else {
                        const float mag = std::sqrt((accumX * accumX) + (accumY * accumY));
                        isDown          = mag >= action.ActuationThreshold();
                    }

                    // Merge across multiple enabled maps sharing the same ActionID cleanly (OR digital, sum/clamp axes).
                    if (entry.current.down || std::abs(entry.current.x) > 1e-6f || std::abs(entry.current.y) > 1e-6f) {
                        entry.current.x += accumX;
                        entry.current.y += accumY;
                        if (action.ClampUnitCircle()) {
                            if (action.Kind() == ActionKind::Axis1D) {
                                entry.current.x = std::clamp(entry.current.x, -1.0f, 1.0f);
                            } else if (action.Kind() == ActionKind::Axis2D) {
                                const float mag = std::sqrt((entry.current.x * entry.current.x) + (entry.current.y * entry.current.y));
                                if (mag > 1.0f) {
                                    entry.current.x /= mag;
                                    entry.current.y /= mag;
                                }
                            }
                        }
                        entry.current.down = entry.current.down || isDown;
                    } else {
                        entry.current.x    = accumX;
                        entry.current.y    = accumY;
                        entry.current.down = isDown;
                    }

                    if (anyActive || isDown) {
                        entry.current.activeDevice = winningDev;
                    }
                }

                if (map.BlocksLowerPriority()) {
                    blockedByModalMap = true;
                }
            }
        }

        // Finalize digital edge states (`pressed`, `held`, `released`) relative to `entry.previous` (Frame N-1).
        for (EvaluatedActionEntry& entry : seat.actions) {
            const bool wasDown     = entry.previous.down;
            const bool nowDown     = entry.current.down;
            entry.current.pressed  = nowDown && !wasDown;
            entry.current.held     = nowDown && wasDown;
            entry.current.released = !nowDown && wasDown;
        }
    }
}

void InputContext::StepHaptics(float dt) noexcept {
    if (!(dt > 0.0f)) {
        return;
    }
    for (size_t slotIdx = 0; slotIdx < kMaxGamepads; ++slotIdx) {
        const GamepadSlot slot {static_cast<uint8_t>(slotIdx)};
        RumbleChannel&    ch   = _rumble[slotIdx];
        if (!ch.active) {
            continue;
        }
        ch.remainingSeconds -= dt;
        if (ch.remainingSeconds <= 0.0f) {
            StopRumble(slot);
        }
    }
}

void InputContext::EvaluateFrame(float dt, ECS::EventBus* eventBus) {
    RefreshActionsForCurrentFrame(eventBus);
    StepHaptics(dt);
}

// --- Multi-Seat & Device Routing ---------------------------------------------

void InputContext::SetRoutingPolicy(SeatRoutingPolicy policy) noexcept {
    _routingPolicy = policy;
}

void InputContext::SetKeyboardMouseSeat(PlayerSeat seat, bool enabled) noexcept {
    if (!seat.IsValid()) {
        return;
    }
    _seats[seat.Index()].receivesKeyboardMouse = enabled;
}

auto InputContext::SeatReceivesKeyboardMouse(PlayerSeat seat) const noexcept -> bool {
    return seat.IsValid() && _seats[seat.Index()].receivesKeyboardMouse;
}

auto InputContext::SeatKeyboardMouse(PlayerSeat seat) const noexcept -> const InputState& {
    return seat.IsValid() ? _seats[seat.Index()].kbm : _nullKbm;
}

auto InputContext::MutableSeatKeyboardMouse(PlayerSeat seat) noexcept -> InputState& {
    return seat.IsValid() ? _seats[seat.Index()].kbm : _nullKbm;
}

auto InputContext::AssignGamepadToSeat(GamepadSlot gamepadSlot, PlayerSeat seat) noexcept -> bool {
    if (!gamepadSlot.IsValid() || (!seat.IsValid() && seat != kUnassignedSeat)) {
        return false;
    }
    _gamepads[gamepadSlot.Index()].assignedSeat = seat;
    return true;
}

void InputContext::UnassignGamepad(GamepadSlot gamepadSlot) noexcept {
    if (gamepadSlot.IsValid()) {
        _gamepads[gamepadSlot.Index()].assignedSeat = kUnassignedSeat;
    }
}

auto InputContext::GamepadSeat(GamepadSlot gamepadSlot) const noexcept -> PlayerSeat {
    return gamepadSlot.IsValid() ? _gamepads[gamepadSlot.Index()].assignedSeat : kUnassignedSeat;
}

auto InputContext::Gamepad(GamepadSlot gamepadSlot) const noexcept -> Optional<const GamepadState&> {
    if (!gamepadSlot.IsValid() || !_gamepads[gamepadSlot.Index()].connected) {
        return {};
    }
    return _gamepads[gamepadSlot.Index()];
}

auto InputContext::MutableGamepad(GamepadSlot gamepadSlot) noexcept -> Optional<GamepadState&> {
    if (!gamepadSlot.IsValid()) {
        return {};
    }
    return _gamepads[gamepadSlot.Index()];
}

auto InputContext::PrimaryGamepadForSeat(PlayerSeat seat) const noexcept -> Optional<const GamepadState&> {
    if (!seat.IsValid()) {
        return {};
    }
    for (size_t g = 0; g < kMaxGamepads; ++g) {
        if (_gamepads[g].connected && _gamepads[g].assignedSeat == seat) {
            return _gamepads[g];
        }
    }
    return {};
}

auto InputContext::ConnectedGamepadCount() const noexcept -> size_t {
    size_t count = 0;
    for (const GamepadState& gp : _gamepads) {
        if (gp.connected) {
            ++count;
        }
    }
    return count;
}

auto InputContext::LastActiveDevice(PlayerSeat seat) const noexcept -> InputDeviceKind {
    return seat.IsValid() ? _seats[seat.Index()].lastActiveDevice : InputDeviceKind::KeyboardMouse;
}

void InputContext::SetGamepadDeadzones(
    GamepadSlot           gamepadSlot,
    const DeadzoneConfig& leftStick,
    const DeadzoneConfig& rightStick,
    float                 triggerDz
) noexcept {
    if (!gamepadSlot.IsValid()) {
        return;
    }
    _gamepads[gamepadSlot.Index()].leftStickDeadzone  = leftStick;
    _gamepads[gamepadSlot.Index()].rightStickDeadzone = rightStick;
    _gamepads[gamepadSlot.Index()].triggerDeadzone    = std::clamp(triggerDz, 0.0f, 0.95f);
}

void InputContext::SetSeatDeadzones(
    PlayerSeat            seat,
    const DeadzoneConfig& leftStick,
    const DeadzoneConfig& rightStick,
    float                 triggerDz
) noexcept {
    for (size_t slotIdx = 0; slotIdx < kMaxGamepads; ++slotIdx) {
        if (_gamepads[slotIdx].assignedSeat == seat) {
            SetGamepadDeadzones(GamepadSlot {static_cast<uint8_t>(slotIdx)}, leftStick, rightStick, triggerDz);
        }
    }
}

// --- Haptics / Rumble --------------------------------------------------------

void InputContext::SetRumbleSink(GamepadRumbleSinkFn sink, void* userdata) noexcept {
    _rumbleSink         = sink;
    _rumbleSinkUserdata = userdata;
}

auto InputContext::SetGamepadRumble(GamepadSlot gamepadSlot, const GamepadRumble& rumble) noexcept -> bool {
    if (!gamepadSlot.IsValid() || !_gamepads[gamepadSlot.Index()].connected) {
        return false;
    }
    GamepadRumble sanitized {
        .lowFrequency    = std::clamp(rumble.lowFrequency, 0.0f, 1.0f),
        .highFrequency   = std::clamp(rumble.highFrequency, 0.0f, 1.0f),
        .leftTrigger     = std::clamp(rumble.leftTrigger, 0.0f, 1.0f),
        .rightTrigger    = std::clamp(rumble.rightTrigger, 0.0f, 1.0f),
        .durationSeconds = std::max(0.0f, rumble.durationSeconds),
    };
    if (sanitized.IsZero() || sanitized.durationSeconds <= 0.0f) {
        StopRumble(gamepadSlot);
        return true;
    }

    _rumble[gamepadSlot.Index()] = RumbleChannel {
        .current          = sanitized,
        .remainingSeconds = sanitized.durationSeconds,
        .active           = true,
    };
    if (_rumbleSink != nullptr) {
        _rumbleSink(_rumbleSinkUserdata, gamepadSlot, sanitized);
    }
    return true;
}

auto InputContext::RumbleSeat(PlayerSeat seat, const GamepadRumble& rumble) noexcept -> uint32_t {
    if (!seat.IsValid()) {
        return 0;
    }
    uint32_t count = 0;
    for (size_t slotIdx = 0; slotIdx < kMaxGamepads; ++slotIdx) {
        if (_gamepads[slotIdx].connected && _gamepads[slotIdx].assignedSeat == seat) {
            if (SetGamepadRumble(GamepadSlot {static_cast<uint8_t>(slotIdx)}, rumble)) {
                ++count;
            }
        }
    }
    return count;
}

void InputContext::StopRumble(GamepadSlot gamepadSlot) noexcept {
    if (!gamepadSlot.IsValid()) {
        return;
    }
    const bool wasActive = _rumble[gamepadSlot.Index()].active;
    _rumble[gamepadSlot.Index()] = RumbleChannel {};
    if (wasActive && _rumbleSink != nullptr) {
        _rumbleSink(_rumbleSinkUserdata, gamepadSlot, GamepadRumble {});
    }
}

void InputContext::StopAllRumble() noexcept {
    for (size_t slotIdx = 0; slotIdx < kMaxGamepads; ++slotIdx) {
        StopRumble(GamepadSlot {static_cast<uint8_t>(slotIdx)});
    }
}

auto InputContext::ActiveRumble(GamepadSlot gamepadSlot) const noexcept -> GamepadRumble {
    if (!gamepadSlot.IsValid() || !_rumble[gamepadSlot.Index()].active) {
        return GamepadRumble {};
    }
    return _rumble[gamepadSlot.Index()].current;
}

// --- Action Maps & Queries ---------------------------------------------------

auto InputContext::CreateActionMap(std::string_view name, int32_t priority) -> InputActionMap& {
    const uint64_t id = Hash64(name);
    for (InputActionMap& existing : _actionMaps) {
        if (existing.Id() == id) {
            existing.SetPriority(priority);
            return existing;
        }
    }
    _actionMaps.emplace_back(name, priority);
    return _actionMaps.back();
}

auto InputContext::RemoveActionMap(std::string_view name) -> bool {
    const uint64_t id = Hash64(name);
    for (auto it = _actionMaps.begin(); it != _actionMaps.end(); ++it) {
        if (it->Id() == id) {
            _actionMaps.erase(it);
            return true;
        }
    }
    return false;
}

auto InputContext::FindActionMap(std::string_view name) noexcept -> Optional<InputActionMap&> {
    const uint64_t id = Hash64(name);
    for (InputActionMap& map : _actionMaps) {
        if (map.Id() == id) {
            return map;
        }
    }
    return {};
}

auto InputContext::FindActionMap(std::string_view name) const noexcept -> Optional<const InputActionMap&> {
    const uint64_t id = Hash64(name);
    for (const InputActionMap& map : _actionMaps) {
        if (map.Id() == id) {
            return map;
        }
    }
    return {};
}

auto InputContext::SetActionMapEnabled(std::string_view name, bool enabled) noexcept -> bool {
    if (auto map = FindActionMap(name)) {
        map->SetEnabled(enabled);
        return true;
    }
    return false;
}

auto InputContext::SetActionMapEnabledForSeat(std::string_view name, PlayerSeat seat, bool enabled) noexcept -> bool {
    if (auto map = FindActionMap(name)) {
        map->SetEnabledForSeat(seat, enabled);
        return true;
    }
    return false;
}

auto InputContext::Action(ActionID id, PlayerSeat seat) const noexcept -> ActionState {
    if (!seat.IsValid() || !id.Valid()) {
        return {};
    }
    for (const EvaluatedActionEntry& entry : _seats[seat.Index()].actions) {
        if (entry.id == id) {
            return entry.current;
        }
    }
    return ActionState {.activeDevice = _seats[seat.Index()].lastActiveDevice};
}

// --- Runtime Rebinding, Capture & Serialization ------------------------------

auto InputContext::Rebind(std::string_view mapName, std::string_view actionName, size_t slotIndex, const InputBinding& binding) -> bool {
    if (auto map = FindActionMap(mapName)) {
        if (auto action = map->FindAction(actionName)) {
            return action->Rebind(slotIndex, binding);
        }
    }
    return false;
}

auto InputContext::AddBinding(std::string_view mapName, std::string_view actionName, const InputBinding& binding) -> bool {
    if (auto map = FindActionMap(mapName)) {
        if (auto action = map->FindAction(actionName)) {
            action->AppendBinding(binding);
            return true;
        }
    }
    return false;
}

auto InputContext::ClearBindings(std::string_view mapName, std::string_view actionName) -> bool {
    if (auto map = FindActionMap(mapName)) {
        if (auto action = map->FindAction(actionName)) {
            action->ClearBindings();
            return true;
        }
    }
    return false;
}

void InputContext::SnapshotAllDefaults() {
    for (InputActionMap& map : _actionMaps) {
        map.SnapshotDefaults();
    }
}

auto InputContext::ResetActionMapToDefaults(std::string_view mapName) -> bool {
    if (auto map = FindActionMap(mapName)) {
        map->ResetToDefaults();
        return true;
    }
    return false;
}

void InputContext::ResetAllToDefaults() {
    for (InputActionMap& map : _actionMaps) {
        map.ResetToDefaults();
    }
}

auto InputContext::FindConflictingActions(std::string_view mapName, const InputBinding& candidate, std::string_view ignoreAction) const
    -> std::vector<ActionID> {
    std::vector<ActionID> conflicts;
    const auto            map = FindActionMap(mapName);
    if (!map) {
        return conflicts;
    }
    const ActionID ignoredID(ignoreAction);
    for (const InputAction& action : map->Actions()) {
        if (ignoredID.Valid() && action.Id() == ignoredID) {
            continue;
        }
        for (const InputBinding& existing : action.Bindings()) {
            if (BindingsOverlapPhysically(existing, candidate)) {
                conflicts.push_back(action.Id());
                break;
            }
        }
    }
    return conflicts;
}

void InputContext::BeginBindingCapture(PlayerSeat seat, bool captureAxes, KeyCode cancelKey) noexcept {
    _capture = CaptureSession {
        .status      = BindingCaptureStatus::Waiting,
        .seat        = seat,
        .captureAxes = captureAxes,
        .cancelKey   = cancelKey,
        .latchedMod  = KeyCode::Unknown,
        .result      = {},
    };
}

void InputContext::CancelBindingCapture() noexcept {
    _capture.status     = BindingCaptureStatus::Cancelled;
    _capture.latchedMod = KeyCode::Unknown;
    _capture.result.reset();
}

auto InputContext::TakeCapturedBinding() noexcept -> Optional<InputBinding> {
    if (_capture.status != BindingCaptureStatus::Captured || !_capture.result.has_value()) {
        return {};
    }
    Optional<InputBinding> out = _capture.result;
    _capture                   = CaptureSession {};
    return out;
}

auto InputContext::SerializeBindings() const -> std::string {
    std::string out = "ZHLN_INPUT_BINDINGS_V1\n";
    for (const InputActionMap& map : _actionMaps) {
        std::format_to(
            std::back_inserter(out),
            "map\t{}\t{}\t{}\t{}\t{}\n",
            map.Name(),
            map.Priority(),
            map.Enabled() ? 1 : 0,
            map.BlocksLowerPriority() ? 1 : 0,
            map.ConsumePhysicalInputs() ? 1 : 0
        );
        for (const InputAction& action : map.Actions()) {
            std::format_to(
                std::back_inserter(out),
                "action\t{}\t{}\t{}\t{}\t{:.6g}\n",
                map.Name(),
                action.Name(),
                Reflect::EnumToString(action.Kind()),
                action.ClampUnitCircle() ? 1 : 0,
                action.ActuationThreshold()
            );
            for (const InputBinding& b : action.Bindings()) {
                std::format_to(
                    std::back_inserter(out),
                    "bind\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:.6g}\t{:.6g}\t{:.6g}\n",
                    map.Name(),
                    action.Name(),
                    Reflect::EnumToString(b.kind),
                    FormatBindingSlotToken(b, 0),
                    FormatBindingSlotToken(b, 1),
                    FormatBindingSlotToken(b, 2),
                    FormatBindingSlotToken(b, 3),
                    FormatModifiers(b.modifiers),
                    b.strictModifiers ? 1 : 0,
                    b.bypassUiCapture ? 1 : 0,
                    b.scaleX,
                    b.scaleY,
                    b.threshold
                );
            }
        }
    }
    return out;
}

auto InputContext::DeserializeBindings(std::string_view text) -> std::expected<void, ErrorCode> {
    size_t pos = 0;

    const auto nextLine = [&]() noexcept -> Optional<std::string_view> {
        while (pos < text.size()) {
            const size_t           nl   = text.find('\n', pos);
            const std::string_view raw  = (nl == std::string_view::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
            pos                         = (nl == std::string_view::npos) ? text.size() : (nl + 1);
            const std::string_view line = TrimWhitespace(raw);
            if (line.empty() || line.starts_with('#')) {
                continue;
            }
            return line;
        }
        return Optional<std::string_view> {};
    };

    const auto splitTabs = [](std::string_view line, std::vector<std::string_view>& fields) {
        fields.clear();
        size_t start = 0;
        while (true) {
            const size_t tab = line.find('\t', start);
            if (tab == std::string_view::npos) {
                fields.push_back(TrimWhitespace(line.substr(start)));
                break;
            }
            fields.push_back(TrimWhitespace(line.substr(start, tab - start)));
            start = tab + 1;
        }
    };

    auto header = nextLine();
    if (!header || *header != "ZHLN_INPUT_BINDINGS_V1") {
        return std::unexpected(InputBindingParseError::InvalidHeader);
    }

    std::vector<InputActionMap> parsedMaps;
    const auto                  findOrMakeMap = [&](std::string_view mapName) -> InputActionMap& {
        const uint64_t id = Hash64(mapName);
        for (InputActionMap& m : parsedMaps) {
            if (m.Id() == id) {
                return m;
            }
        }
        parsedMaps.emplace_back(mapName, 0);
        return parsedMaps.back();
    };

    std::vector<std::string_view> cols;
    while (auto line = nextLine()) {
        splitTabs(*line, cols);
        if (cols.empty()) {
            continue;
        }

        if (cols[0] == "map") {
            if (cols.size() != 6 || cols[1].empty()) {
                return std::unexpected(InputBindingParseError::MalformedLine);
            }
            int32_t prio = 0;
            const auto [pPtr, pEc] = std::from_chars(cols[2].data(), cols[2].data() + cols[2].size(), prio);
            if (pEc != std::errc {} || pPtr != cols[2].data() + cols[2].size()) {
                return std::unexpected(InputBindingParseError::InvalidNumericValue);
            }
            InputActionMap& m = findOrMakeMap(cols[1]);
            m.SetPriority(prio);
            m.SetEnabled(cols[3] != "0");
            m.SetBlocksLowerPriority(cols[4] != "0");
            m.SetConsumePhysicalInputs(cols[5] != "0");
        } else if (cols[0] == "action") {
            if (cols.size() != 6 || cols[1].empty() || cols[2].empty()) {
                return std::unexpected(InputBindingParseError::MalformedLine);
            }
            const auto kind = Reflect::StringToEnum<ActionKind>(cols[3]);
            if (!kind) {
                return std::unexpected(InputBindingParseError::UnknownEnumerator);
            }
            const auto thresh = ParseFloatToken(cols[5]);
            if (!thresh) {
                return std::unexpected(InputBindingParseError::InvalidNumericValue);
            }
            InputActionMap& m   = findOrMakeMap(cols[1]);
            InputAction&    act = m.AddAction(cols[2], *kind);
            act.ClearBindings();
            act.SetClampUnitCircle(cols[4] != "0");
            act.SetActuationThreshold(*thresh);
        } else if (cols[0] == "bind") {
            if (cols.size() != 14 || cols[1].empty() || cols[2].empty()) {
                return std::unexpected(InputBindingParseError::MalformedLine);
            }
            const auto bKind = Reflect::StringToEnum<InputBindingKind>(cols[3]);
            if (!bKind) {
                return std::unexpected(InputBindingParseError::UnknownBindingKind);
            }
            const auto mods = ParseModifiers(cols[8]);
            const auto sx   = ParseFloatToken(cols[11]);
            const auto sy   = ParseFloatToken(cols[12]);
            const auto th   = ParseFloatToken(cols[13]);

            InputBinding binding {
                .kind            = *bKind,
                .modifiers       = mods.value_or(KeyModifiers::None),
                .strictModifiers = (cols[9] != "0"),
                .bypassUiCapture     = (cols[10] != "0"),
            };
            const std::array<std::string_view, 4> slotTokens {cols[4], cols[5], cols[6], cols[7]};
            if (!mods || !ParseBindingCodeTokens(binding, slotTokens)) {
                return std::unexpected(InputBindingParseError::UnknownEnumerator);
            }
            if (!sx || !sy || !th) {
                return std::unexpected(InputBindingParseError::InvalidNumericValue);
            }
            binding.scaleX    = *sx;
            binding.scaleY    = *sy;
            binding.threshold = *th;

            InputActionMap& m   = findOrMakeMap(cols[1]);
            auto            act = m.FindAction(cols[2]);
            if (!act) {
                act = m.AddAction(cols[2], ActionKind::Digital);
            }

            std::vector<InputBinding> current(act->Bindings().begin(), act->Bindings().end());
            current.push_back(binding);
            act->ReplaceBindings(current);
        } else {
            return std::unexpected(InputBindingParseError::MalformedLine);
        }
    }

    // Apply parsed maps onto existing maps (preserving default snapshots if already authored) or add new ones.
    for (InputActionMap& parsed : parsedMaps) {
        if (auto existing = FindActionMap(parsed.Name())) {
            existing->SetPriority(parsed.Priority());
            existing->SetEnabled(parsed.Enabled());
            existing->SetBlocksLowerPriority(parsed.BlocksLowerPriority());
            existing->SetConsumePhysicalInputs(parsed.ConsumePhysicalInputs());
            for (const InputAction& parsedAct : parsed.Actions()) {
                InputAction& target = existing->AddAction(parsedAct.Name(), parsedAct.Kind());
                target.SetClampUnitCircle(parsedAct.ClampUnitCircle());
                target.SetActuationThreshold(parsedAct.ActuationThreshold());
                target.ReplaceBindings(parsedAct.Bindings());
            }
        } else {
            _actionMaps.push_back(std::move(parsed));
        }
    }

    return {};
}

} // namespace ZHLN
