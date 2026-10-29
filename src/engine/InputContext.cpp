// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/InputContext.hpp>
#include <Zahlen/ecs/EventBus.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numeric>

namespace ZHLN {

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
    std::bitset<kKeyCodeCount>     keys {};
    std::bitset<kGamepadButtonCount> gamepadButtons {};
    uint8_t                        gamepadAxesMask = 0;
    bool                           mouseMotionX    = false;
    bool                           mouseMotionY    = false;
    bool                           mouseWheel      = false;
};

struct BindingEvalSample {
    float           x         = 0.0f;
    float           y         = 0.0f;
    bool            active    = false;
    InputDeviceKind device    = InputDeviceKind::KeyboardMouse;
};

[[nodiscard]] auto CheckBindingModifiers(
    const Components::InputStateComponent& kbm,
    KeyModifiers                           required,
    bool                                   strict,
    bool                                   rawBypass
) noexcept -> bool {
    const KeyModifiers current = rawBypass ? kbm.ModifiersRaw() : kbm.Modifiers();
    if (strict) {
        return current == required;
    }
    return (current & required) == required;
}

[[nodiscard]] auto KeyDownForBinding(const Components::InputStateComponent& kbm, uint8_t key, bool rawBypass) noexcept -> bool {
    if (key == 0 || key >= kKeyCodeCount) {
        return false;
    }
    const auto kc = static_cast<KeyCode>(key);
    if (MouseButtonFromKeyCode(kc).has_value()) {
        return rawBypass ? kbm.IsMouseButtonDownRaw(key) : kbm.IsMouseButtonDown(key);
    }
    return rawBypass ? kbm.IsKeyDownRaw(key) : kbm.IsKeyDown(key);
}

[[nodiscard]] auto KeyPressedEdgeForBinding(const Components::InputStateComponent& kbm, uint8_t key, bool rawBypass) noexcept -> bool {
    if (key == 0 || key >= kKeyCodeCount) {
        return false;
    }
    const auto kc = static_cast<KeyCode>(key);
    if (const auto mb = MouseButtonFromKeyCode(kc); mb.has_value()) {
        return rawBypass ? kbm.IsMouseButtonPressedRaw(*mb) : kbm.IsMouseButtonPressed(*mb);
    }
    return rawBypass ? kbm.IsKeyPressedRaw(key) : kbm.IsKeyPressed(key);
}

[[nodiscard]] auto EvaluateSingleBinding(
    const InputBinding&                    binding,
    ActionKind                             actionKind,
    float                                  actionThreshold,
    const Components::InputStateComponent* kbm,
    std::span<const GamepadState* const>   seatGamepads,
    const SeatConsumptionMask&             consumed
) noexcept -> BindingEvalSample {
    BindingEvalSample out {};

    const auto evalKeyLike = [&](uint8_t keyCode, KeyModifiers mods, bool strictMods, bool rawBypass, float scale) noexcept {
        if (kbm == nullptr || keyCode == 0 || keyCode >= kKeyCodeCount || consumed.keys.test(keyCode)) {
            return;
        }
        if (!CheckBindingModifiers(*kbm, mods, strictMods, rawBypass)) {
            return;
        }
        const bool isDown    = KeyDownForBinding(*kbm, keyCode, rawBypass);
        const bool tapRising = KeyPressedEdgeForBinding(*kbm, keyCode, rawBypass);
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
            evalKeyLike(binding.code0, binding.modifiers, binding.strictModifiers, binding.rawBypassUI, binding.scaleX);
            break;

        case InputBindingKind::MouseButton: {
            const KeyCode kc = ToKeyCode(static_cast<MouseButton>(binding.code0));
            evalKeyLike(static_cast<uint8_t>(kc), binding.modifiers, binding.strictModifiers, binding.rawBypassUI, binding.scaleX);
            break;
        }

        case InputBindingKind::KeyChord: {
            if (kbm == nullptr || binding.code0 == 0 || binding.code1 == 0 || binding.code0 >= kKeyCodeCount ||
                binding.code1 >= kKeyCodeCount) {
                break;
            }
            if (consumed.keys.test(binding.code0) || consumed.keys.test(binding.code1)) {
                break;
            }
            if (!CheckBindingModifiers(*kbm, binding.modifiers, binding.strictModifiers, binding.rawBypassUI)) {
                break;
            }
            const bool d0 = KeyDownForBinding(*kbm, binding.code0, binding.rawBypassUI) ||
                            KeyPressedEdgeForBinding(*kbm, binding.code0, binding.rawBypassUI);
            const bool d1 = KeyDownForBinding(*kbm, binding.code1, binding.rawBypassUI) ||
                            KeyPressedEdgeForBinding(*kbm, binding.code1, binding.rawBypassUI);
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
            const float dx = binding.rawBypassUI ? kbm->mouseDeltaX : kbm->GetMouseDeltaX();
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
            const float dy = binding.rawBypassUI ? kbm->mouseDeltaY : kbm->GetMouseDeltaY();
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
            const float dx = consumed.mouseMotionX ? 0.0f : (binding.rawBypassUI ? kbm->mouseDeltaX : kbm->GetMouseDeltaX());
            const float dy = consumed.mouseMotionY ? 0.0f : (binding.rawBypassUI ? kbm->mouseDeltaY : kbm->GetMouseDeltaY());
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
            const float w = binding.rawBypassUI ? kbm->mouseWheel : kbm->GetMouseWheel();
            if (std::abs(w) > 1e-6f) {
                out.x      = w * binding.scaleX;
                out.active = (actionKind == ActionKind::Digital) ? (std::abs(out.x) >= 0.01f) : true;
                out.device = InputDeviceKind::KeyboardMouse;
            }
            break;
        }

        case InputBindingKind::GamepadButton: {
            if (binding.code0 >= kGamepadButtonCount || consumed.gamepadButtons.test(binding.code0)) {
                break;
            }
            const auto btn = static_cast<GamepadButton>(binding.code0);
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
            if (binding.code0 >= kGamepadButtonCount || binding.code1 >= kGamepadButtonCount) {
                break;
            }
            if (consumed.gamepadButtons.test(binding.code0) || consumed.gamepadButtons.test(binding.code1)) {
                break;
            }
            const auto b0 = static_cast<GamepadButton>(binding.code0);
            const auto b1 = static_cast<GamepadButton>(binding.code1);
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
            if (binding.code0 >= kGamepadAxisCount || ((consumed.gamepadAxesMask & (1U << binding.code0)) != 0U)) {
                break;
            }
            const auto  axis   = static_cast<GamepadAxis>(binding.code0);
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
            if (!consumed.keys.test(binding.code0) && KeyDownForBinding(*kbm, binding.code0, binding.rawBypassUI)) {
                sum -= 1.0f;
            }
            if (!consumed.keys.test(binding.code1) && KeyDownForBinding(*kbm, binding.code1, binding.rawBypassUI)) {
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
            float best = 0.0f;
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                float sum = 0.0f;
                if (binding.code0 < kGamepadButtonCount && !consumed.gamepadButtons.test(binding.code0) &&
                    gp->IsButtonDown(static_cast<GamepadButton>(binding.code0))) {
                    sum -= 1.0f;
                }
                if (binding.code1 < kGamepadButtonCount && !consumed.gamepadButtons.test(binding.code1) &&
                    gp->IsButtonDown(static_cast<GamepadButton>(binding.code1))) {
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
            if (!consumed.keys.test(binding.code0) && KeyDownForBinding(*kbm, binding.code0, binding.rawBypassUI)) {
                y += 1.0f;
            }
            if (!consumed.keys.test(binding.code1) && KeyDownForBinding(*kbm, binding.code1, binding.rawBypassUI)) {
                y -= 1.0f;
            }
            if (!consumed.keys.test(binding.code2) && KeyDownForBinding(*kbm, binding.code2, binding.rawBypassUI)) {
                x -= 1.0f;
            }
            if (!consumed.keys.test(binding.code3) && KeyDownForBinding(*kbm, binding.code3, binding.rawBypassUI)) {
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
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                float x = 0.0f;
                float y = 0.0f;
                if (binding.code0 < kGamepadButtonCount && !consumed.gamepadButtons.test(binding.code0) &&
                    gp->IsButtonDown(static_cast<GamepadButton>(binding.code0))) {
                    y += 1.0f;
                }
                if (binding.code1 < kGamepadButtonCount && !consumed.gamepadButtons.test(binding.code1) &&
                    gp->IsButtonDown(static_cast<GamepadButton>(binding.code1))) {
                    y -= 1.0f;
                }
                if (binding.code2 < kGamepadButtonCount && !consumed.gamepadButtons.test(binding.code2) &&
                    gp->IsButtonDown(static_cast<GamepadButton>(binding.code2))) {
                    x -= 1.0f;
                }
                if (binding.code3 < kGamepadButtonCount && !consumed.gamepadButtons.test(binding.code3) &&
                    gp->IsButtonDown(static_cast<GamepadButton>(binding.code3))) {
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
            if (binding.code0 >= kGamepadAxisCount || binding.code1 >= kGamepadAxisCount) {
                break;
            }
            if ((consumed.gamepadAxesMask & ((1U << binding.code0) | (1U << binding.code1))) != 0U) {
                break;
            }
            float bestLenSq = 0.0f;
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                float sx = 0.0f;
                float sy = 0.0f;
                if (binding.code0 == static_cast<uint8_t>(GamepadAxis::LeftX) && binding.code1 == static_cast<uint8_t>(GamepadAxis::LeftY)) {
                    const StickSample stick = gp->LeftStick();
                    sx                      = stick.x;
                    sy                      = stick.y;
                } else if (binding.code0 == static_cast<uint8_t>(GamepadAxis::RightX) &&
                           binding.code1 == static_cast<uint8_t>(GamepadAxis::RightY)) {
                    const StickSample stick = gp->RightStick();
                    sx                      = stick.x;
                    sy                      = stick.y;
                } else {
                    sx = gp->Axis(static_cast<GamepadAxis>(binding.code0));
                    sy = gp->Axis(static_cast<GamepadAxis>(binding.code1));
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
    const InputBinding&                    binding,
    const Components::InputStateComponent* kbm,
    std::span<const GamepadState* const>   seatGamepads,
    SeatConsumptionMask&                   consumed
) noexcept {
    const auto markKey = [&](uint8_t k) noexcept {
        if (k != 0 && k < kKeyCodeCount) {
            consumed.keys.set(k);
        }
    };
    const auto markGpBtn = [&](uint8_t b) noexcept {
        if (b < kGamepadButtonCount) {
            consumed.gamepadButtons.set(b);
        }
    };

    switch (binding.kind) {
        case InputBindingKind::None:
            break;
        case InputBindingKind::Key:
            markKey(binding.code0);
            break;
        case InputBindingKind::MouseButton:
            markKey(static_cast<uint8_t>(ToKeyCode(static_cast<MouseButton>(binding.code0))));
            break;
        case InputBindingKind::KeyChord:
            markKey(binding.code0);
            markKey(binding.code1);
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
            markGpBtn(binding.code0);
            break;
        case InputBindingKind::GamepadChord:
            markGpBtn(binding.code0);
            markGpBtn(binding.code1);
            break;
        case InputBindingKind::GamepadAxis1D:
            if (binding.code0 < kGamepadAxisCount) {
                consumed.gamepadAxesMask = static_cast<uint8_t>(consumed.gamepadAxesMask | (1U << binding.code0));
            }
            break;
        case InputBindingKind::Composite1DKeys:
            if (kbm != nullptr) {
                if (KeyDownForBinding(*kbm, binding.code0, binding.rawBypassUI)) {
                    markKey(binding.code0);
                }
                if (KeyDownForBinding(*kbm, binding.code1, binding.rawBypassUI)) {
                    markKey(binding.code1);
                }
            }
            break;
        case InputBindingKind::Composite1DGamepad:
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                if (binding.code0 < kGamepadButtonCount && gp->IsButtonDown(static_cast<GamepadButton>(binding.code0))) {
                    markGpBtn(binding.code0);
                }
                if (binding.code1 < kGamepadButtonCount && gp->IsButtonDown(static_cast<GamepadButton>(binding.code1))) {
                    markGpBtn(binding.code1);
                }
            }
            break;
        case InputBindingKind::Composite2DKeys:
            if (kbm != nullptr) {
                for (uint8_t c : {binding.code0, binding.code1, binding.code2, binding.code3}) {
                    if (KeyDownForBinding(*kbm, c, binding.rawBypassUI)) {
                        markKey(c);
                    }
                }
            }
            break;
        case InputBindingKind::Composite2DGamepad:
            for (const GamepadState* gp : seatGamepads) {
                if (gp == nullptr) {
                    continue;
                }
                for (uint8_t c : {binding.code0, binding.code1, binding.code2, binding.code3}) {
                    if (c < kGamepadButtonCount && gp->IsButtonDown(static_cast<GamepadButton>(c))) {
                        markGpBtn(c);
                    }
                }
            }
            break;
        case InputBindingKind::GamepadStick2D:
            if (binding.code0 < kGamepadAxisCount && binding.code1 < kGamepadAxisCount) {
                consumed.gamepadAxesMask =
                    static_cast<uint8_t>(consumed.gamepadAxesMask | (1U << binding.code0) | (1U << binding.code1));
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
            return static_cast<uint8_t>(ToKeyCode(static_cast<MouseButton>(a.code0))) == b.code0 && a.modifiers == b.modifiers;
        }
        if (b.kind == InputBindingKind::MouseButton && a.kind == InputBindingKind::Key) {
            return static_cast<uint8_t>(ToKeyCode(static_cast<MouseButton>(b.code0))) == a.code0 && a.modifiers == b.modifiers;
        }
        return false;
    }
    switch (a.kind) {
        case InputBindingKind::Key:
        case InputBindingKind::MouseButton:
            return a.code0 == b.code0 && a.modifiers == b.modifiers;
        case InputBindingKind::KeyChord: {
            const bool samePair = (a.code0 == b.code0 && a.code1 == b.code1) || (a.code0 == b.code1 && a.code1 == b.code0);
            return samePair && a.modifiers == b.modifiers;
        }
        case InputBindingKind::GamepadButton:
        case InputBindingKind::GamepadAxis1D:
            return a.code0 == b.code0;
        case InputBindingKind::GamepadChord:
            return (a.code0 == b.code0 && a.code1 == b.code1) || (a.code0 == b.code1 && a.code1 == b.code0);
        case InputBindingKind::MouseDeltaX:
        case InputBindingKind::MouseDeltaY:
        case InputBindingKind::MouseDelta2D:
        case InputBindingKind::MouseWheel:
            return true;
        case InputBindingKind::Composite1DKeys:
        case InputBindingKind::Composite1DGamepad:
        case InputBindingKind::GamepadStick2D:
            return a.code0 == b.code0 || a.code1 == b.code1 || a.code0 == b.code1 || a.code1 == b.code0;
        case InputBindingKind::Composite2DKeys:
        case InputBindingKind::Composite2DGamepad: {
            const std::array<uint8_t, 4> ca {a.code0, a.code1, a.code2, a.code3};
            const std::array<uint8_t, 4> cb {b.code0, b.code1, b.code2, b.code3};
            for (uint8_t x : ca) {
                for (uint8_t y : cb) {
                    if (x != 0 && x == y) {
                        return true;
                    }
                }
            }
            return false;
        }
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

[[nodiscard]] auto FormatCodeToken(InputBindingKind kind, size_t index, uint8_t raw) -> std::string_view {
    switch (kind) {
        case InputBindingKind::Key:
            return (index == 0) ? Reflect::EnumToString(static_cast<KeyCode>(raw)) : "-";
        case InputBindingKind::KeyChord:
            return (index < 2) ? Reflect::EnumToString(static_cast<KeyCode>(raw)) : "-";
        case InputBindingKind::Composite1DKeys:
            return (index < 2) ? Reflect::EnumToString(static_cast<KeyCode>(raw)) : "-";
        case InputBindingKind::Composite2DKeys:
            return Reflect::EnumToString(static_cast<KeyCode>(raw));
        case InputBindingKind::MouseButton:
            return (index == 0) ? Reflect::EnumToString(static_cast<MouseButton>(raw)) : "-";
        case InputBindingKind::GamepadButton:
            return (index == 0) ? Reflect::EnumToString(static_cast<GamepadButton>(raw)) : "-";
        case InputBindingKind::GamepadChord:
        case InputBindingKind::Composite1DGamepad:
            return (index < 2) ? Reflect::EnumToString(static_cast<GamepadButton>(raw)) : "-";
        case InputBindingKind::Composite2DGamepad:
            return Reflect::EnumToString(static_cast<GamepadButton>(raw));
        case InputBindingKind::GamepadAxis1D:
            return (index == 0) ? Reflect::EnumToString(static_cast<GamepadAxis>(raw)) : "-";
        case InputBindingKind::GamepadStick2D:
            return (index < 2) ? Reflect::EnumToString(static_cast<GamepadAxis>(raw)) : "-";
        default:
            return "-";
    }
}

[[nodiscard]] auto ParseCodeToken(InputBindingKind kind, size_t index, std::string_view token) noexcept -> Optional<uint8_t> {
    if (token == "-") {
        return uint8_t {0};
    }
    switch (kind) {
        case InputBindingKind::Key:
        case InputBindingKind::KeyChord:
        case InputBindingKind::Composite1DKeys:
        case InputBindingKind::Composite2DKeys: {
            if (auto kc = Reflect::StringToEnum<KeyCode>(token)) {
                return static_cast<uint8_t>(*kc);
            }
            return Optional<uint8_t> {};
        }
        case InputBindingKind::MouseButton: {
            if (auto mb = Reflect::StringToEnum<MouseButton>(token)) {
                return static_cast<uint8_t>(*mb);
            }
            return Optional<uint8_t> {};
        }
        case InputBindingKind::GamepadButton:
        case InputBindingKind::GamepadChord:
        case InputBindingKind::Composite1DGamepad:
        case InputBindingKind::Composite2DGamepad: {
            if (auto gb = Reflect::StringToEnum<GamepadButton>(token)) {
                return static_cast<uint8_t>(*gb);
            }
            return Optional<uint8_t> {};
        }
        case InputBindingKind::GamepadAxis1D:
        case InputBindingKind::GamepadStick2D: {
            if (index < 2) {
                if (auto ga = Reflect::StringToEnum<GamepadAxis>(token)) {
                    return static_cast<uint8_t>(*ga);
                }
            }
            return Optional<uint8_t> {};
        }
        default:
            return uint8_t {0};
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
    const ActionId id(name);
    for (InputAction& existing : _actions) {
        if (existing.Id() == id) {
            return existing;
        }
    }
    _actions.emplace_back(name, kind);
    return _actions.back();
}

auto InputActionMap::FindAction(ActionId id) noexcept -> Optional<InputAction&> {
    for (InputAction& action : _actions) {
        if (action.Id() == id) {
            return action;
        }
    }
    return {};
}

auto InputActionMap::FindAction(ActionId id) const noexcept -> Optional<const InputAction&> {
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
        _seats[i].kbm.seat              = static_cast<PlayerSeat>(i);
        _seats[i].receivesKeyboardMouse = (i == kPrimarySeat);
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
    if (targetSeat >= kMaxPlayerSeats || key == KeyCode::Unknown) {
        return;
    }
    SeatRuntime& seat = _seats[targetSeat];
    seat.kbm.SetKey(key, down);
    if (down) {
        seat.kbm.QueueKeyPress(key);
        seat.lastActiveDevice = InputDeviceKind::KeyboardMouse;
    }
}

void InputContext::InjectMouseMove(float x, float y, PlayerSeat targetSeat) noexcept {
    if (targetSeat >= kMaxPlayerSeats) {
        return;
    }
    SeatRuntime& seat   = _seats[targetSeat];
    const float  before = std::abs(seat.kbm.mouseDeltaX) + std::abs(seat.kbm.mouseDeltaY);
    seat.kbm.ApplyLocalMotion(x, y);
    const float after = std::abs(seat.kbm.mouseDeltaX) + std::abs(seat.kbm.mouseDeltaY);
    if (after > before + 0.5f) {
        seat.lastActiveDevice = InputDeviceKind::KeyboardMouse;
    }
}

void InputContext::InjectMouseRelative(float x, float y, float dx, float dy, PlayerSeat targetSeat) noexcept {
    if (targetSeat >= kMaxPlayerSeats) {
        return;
    }
    SeatRuntime& seat   = _seats[targetSeat];
    const float  before = std::abs(seat.kbm.mouseDeltaX) + std::abs(seat.kbm.mouseDeltaY);
    seat.kbm.ApplyRelativeMotion(x, y, dx, dy);
    const float after = std::abs(seat.kbm.mouseDeltaX) + std::abs(seat.kbm.mouseDeltaY);
    if (after > before + 0.5f) {
        seat.lastActiveDevice = InputDeviceKind::KeyboardMouse;
    }
}

void InputContext::InjectMouseScroll(float delta, PlayerSeat targetSeat) noexcept {
    if (targetSeat >= kMaxPlayerSeats) {
        return;
    }
    SeatRuntime& seat = _seats[targetSeat];
    seat.kbm.ApplyWheel(delta);
    if (std::abs(delta) > 1e-4f) {
        seat.lastActiveDevice = InputDeviceKind::KeyboardMouse;
    }
}

void InputContext::InjectChar(uint32_t codepoint, PlayerSeat targetSeat) noexcept {
    if (targetSeat >= kMaxPlayerSeats) {
        return;
    }
    _seats[targetSeat].kbm.QueueChar(codepoint);
}

void InputContext::SyncCaptureFlags(bool wantCaptureKeyboard, bool wantCaptureMouse, PlayerSeat seat) noexcept {
    if (seat >= kMaxPlayerSeats) {
        return;
    }
    _seats[seat].kbm.wantCaptureKeyboard = wantCaptureKeyboard;
    _seats[seat].kbm.wantCaptureMouse    = wantCaptureMouse;
}

void InputContext::SyncFromInputStateComponent(const Components::InputStateComponent& component) noexcept {
    const PlayerSeat seat = (component.seat < kMaxPlayerSeats) ? component.seat : kPrimarySeat;
    _seats[seat].kbm      = component;
    _seats[seat].kbm.seat = seat;
}

auto InputContext::SelectAutoSeatForGamepad(uint8_t slot) const noexcept -> PlayerSeat {
    if (_routingPolicy == SeatRoutingPolicy::SharePrimarySeat) {
        return (slot < kMaxPlayerSeats) ? static_cast<PlayerSeat>(slot) : kUnassignedSeat;
    }

    // DedicatedGamepadSeats: prefer seats that do NOT already have KB/M or another connected gamepad.
    for (PlayerSeat s = 0; s < kMaxPlayerSeats; ++s) {
        if (_seats[s].receivesKeyboardMouse) {
            continue;
        }
        bool occupied = false;
        for (size_t g = 0; g < kMaxGamepads; ++g) {
            if (g != slot && _gamepads[g].connected && _gamepads[g].assignedSeat == s) {
                occupied = true;
                break;
            }
        }
        if (!occupied) {
            return s;
        }
    }

    // Fallback: any seat without a connected gamepad.
    for (PlayerSeat s = 0; s < kMaxPlayerSeats; ++s) {
        bool occupied = false;
        for (size_t g = 0; g < kMaxGamepads; ++g) {
            if (g != slot && _gamepads[g].connected && _gamepads[g].assignedSeat == s) {
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

void InputContext::InjectGamepadConnection(uint8_t slot, bool connected, std::string_view name) noexcept {
    if (slot >= kMaxGamepads) {
        return;
    }
    GamepadState& gp = _gamepads[slot];
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
        if (gp.assignedSeat == kUnassignedSeat || gp.assignedSeat >= kMaxPlayerSeats) {
            gp.assignedSeat = SelectAutoSeatForGamepad(slot);
        }
        if (gp.assignedSeat < kMaxPlayerSeats) {
            _seats[gp.assignedSeat].lastActiveDevice = InputDeviceKind::Gamepad;
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

void InputContext::InjectGamepadSample(uint8_t slot, const GamepadRawSample& sample) noexcept {
    if (slot >= kMaxGamepads) {
        return;
    }
    GamepadState& gp = _gamepads[slot];
    if (!gp.connected) {
        InjectGamepadConnection(slot, true, "Gamepad");
    }
    gp.ApplySample(sample);
    if (gp.assignedSeat < kMaxPlayerSeats && gp.HasAnyActivity()) {
        _seats[gp.assignedSeat].lastActiveDevice = InputDeviceKind::Gamepad;
    }
}

void InputContext::InjectGamepadButton(uint8_t slot, GamepadButton button, bool down) noexcept {
    if (slot >= kMaxGamepads) {
        return;
    }
    GamepadState& gp = _gamepads[slot];
    if (!gp.connected) {
        return;
    }
    gp.SetButton(button, down);
    if (down && gp.assignedSeat < kMaxPlayerSeats) {
        _seats[gp.assignedSeat].lastActiveDevice = InputDeviceKind::Gamepad;
    }
}

void InputContext::InjectGamepadAxis(uint8_t slot, GamepadAxis axis, float value) noexcept {
    if (slot >= kMaxGamepads) {
        return;
    }
    GamepadState& gp = _gamepads[slot];
    if (!gp.connected) {
        return;
    }
    gp.SetAxis(axis, value);
    if (gp.assignedSeat < kMaxPlayerSeats && std::abs(gp.Axis(axis)) > 0.05f) {
        _seats[gp.assignedSeat].lastActiveDevice = InputDeviceKind::Gamepad;
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
    if (_capture.seat >= kMaxPlayerSeats) {
        _capture.status = BindingCaptureStatus::Cancelled;
        return;
    }

    const SeatRuntime& seat = _seats[_capture.seat];
    if (seat.receivesKeyboardMouse) {
        const auto& kbm = seat.kbm;
        if (_capture.cancelKey != KeyCode::Unknown && kbm.IsKeyPressedRaw(_capture.cancelKey)) {
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
                if (kbm.IsKeyPressedRaw(kc)) {
                    _capture.latchedMod = kc;
                }
                continue;
            }
            if (const auto mb = MouseButtonFromKeyCode(kc); mb.has_value()) {
                if (kbm.IsMouseButtonPressedRaw(*mb)) {
                    _capture.result     = InputBinding::Mouse(*mb, kbm.ModifiersRaw());
                    _capture.status     = BindingCaptureStatus::Captured;
                    _capture.latchedMod = KeyCode::Unknown;
                    return;
                }
            } else if (kbm.IsKeyPressedRaw(kc)) {
                _capture.result     = InputBinding::Key(kc, kbm.ModifiersRaw());
                _capture.status     = BindingCaptureStatus::Captured;
                _capture.latchedMod = KeyCode::Unknown;
                return;
            }
        }

        // Standalone modifier key bound on clean release without any chorded key.
        if (_capture.latchedMod != KeyCode::Unknown && kbm.IsKeyReleasedRaw(_capture.latchedMod)) {
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

    for (PlayerSeat seatIdx = 0; seatIdx < kMaxPlayerSeats; ++seatIdx) {
        SeatRuntime& seat = _seats[seatIdx];

        const Components::InputStateComponent* kbm = seat.receivesKeyboardMouse ? &seat.kbm : nullptr;

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

        const auto findOrCreateEntry = [&](ActionId id) -> EvaluatedActionEntry& {
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
                    uint8_t maxL = 0;
                    for (const InputBinding& b : actions[lhs].Bindings()) {
                        maxL = std::max(maxL, b.Specificity());
                    }
                    uint8_t maxR = 0;
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
                            binding, action.Kind(), action.ActuationThreshold(), kbm, seatPads, consumed
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
                                    MarkBindingConsumed(binding, kbm, seatPads, consumed);
                                }
                            }
                        } else if (action.Kind() == ActionKind::Axis1D) {
                            accumX += sample.x;
                            anyActive  = anyActive || sample.active || std::abs(sample.x) >= action.ActuationThreshold();
                            winningDev = sample.device;
                            if (map.ConsumePhysicalInputs() && std::abs(sample.x) > 1e-5f) {
                                MarkBindingConsumed(binding, kbm, seatPads, consumed);
                            }
                        } else {
                            accumX += sample.x;
                            accumY += sample.y;
                            anyActive  = true;
                            winningDev = sample.device;
                            if (map.ConsumePhysicalInputs() && (std::abs(sample.x) > 1e-5f || std::abs(sample.y) > 1e-5f)) {
                                MarkBindingConsumed(binding, kbm, seatPads, consumed);
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

                    // Merge across multiple enabled maps sharing the same ActionId cleanly (OR digital, sum/clamp axes).
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
    for (uint8_t slot = 0; slot < kMaxGamepads; ++slot) {
        RumbleChannel& ch = _rumble[slot];
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
    if (seat >= kMaxPlayerSeats) {
        return;
    }
    _seats[seat].receivesKeyboardMouse = enabled;
}

auto InputContext::SeatReceivesKeyboardMouse(PlayerSeat seat) const noexcept -> bool {
    return seat < kMaxPlayerSeats && _seats[seat].receivesKeyboardMouse;
}

auto InputContext::SeatKeyboardMouse(PlayerSeat seat) const noexcept -> const Components::InputStateComponent& {
    return (seat < kMaxPlayerSeats) ? _seats[seat].kbm : _nullKbm;
}

auto InputContext::MutableSeatKeyboardMouse(PlayerSeat seat) noexcept -> Components::InputStateComponent& {
    return (seat < kMaxPlayerSeats) ? _seats[seat].kbm : _nullKbm;
}

auto InputContext::AssignGamepadToSeat(uint8_t gamepadSlot, PlayerSeat seat) noexcept -> bool {
    if (gamepadSlot >= kMaxGamepads || (seat >= kMaxPlayerSeats && seat != kUnassignedSeat)) {
        return false;
    }
    _gamepads[gamepadSlot].assignedSeat = seat;
    return true;
}

void InputContext::UnassignGamepad(uint8_t gamepadSlot) noexcept {
    if (gamepadSlot < kMaxGamepads) {
        _gamepads[gamepadSlot].assignedSeat = kUnassignedSeat;
    }
}

auto InputContext::GamepadSeat(uint8_t gamepadSlot) const noexcept -> PlayerSeat {
    return (gamepadSlot < kMaxGamepads) ? _gamepads[gamepadSlot].assignedSeat : kUnassignedSeat;
}

auto InputContext::Gamepad(uint8_t gamepadSlot) const noexcept -> Optional<const GamepadState&> {
    if (gamepadSlot >= kMaxGamepads || !_gamepads[gamepadSlot].connected) {
        return {};
    }
    return _gamepads[gamepadSlot];
}

auto InputContext::MutableGamepad(uint8_t gamepadSlot) noexcept -> Optional<GamepadState&> {
    if (gamepadSlot >= kMaxGamepads) {
        return {};
    }
    return _gamepads[gamepadSlot];
}

auto InputContext::PrimaryGamepadForSeat(PlayerSeat seat) const noexcept -> Optional<const GamepadState&> {
    if (seat >= kMaxPlayerSeats) {
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
    return (seat < kMaxPlayerSeats) ? _seats[seat].lastActiveDevice : InputDeviceKind::KeyboardMouse;
}

void InputContext::SetGamepadDeadzones(
    uint8_t               gamepadSlot,
    const DeadzoneConfig& leftStick,
    const DeadzoneConfig& rightStick,
    float                 triggerDz
) noexcept {
    if (gamepadSlot >= kMaxGamepads) {
        return;
    }
    _gamepads[gamepadSlot].leftStickDeadzone  = leftStick;
    _gamepads[gamepadSlot].rightStickDeadzone = rightStick;
    _gamepads[gamepadSlot].triggerDeadzone    = std::clamp(triggerDz, 0.0f, 0.95f);
}

void InputContext::SetSeatDeadzones(
    PlayerSeat            seat,
    const DeadzoneConfig& leftStick,
    const DeadzoneConfig& rightStick,
    float                 triggerDz
) noexcept {
    for (uint8_t slot = 0; slot < kMaxGamepads; ++slot) {
        if (_gamepads[slot].assignedSeat == seat) {
            SetGamepadDeadzones(slot, leftStick, rightStick, triggerDz);
        }
    }
}

// --- Haptics / Rumble --------------------------------------------------------

void InputContext::SetRumbleSink(GamepadRumbleSinkFn sink, void* userdata) noexcept {
    _rumbleSink         = sink;
    _rumbleSinkUserdata = userdata;
}

auto InputContext::SetGamepadRumble(uint8_t gamepadSlot, const GamepadRumble& rumble) noexcept -> bool {
    if (gamepadSlot >= kMaxGamepads || !_gamepads[gamepadSlot].connected) {
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

    _rumble[gamepadSlot] = RumbleChannel {
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
    if (seat >= kMaxPlayerSeats) {
        return 0;
    }
    uint32_t count = 0;
    for (uint8_t slot = 0; slot < kMaxGamepads; ++slot) {
        if (_gamepads[slot].connected && _gamepads[slot].assignedSeat == seat) {
            if (SetGamepadRumble(slot, rumble)) {
                ++count;
            }
        }
    }
    return count;
}

void InputContext::StopRumble(uint8_t gamepadSlot) noexcept {
    if (gamepadSlot >= kMaxGamepads) {
        return;
    }
    const bool wasActive = _rumble[gamepadSlot].active;
    _rumble[gamepadSlot] = RumbleChannel {};
    if (wasActive && _rumbleSink != nullptr) {
        _rumbleSink(_rumbleSinkUserdata, gamepadSlot, GamepadRumble {});
    }
}

void InputContext::StopAllRumble() noexcept {
    for (uint8_t slot = 0; slot < kMaxGamepads; ++slot) {
        StopRumble(slot);
    }
}

auto InputContext::ActiveRumble(uint8_t gamepadSlot) const noexcept -> GamepadRumble {
    if (gamepadSlot >= kMaxGamepads || !_rumble[gamepadSlot].active) {
        return GamepadRumble {};
    }
    return _rumble[gamepadSlot].current;
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

auto InputContext::Action(ActionId id, PlayerSeat seat) const noexcept -> ActionState {
    if (seat >= kMaxPlayerSeats || !id.Valid()) {
        return {};
    }
    for (const EvaluatedActionEntry& entry : _seats[seat].actions) {
        if (entry.id == id) {
            return entry.current;
        }
    }
    return ActionState {.activeDevice = _seats[seat].lastActiveDevice};
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
    -> std::vector<ActionId> {
    std::vector<ActionId> conflicts;
    const auto            map = FindActionMap(mapName);
    if (!map) {
        return conflicts;
    }
    const ActionId ignoredId(ignoreAction);
    for (const InputAction& action : map->Actions()) {
        if (ignoredId.Valid() && action.Id() == ignoredId) {
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
                    FormatCodeToken(b.kind, 0, b.code0),
                    FormatCodeToken(b.kind, 1, b.code1),
                    FormatCodeToken(b.kind, 2, b.code2),
                    FormatCodeToken(b.kind, 3, b.code3),
                    FormatModifiers(b.modifiers),
                    b.strictModifiers ? 1 : 0,
                    b.rawBypassUI ? 1 : 0,
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
            const auto c0   = ParseCodeToken(*bKind, 0, cols[4]);
            const auto c1   = ParseCodeToken(*bKind, 1, cols[5]);
            const auto c2   = ParseCodeToken(*bKind, 2, cols[6]);
            const auto c3   = ParseCodeToken(*bKind, 3, cols[7]);
            const auto mods = ParseModifiers(cols[8]);
            const auto sx   = ParseFloatToken(cols[11]);
            const auto sy   = ParseFloatToken(cols[12]);
            const auto th   = ParseFloatToken(cols[13]);
            if (!c0 || !c1 || !c2 || !c3 || !mods) {
                return std::unexpected(InputBindingParseError::UnknownEnumerator);
            }
            if (!sx || !sy || !th) {
                return std::unexpected(InputBindingParseError::InvalidNumericValue);
            }

            InputActionMap& m   = findOrMakeMap(cols[1]);
            auto            act = m.FindAction(cols[2]);
            if (!act) {
                act = m.AddAction(cols[2], ActionKind::Digital);
            }

            InputBinding binding {
                .kind            = *bKind,
                .code0           = *c0,
                .code1           = *c1,
                .code2           = *c2,
                .code3           = *c3,
                .modifiers       = *mods,
                .strictModifiers = (cols[9] != "0"),
                .rawBypassUI     = (cols[10] != "0"),
                .scaleX          = *sx,
                .scaleY          = *sy,
                .threshold       = *th,
            };

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
