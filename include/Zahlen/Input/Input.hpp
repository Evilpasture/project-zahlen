// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/EnumFlags.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/String.hpp>
#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace ZHLN {

enum class KeyCode : uint8_t {
    Unknown = 0,
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    LShift,
    RShift,
    LControl,
    RControl,
    LAlt,
    RAlt,
    Space,
    Escape,
    Enter,
    Backspace,
    Tab,
    Delete,
    Up,
    Down,
    Left,
    Right,
    LButton,
    RButton,
    MButton,
    Home,
    End,
    PageUp,
    PageDown,
    LSuper,
    RSuper,
    Insert,
    CapsLock,
    Pause,
    PrintScreen,
    Minus,
    Equal,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Apostrophe,
    GraveAccent,
    Comma,
    Period,
    Slash,
    XButton1,
    XButton2,
};

inline constexpr size_t kKeyCodeCount = static_cast<size_t>(KeyCode::XButton2) + 1;

enum class MouseButton : uint8_t {
    Left = 0,
    Right,
    Middle,
    X1,
    X2,
};

inline constexpr size_t kMouseButtonCount = static_cast<size_t>(MouseButton::X2) + 1;

[[nodiscard]] constexpr auto ToKeyCode(MouseButton button) noexcept -> KeyCode {
    switch (button) {
        case MouseButton::Left:
            return KeyCode::LButton;
        case MouseButton::Right:
            return KeyCode::RButton;
        case MouseButton::Middle:
            return KeyCode::MButton;
        case MouseButton::X1:
            return KeyCode::XButton1;
        case MouseButton::X2:
            return KeyCode::XButton2;
    }
    return KeyCode::Unknown;
}

[[nodiscard]] constexpr auto MouseButtonFromKeyCode(KeyCode key) noexcept -> Optional<MouseButton> {
    switch (key) {
        case KeyCode::LButton:
            return MouseButton::Left;
        case KeyCode::RButton:
            return MouseButton::Right;
        case KeyCode::MButton:
            return MouseButton::Middle;
        case KeyCode::XButton1:
            return MouseButton::X1;
        case KeyCode::XButton2:
            return MouseButton::X2;
        default:
            return Optional<MouseButton> {};
    }
}

enum class KeyModifiers : uint8_t {
    None  = 0,
    Shift = 1U << 0U,
    Ctrl  = 1U << 1U,
    Alt   = 1U << 2U,
    Super = 1U << 3U,
};

template <>
inline constexpr bool EnableEnumFlags<KeyModifiers> = true;

// --- Multi-Seat & Device Identity --------------------------------------------

inline constexpr size_t kMaxPlayerSeats = 4;
inline constexpr size_t kMaxGamepads    = 4;

struct PlayerSeat {
    uint8_t value = 0;

    constexpr PlayerSeat() noexcept = default;
    constexpr explicit PlayerSeat(uint8_t v) noexcept : value(v) {}

    [[nodiscard]] constexpr auto IsValid() const noexcept -> bool {
        return value < kMaxPlayerSeats;
    }
    [[nodiscard]] constexpr auto Index() const noexcept -> size_t {
        return static_cast<size_t>(value);
    }

    constexpr auto operator==(const PlayerSeat&) const noexcept -> bool = default;
};

struct GamepadSlot {
    uint8_t value = 0;

    constexpr GamepadSlot() noexcept = default;
    constexpr explicit GamepadSlot(uint8_t v) noexcept : value(v) {}

    [[nodiscard]] constexpr auto IsValid() const noexcept -> bool {
        return value < kMaxGamepads;
    }
    [[nodiscard]] constexpr auto Index() const noexcept -> size_t {
        return static_cast<size_t>(value);
    }

    constexpr auto operator==(const GamepadSlot&) const noexcept -> bool = default;
};

inline constexpr PlayerSeat  kPrimarySeat        {0};
inline constexpr PlayerSeat  kUnassignedSeat     {0xFF};
inline constexpr GamepadSlot kInvalidGamepadSlot {0xFF};

inline constexpr uint8_t kAllSeatsMaskBits = static_cast<uint8_t>((1U << kMaxPlayerSeats) - 1U);

struct SeatMask {
    uint8_t bits = 0;

    constexpr SeatMask() noexcept = default;
    constexpr explicit SeatMask(uint8_t b) noexcept : bits(static_cast<uint8_t>(b & kAllSeatsMaskBits)) {}

    [[nodiscard]] static constexpr auto All() noexcept -> SeatMask {
        return SeatMask {kAllSeatsMaskBits};
    }
    [[nodiscard]] static constexpr auto None() noexcept -> SeatMask {
        return SeatMask {0};
    }
    [[nodiscard]] static constexpr auto FromSeat(PlayerSeat seat) noexcept -> SeatMask {
        return seat.IsValid() ? SeatMask {static_cast<uint8_t>(1U << seat.value)} : None();
    }

    [[nodiscard]] constexpr auto Contains(PlayerSeat seat) const noexcept -> bool {
        return seat.IsValid() && ((bits & (1U << seat.value)) != 0U);
    }
    constexpr void Set(PlayerSeat seat) noexcept {
        if (seat.IsValid()) {
            bits = static_cast<uint8_t>(bits | (1U << seat.value));
        }
    }
    constexpr void Clear(PlayerSeat seat) noexcept {
        if (seat.IsValid()) {
            bits = static_cast<uint8_t>(bits & ~(1U << seat.value));
        }
    }

    constexpr auto operator==(const SeatMask&) const noexcept -> bool = default;
};

struct UiCaptureState {
    bool keyboard = false;
    bool mouse    = false;

    constexpr auto operator==(const UiCaptureState&) const noexcept -> bool = default;
};

enum class InputDeviceKind : uint8_t {
    KeyboardMouse = 0,
    Gamepad       = 1,
};

struct InputDeviceID {
    InputDeviceKind kind = InputDeviceKind::KeyboardMouse;
    GamepadSlot     slot {};

    [[nodiscard]] static constexpr auto KeyboardMouse(GamepadSlot deviceSlot = GamepadSlot {0}) noexcept -> InputDeviceID {
        return InputDeviceID {.kind = InputDeviceKind::KeyboardMouse, .slot = deviceSlot};
    }

    [[nodiscard]] static constexpr auto Gamepad(GamepadSlot gamepadSlot) noexcept -> InputDeviceID {
        return InputDeviceID {.kind = InputDeviceKind::Gamepad, .slot = gamepadSlot};
    }

    constexpr auto operator==(const InputDeviceID&) const noexcept -> bool = default;
};

enum class SeatRoutingPolicy : uint8_t {
    // Keyboard/mouse and Gamepad 0 both drive Seat 0; Gamepad 1..3 drive Seats 1..3.
    SharePrimarySeat = 0,
    // Keyboard/mouse drives Seat 0; newly connected gamepads claim dedicated seats (1..3, then 0 if free).
    DedicatedGamepadSeats = 1,
};

// --- Gamepad Buttons, Axes, Deadzones & Haptics ------------------------------

enum class GamepadButton : uint8_t {
    South = 0, // A / Cross
    East,      // B / Circle
    West,      // X / Square
    North,     // Y / Triangle
    LeftBumper,
    RightBumper,
    Back,  // View / Share / Select
    Start, // Menu / Options
    Guide,
    LeftThumb,
    RightThumb,
    DPadUp,
    DPadRight,
    DPadDown,
    DPadLeft,
};

inline constexpr size_t kGamepadButtonCount = static_cast<size_t>(GamepadButton::DPadLeft) + 1;

enum class GamepadAxis : uint8_t {
    LeftX = 0,    // [-1, +1] (+X right)
    LeftY,        // [-1, +1] (+Y up / forward)
    RightX,       // [-1, +1] (+X right)
    RightY,       // [-1, +1] (+Y up / forward)
    LeftTrigger,  // [ 0, +1] (0 released, +1 full pull)
    RightTrigger, // [ 0, +1] (0 released, +1 full pull)
};

inline constexpr size_t kGamepadAxisCount = static_cast<size_t>(GamepadAxis::RightTrigger) + 1;

enum class DeadzoneMode : uint8_t {
    ScaledRadial = 0,
    Axial        = 1,
    Hybrid       = 2,
};

struct DeadzoneConfig {
    DeadzoneMode mode          = DeadzoneMode::ScaledRadial;
    float        inner         = 0.14f;
    float        outer         = 0.95f;
    float        curveExponent = 1.0f;

    constexpr auto operator==(const DeadzoneConfig&) const noexcept -> bool = default;
};

struct StickSample {
    float x         = 0.0f;
    float y         = 0.0f;
    float magnitude = 0.0f;

    constexpr auto operator==(const StickSample&) const noexcept -> bool = default;
};

[[nodiscard]] inline auto ApplyResponseCurve(float unitMagnitude, float exponent) noexcept -> float {
    const float clamped = std::clamp(unitMagnitude, 0.0f, 1.0f);
    if (!(exponent > 0.0f) || std::abs(exponent - 1.0f) <= 1e-4f) {
        return clamped;
    }
    return std::pow(clamped, exponent);
}

[[nodiscard]] inline auto ApplyAxisDeadzone(float raw, float inner = 0.12f, float outer = 0.95f, float curveExponent = 1.0f) noexcept -> float {
    if (!(raw > -std::numeric_limits<float>::infinity() && raw < std::numeric_limits<float>::infinity())) {
        return 0.0f;
    }
    const float safeInner = std::clamp(inner, 0.0f, 0.99f);
    const float safeOuter = std::clamp(outer, safeInner + 1e-4f, 1.0f);
    const float absVal    = std::abs(raw);
    if (absVal <= safeInner) {
        return 0.0f;
    }
    const float normalized = std::clamp((absVal - safeInner) / (safeOuter - safeInner), 0.0f, 1.0f);
    const float curved     = ApplyResponseCurve(normalized, curveExponent);
    return (raw < 0.0f) ? -curved : curved;
}

[[nodiscard]] inline auto ApplyTriggerDeadzone(float rawZeroToOne, float inner = 0.05f, float outer = 0.98f, float curveExponent = 1.0f) noexcept
    -> float {
    if (!(rawZeroToOne > 0.0f && rawZeroToOne < std::numeric_limits<float>::infinity())) {
        return 0.0f;
    }
    const float safeInner = std::clamp(inner, 0.0f, 0.99f);
    const float safeOuter = std::clamp(outer, safeInner + 1e-4f, 1.0f);
    if (rawZeroToOne <= safeInner) {
        return 0.0f;
    }
    const float normalized = std::clamp((rawZeroToOne - safeInner) / (safeOuter - safeInner), 0.0f, 1.0f);
    return ApplyResponseCurve(normalized, curveExponent);
}

[[nodiscard]] inline auto ApplyStickDeadzone(float rawX, float rawY, const DeadzoneConfig& cfg = {}) noexcept -> StickSample {
    if (!(rawX > -std::numeric_limits<float>::infinity() && rawX < std::numeric_limits<float>::infinity()) ||
        !(rawY > -std::numeric_limits<float>::infinity() && rawY < std::numeric_limits<float>::infinity())) {
        return {};
    }

    const float safeInner = std::clamp(cfg.inner, 0.0f, 0.99f);
    const float safeOuter = std::clamp(cfg.outer, safeInner + 1e-4f, 1.0f);

    if (cfg.mode == DeadzoneMode::Axial) {
        const float fx  = ApplyAxisDeadzone(rawX, safeInner, safeOuter, cfg.curveExponent);
        const float fy  = ApplyAxisDeadzone(rawY, safeInner, safeOuter, cfg.curveExponent);
        const float mag = std::sqrt((fx * fx) + (fy * fy));
        if (mag > 1.0f) {
            return StickSample {.x = fx / mag, .y = fy / mag, .magnitude = 1.0f};
        }
        return StickSample {.x = fx, .y = fy, .magnitude = mag};
    }

    const float rawMag = std::sqrt((rawX * rawX) + (rawY * rawY));
    if (rawMag <= safeInner || rawMag <= 1e-6f) {
        return {};
    }

    if (cfg.mode == DeadzoneMode::Hybrid) {
        // Radial activation gate + gentle cardinal bow-tie stabilization (25% of inner deadzone).
        const float axialGate = safeInner * 0.25f;
        const float ax        = (std::abs(rawX) <= axialGate) ? 0.0f : rawX;
        const float ay        = (std::abs(rawY) <= axialGate) ? 0.0f : rawY;
        const float gatedMag  = std::sqrt((ax * ax) + (ay * ay));
        if (gatedMag <= 1e-6f) {
            return {};
        }
        const float norm   = std::clamp((rawMag - safeInner) / (safeOuter - safeInner), 0.0f, 1.0f);
        const float curved = ApplyResponseCurve(norm, cfg.curveExponent);
        const float dirX   = ax / gatedMag;
        const float dirY   = ay / gatedMag;
        return StickSample {.x = dirX * curved, .y = dirY * curved, .magnitude = curved};
    }

    // ScaledRadial
    const float norm   = std::clamp((rawMag - safeInner) / (safeOuter - safeInner), 0.0f, 1.0f);
    const float curved = ApplyResponseCurve(norm, cfg.curveExponent);
    const float dirX   = rawX / rawMag;
    const float dirY   = rawY / rawMag;
    return StickSample {.x = dirX * curved, .y = dirY * curved, .magnitude = curved};
}

struct GamepadHardwareSample {
    std::array<bool, kGamepadButtonCount> buttons {};
    std::array<float, kGamepadAxisCount>  axes {};

    constexpr void SetButton(GamepadButton button, bool down) noexcept {
        const size_t idx = EnumIndex(button);
        if (idx < kGamepadButtonCount) {
            buttons[idx] = down;
        }
    }

    constexpr void SetAxis(GamepadAxis axis, float value) noexcept {
        const size_t idx = EnumIndex(axis);
        if (idx < kGamepadAxisCount) {
            axes[idx] = value;
        }
    }
};

struct GamepadState {
    bool       connected    = false;
    PlayerSeat assignedSeat = kUnassignedSeat;
    String32   name {};

    std::bitset<kGamepadButtonCount> buttonsDown {};
    std::bitset<kGamepadButtonCount> prevButtonsDown {};
    std::bitset<kGamepadButtonCount> pressedButtons {};
    std::bitset<kGamepadButtonCount> releasedButtons {};

    std::array<float, kGamepadAxisCount> hardwareAxes {};

    EnumBitMask<GamepadAxis> triggersDown {};
    EnumBitMask<GamepadAxis> prevTriggersDown {};
    EnumBitMask<GamepadAxis> pressedTriggersMask {};
    EnumBitMask<GamepadAxis> releasedTriggersMask {};

    DeadzoneConfig leftStickDeadzone {};
    DeadzoneConfig rightStickDeadzone {};
    float          triggerDeadzone        = 0.05f;
    float          triggerButtonThreshold = 0.50f;

    void BeginFrame() noexcept {
        prevButtonsDown = buttonsDown;
        pressedButtons.reset();
        releasedButtons.reset();
        prevTriggersDown = triggersDown;
        pressedTriggersMask.Reset();
        releasedTriggersMask.Reset();
    }

    void ResetTransientState() noexcept {
        buttonsDown.reset();
        prevButtonsDown.reset();
        pressedButtons.reset();
        releasedButtons.reset();
        hardwareAxes.fill(0.0f);
        triggersDown.Reset();
        prevTriggersDown.Reset();
        pressedTriggersMask.Reset();
        releasedTriggersMask.Reset();
    }

    void SetButton(GamepadButton button, bool down) noexcept {
        const size_t idx = EnumIndex(button);
        if (idx >= kGamepadButtonCount) {
            return;
        }
        const bool wasDown = buttonsDown.test(idx);
        if (down && !wasDown) {
            pressedButtons.set(idx);
        } else if (!down && wasDown) {
            releasedButtons.set(idx);
        }
        buttonsDown.set(idx, down);
    }

    void SetAxis(GamepadAxis axis, float value) noexcept {
        const size_t idx = EnumIndex(axis);
        if (idx >= kGamepadAxisCount) {
            return;
        }
        if (axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger) {
            const float clamped = std::clamp(value, 0.0f, 1.0f);
            hardwareAxes[idx]        = clamped;
            const bool down     = ApplyTriggerDeadzone(clamped, triggerDeadzone) >= triggerButtonThreshold;
            const bool was      = triggersDown.Contains(axis);
            if (down && !was) {
                pressedTriggersMask.Set(axis);
            } else if (!down && was) {
                releasedTriggersMask.Set(axis);
            }
            if (down) {
                triggersDown.Set(axis);
            } else {
                triggersDown.Clear(axis);
            }
        } else {
            hardwareAxes[idx] = std::clamp(value, -1.0f, 1.0f);
        }
    }

    void ApplySample(const GamepadHardwareSample& sample) noexcept {
        for (size_t i = 0; i < kGamepadButtonCount; ++i) {
            SetButton(static_cast<GamepadButton>(i), sample.buttons[i]);
        }
        for (size_t i = 0; i < kGamepadAxisCount; ++i) {
            SetAxis(static_cast<GamepadAxis>(i), sample.axes[i]);
        }
    }

    [[nodiscard]] auto IsButtonDown(GamepadButton button) const noexcept -> bool {
        const size_t idx = EnumIndex(button);
        return connected && idx < kGamepadButtonCount && buttonsDown.test(idx);
    }

    [[nodiscard]] auto IsButtonPressed(GamepadButton button) const noexcept -> bool {
        const size_t idx = EnumIndex(button);
        if (!connected || idx >= kGamepadButtonCount) {
            return false;
        }
        return pressedButtons.test(idx) || (buttonsDown.test(idx) && !prevButtonsDown.test(idx));
    }

    [[nodiscard]] auto IsButtonHeld(GamepadButton button) const noexcept -> bool {
        const size_t idx = EnumIndex(button);
        return connected && idx < kGamepadButtonCount && buttonsDown.test(idx) && prevButtonsDown.test(idx) && !pressedButtons.test(idx);
    }

    [[nodiscard]] auto IsButtonReleased(GamepadButton button) const noexcept -> bool {
        const size_t idx = EnumIndex(button);
        if (!connected || idx >= kGamepadButtonCount) {
            return false;
        }
        return releasedButtons.test(idx) || (!buttonsDown.test(idx) && prevButtonsDown.test(idx));
    }

    [[nodiscard]] auto HardwareAxis(GamepadAxis axis) const noexcept -> float {
        const size_t idx = EnumIndex(axis);
        return (connected && idx < kGamepadAxisCount) ? hardwareAxes[idx] : 0.0f;
    }

    [[nodiscard]] auto LeftStick() const noexcept -> StickSample {
        if (!connected) {
            return {};
        }
        return ApplyStickDeadzone(hardwareAxes[EnumIndex(GamepadAxis::LeftX)], hardwareAxes[EnumIndex(GamepadAxis::LeftY)], leftStickDeadzone);
    }

    [[nodiscard]] auto RightStick() const noexcept -> StickSample {
        if (!connected) {
            return {};
        }
        return ApplyStickDeadzone(hardwareAxes[EnumIndex(GamepadAxis::RightX)], hardwareAxes[EnumIndex(GamepadAxis::RightY)], rightStickDeadzone);
    }

    [[nodiscard]] auto LeftTrigger() const noexcept -> float {
        return connected ? ApplyTriggerDeadzone(hardwareAxes[EnumIndex(GamepadAxis::LeftTrigger)], triggerDeadzone) : 0.0f;
    }

    [[nodiscard]] auto RightTrigger() const noexcept -> float {
        return connected ? ApplyTriggerDeadzone(hardwareAxes[EnumIndex(GamepadAxis::RightTrigger)], triggerDeadzone) : 0.0f;
    }

    [[nodiscard]] auto Axis(GamepadAxis axis) const noexcept -> float {
        if (!connected) {
            return 0.0f;
        }
        switch (axis) {
            case GamepadAxis::LeftX:
                return LeftStick().x;
            case GamepadAxis::LeftY:
                return LeftStick().y;
            case GamepadAxis::RightX:
                return RightStick().x;
            case GamepadAxis::RightY:
                return RightStick().y;
            case GamepadAxis::LeftTrigger:
                return LeftTrigger();
            case GamepadAxis::RightTrigger:
                return RightTrigger();
        }
        return 0.0f;
    }

    [[nodiscard]] auto IsTriggerDown(GamepadAxis axis) const noexcept -> bool {
        return connected && (axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger) && triggersDown.Contains(axis);
    }

    [[nodiscard]] auto IsTriggerPressed(GamepadAxis axis) const noexcept -> bool {
        if (!connected || (axis != GamepadAxis::LeftTrigger && axis != GamepadAxis::RightTrigger)) {
            return false;
        }
        return pressedTriggersMask.Contains(axis) || (triggersDown.Contains(axis) && !prevTriggersDown.Contains(axis));
    }

    [[nodiscard]] auto IsTriggerReleased(GamepadAxis axis) const noexcept -> bool {
        if (!connected || (axis != GamepadAxis::LeftTrigger && axis != GamepadAxis::RightTrigger)) {
            return false;
        }
        return releasedTriggersMask.Contains(axis) || (!triggersDown.Contains(axis) && prevTriggersDown.Contains(axis));
    }

    [[nodiscard]] auto HasAnyActivity() const noexcept -> bool {
        if (!connected) {
            return false;
        }
        if (buttonsDown.any() || pressedButtons.any()) {
            return true;
        }
        return LeftStick().magnitude > 0.01f || RightStick().magnitude > 0.01f || LeftTrigger() > 0.01f || RightTrigger() > 0.01f;
    }
};

struct GamepadRumble {
    float lowFrequency    = 0.0f; // [0, 1] heavy motor
    float highFrequency   = 0.0f; // [0, 1] light motor
    float leftTrigger     = 0.0f; // [0, 1] left trigger motor
    float rightTrigger    = 0.0f; // [0, 1] right trigger motor
    float durationSeconds = 0.0f; // <= 0 stops motor output

    [[nodiscard]] constexpr auto IsZero() const noexcept -> bool {
        return lowFrequency <= 0.0f && highFrequency <= 0.0f && leftTrigger <= 0.0f && rightTrigger <= 0.0f;
    }

    constexpr auto operator==(const GamepadRumble&) const noexcept -> bool = default;
};

struct GamepadConnectionEvent {
    GamepadSlot gamepadSlot {};
    PlayerSeat assignedSeat = kUnassignedSeat;
    bool       connected    = false;
    String32   name {};
};

struct InputState {
    PlayerSeat seat = kPrimarySeat;

    std::bitset<kKeyCodeCount> keys {};
    std::bitset<kKeyCodeCount> prevKeys {};
    std::bitset<kKeyCodeCount> pressedKeys {};
    std::bitset<kKeyCodeCount> releasedKeys {};

    float mouseX      = 0.0f;
    float mouseY      = 0.0f;
    float mouseDeltaX = 0.0f;
    float mouseDeltaY = 0.0f;
    float mouseWheel  = 0.0f;

    float lastX      = 0.0f;
    float lastY      = 0.0f;
    bool  firstMouse = true;

    void BeginFrame() noexcept {
        prevKeys = keys;
        pressedKeys.reset();
        releasedKeys.reset();
        ResetDeltas();
    }

    void SetKey(KeyCode key, bool down) noexcept {
        const size_t idx = EnumIndex(key);
        if (key == KeyCode::Unknown || idx >= keys.size()) {
            return;
        }
        const bool wasDown = keys.test(idx);
        if (down && !wasDown) {
            pressedKeys.set(idx);
        } else if (!down && wasDown) {
            releasedKeys.set(idx);
        }
        keys.set(idx, down);
    }

    void SetMouseButton(MouseButton button, bool down) noexcept {
        SetKey(ToKeyCode(button), down);
    }

    void ApplyLocalMotion(float x, float y) noexcept {
        mouseX = x;
        mouseY = y;
        if (firstMouse) {
            lastX      = x;
            lastY      = y;
            firstMouse = false;
        }
        mouseDeltaX += (x - lastX);
        mouseDeltaY += (y - lastY);
        lastX = x;
        lastY = y;
    }

    void ApplyRelativeMotion(float x, float y, float dx, float dy) noexcept {
        mouseX     = x;
        mouseY     = y;
        lastX      = x;
        lastY      = y;
        firstMouse = false;
        mouseDeltaX += dx;
        mouseDeltaY += dy;
    }

    void ApplyWheel(float delta) noexcept {
        mouseWheel += delta;
    }

    struct QueuedInput {
        uint32_t codepoint = 0;
        KeyCode  key       = KeyCode::Unknown;
        bool     isChar    = false;
    };
    static constexpr size_t                  kMaxQueuedInput = 64;
    std::array<QueuedInput, kMaxQueuedInput> queuedInput {};
    size_t                                   queuedInputCount = 0;

    void QueueChar(uint32_t codepoint) noexcept {
        if (queuedInputCount < kMaxQueuedInput) {
            queuedInput[queuedInputCount++] = QueuedInput {.codepoint = codepoint, .isChar = true};
        }
    }

    void QueueKeyPress(KeyCode key) noexcept {
        if (queuedInputCount < kMaxQueuedInput) {
            queuedInput[queuedInputCount++] = QueuedInput {.key = key, .isChar = false};
        }
    }

    void ClearQueuedInput() noexcept {
        queuedInputCount = 0;
    }

    void ResetDeltas() noexcept {
        mouseDeltaX = 0.0f;
        mouseDeltaY = 0.0f;
        mouseWheel  = 0.0f;
    }

    // --- Physical Key Level & Edge Queries -----------------------------------
    // InputState is pure physical input. Pass UiCaptureState to apply UI
    // gating without duplicating queries; omit it (default) for physical.

    [[nodiscard]] bool IsKeyDown(KeyCode key, UiCaptureState uiCapture = {}) const noexcept {
        if (uiCapture.keyboard) {
            return false;
        }
        const size_t idx = EnumIndex(key);
        return key != KeyCode::Unknown && idx < keys.size() && keys.test(idx);
    }

    [[nodiscard]] bool IsKeyPressed(KeyCode key, UiCaptureState uiCapture = {}) const noexcept {
        if (uiCapture.keyboard) {
            return false;
        }
        const size_t idx = EnumIndex(key);
        if (key == KeyCode::Unknown || idx >= keys.size()) {
            return false;
        }
        return pressedKeys.test(idx) || (keys.test(idx) && !prevKeys.test(idx));
    }

    [[nodiscard]] bool IsKeyHeld(KeyCode key, UiCaptureState uiCapture = {}) const noexcept {
        if (uiCapture.keyboard) {
            return false;
        }
        const size_t idx = EnumIndex(key);
        return key != KeyCode::Unknown && idx < keys.size() && keys.test(idx) && prevKeys.test(idx) && !pressedKeys.test(idx);
    }

    [[nodiscard]] bool IsKeyReleased(KeyCode key, UiCaptureState uiCapture = {}) const noexcept {
        if (uiCapture.keyboard) {
            return false;
        }
        const size_t idx = EnumIndex(key);
        if (key == KeyCode::Unknown || idx >= keys.size()) {
            return false;
        }
        return releasedKeys.test(idx) || (!keys.test(idx) && prevKeys.test(idx));
    }

    // --- Physical Mouse Button Level & Edge Queries --------------------------

    [[nodiscard]] bool IsMouseButtonDown(MouseButton button, UiCaptureState uiCapture = {}) const noexcept {
        return IsKeyDown(ToKeyCode(button), uiCapture);
    }

    [[nodiscard]] bool IsMouseButtonPressed(MouseButton button, UiCaptureState uiCapture = {}) const noexcept {
        return IsKeyPressed(ToKeyCode(button), uiCapture);
    }

    [[nodiscard]] bool IsMouseButtonHeld(MouseButton button, UiCaptureState uiCapture = {}) const noexcept {
        return IsKeyHeld(ToKeyCode(button), uiCapture);
    }

    [[nodiscard]] bool IsMouseButtonReleased(MouseButton button, UiCaptureState uiCapture = {}) const noexcept {
        return IsKeyReleased(ToKeyCode(button), uiCapture);
    }

    // --- Physical Modifiers --------------------------------------------------

    [[nodiscard]] auto Modifiers(UiCaptureState uiCapture = {}) const noexcept -> KeyModifiers {
        if (uiCapture.keyboard) {
            return KeyModifiers::None;
        }
        KeyModifiers mods = KeyModifiers::None;
        if (IsKeyDown(KeyCode::LShift) || IsKeyDown(KeyCode::RShift)) {
            mods |= KeyModifiers::Shift;
        }
        if (IsKeyDown(KeyCode::LControl) || IsKeyDown(KeyCode::RControl)) {
            mods |= KeyModifiers::Ctrl;
        }
        if (IsKeyDown(KeyCode::LAlt) || IsKeyDown(KeyCode::RAlt)) {
            mods |= KeyModifiers::Alt;
        }
        if (IsKeyDown(KeyCode::LSuper) || IsKeyDown(KeyCode::RSuper)) {
            mods |= KeyModifiers::Super;
        }
        return mods;
    }

    [[nodiscard]] auto HasModifiers(KeyModifiers required, bool exact = true, UiCaptureState uiCapture = {}) const noexcept -> bool {
        const KeyModifiers current = Modifiers(uiCapture);
        return exact ? (current == required) : ((current & required) == required);
    }

    [[nodiscard]] float GetMouseDeltaX(UiCaptureState uiCapture = {}) const noexcept {
        return uiCapture.mouse ? 0.0f : mouseDeltaX;
    }

    [[nodiscard]] float GetMouseDeltaY(UiCaptureState uiCapture = {}) const noexcept {
        return uiCapture.mouse ? 0.0f : mouseDeltaY;
    }

    [[nodiscard]] float GetMouseWheel(UiCaptureState uiCapture = {}) const noexcept {
        return uiCapture.mouse ? 0.0f : mouseWheel;
    }
};

static_assert(std::is_trivially_copyable_v<PlayerSeat>);
static_assert(std::is_trivially_copyable_v<GamepadSlot>);
static_assert(std::is_trivially_copyable_v<SeatMask>);
static_assert(std::is_trivially_copyable_v<UiCaptureState>);
static_assert(std::is_trivially_copyable_v<InputDeviceID>);
static_assert(std::is_trivially_copyable_v<DeadzoneConfig>);
static_assert(std::is_trivially_copyable_v<StickSample>);
static_assert(std::is_trivially_copyable_v<GamepadHardwareSample>);
static_assert(std::is_trivially_copyable_v<GamepadState>);
static_assert(std::is_trivially_copyable_v<GamepadRumble>);
static_assert(std::is_trivially_copyable_v<GamepadConnectionEvent>);
static_assert(std::is_trivially_copyable_v<InputState>);

} // namespace ZHLN
