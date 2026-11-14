// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Input/ActionValues.hpp
//
// The virtual-input vocabulary: what an action is called, what state it is in,
// which physical input produces it, and the fixed-layout configuration nodes
// that set up action maps across the COM boundary. Split out of
// InputContext.hpp the way PhysicsValues.hpp was split out of Physics.hpp --
// naming an action or describing a binding should not require the router, its
// action-map storage or <vector> and <string>. InputContext.hpp includes this
// file, so every existing spelling still resolves.
//
// Every type here is a fixed layout of scalars, which is what lets them cross
// the COM boundary.

#include <Zahlen/Core/EnumFlags.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Input/Input.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ZHLN {

struct ActionID {
    uint64_t value = 0;

    constexpr ActionID() noexcept = default;
    constexpr explicit ActionID(uint64_t rawHash) noexcept : value(rawHash) {}
    constexpr ActionID(std::string_view actionName) noexcept : value(actionName.empty() ? 0ULL : Hash64(actionName)) {}
    constexpr ActionID(const char* actionName) noexcept
        : value((actionName == nullptr || actionName[0] == '\0') ? 0ULL : Hash64(std::string_view(actionName))) {}

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return value != 0;
    }

    constexpr auto operator==(const ActionID&) const noexcept -> bool = default;
};

enum class ActionKind : uint8_t {
    Digital = 0,
    Axis1D  = 1,
    Axis2D  = 2,
};

struct Axis2DValue {
    float x = 0.0f;
    float y = 0.0f;

    [[nodiscard]] auto LengthSquared() const noexcept -> float {
        return (x * x) + (y * y);
    }

    [[nodiscard]] auto Length() const noexcept -> float {
        return std::sqrt(LengthSquared());
    }

    constexpr auto operator==(const Axis2DValue&) const noexcept -> bool = default;
};

struct ActionState {
    float           x            = 0.0f;
    float           y            = 0.0f;
    bool            down         = false;
    bool            pressed      = false;
    bool            held         = false;
    bool            released     = false;
    InputDeviceKind activeDevice = InputDeviceKind::KeyboardMouse;

    [[nodiscard]] constexpr auto Value1D() const noexcept -> float {
        return x;
    }

    [[nodiscard]] constexpr auto Value2D() const noexcept -> Axis2DValue {
        return Axis2DValue {.x = x, .y = y};
    }

    [[nodiscard]] constexpr auto Down() const noexcept -> bool {
        return down;
    }

    [[nodiscard]] constexpr auto Pressed() const noexcept -> bool {
        return pressed;
    }

    [[nodiscard]] constexpr auto Held() const noexcept -> bool {
        return held;
    }

    [[nodiscard]] constexpr auto Released() const noexcept -> bool {
        return released;
    }
};

enum class InputBindingKind : uint8_t {
    None = 0,
    Key,
    KeyChord,
    MouseButton,
    MouseDeltaX,
    MouseDeltaY,
    MouseDelta2D,
    MouseWheel,
    GamepadButton,
    GamepadChord,
    GamepadAxis1D,
    Composite1DKeys,
    Composite1DGamepad,
    Composite2DKeys,
    Composite2DGamepad,
    GamepadStick2D,
};

// Trivially copyable physical-to-virtual binding descriptor.
// Construct via the semantic static factories (`InputBinding::Key`, `WASD`, `LeftStick2D`, ...).
struct InputBinding {
    InputBindingKind             kind            = InputBindingKind::None;
    std::array<KeyCode, 4>       keys            = {KeyCode::Unknown, KeyCode::Unknown, KeyCode::Unknown, KeyCode::Unknown};
    MouseButton                  mouseButton     = MouseButton::Left;
    std::array<GamepadButton, 4> gamepadButtons  = {GamepadButton::South, GamepadButton::South, GamepadButton::South, GamepadButton::South};
    std::array<GamepadAxis, 2>   gamepadAxes     = {GamepadAxis::LeftX, GamepadAxis::LeftX};
    KeyModifiers                 modifiers       = KeyModifiers::None;
    bool                         strictModifiers = false;
    bool                         bypassUiCapture = false;
    float                        scaleX          = 1.0f;
    float                        scaleY          = 1.0f;
    float                        threshold       = 0.50f;

    [[nodiscard]] static constexpr auto Key(KeyCode key, KeyModifiers requiredMods = KeyModifiers::None, float scale = 1.0f) noexcept
        -> InputBinding {
        return InputBinding {
            .kind            = InputBindingKind::Key,
            .keys            = {key, KeyCode::Unknown, KeyCode::Unknown, KeyCode::Unknown},
            .modifiers       = requiredMods,
            .strictModifiers = (requiredMods != KeyModifiers::None),
            .scaleX          = scale,
        };
    }

    [[nodiscard]] static constexpr auto KeyChord(KeyCode first, KeyCode second, KeyModifiers requiredMods = KeyModifiers::None) noexcept
        -> InputBinding {
        return InputBinding {
            .kind            = InputBindingKind::KeyChord,
            .keys            = {first, second, KeyCode::Unknown, KeyCode::Unknown},
            .modifiers       = requiredMods,
            .strictModifiers = (requiredMods != KeyModifiers::None),
        };
    }

    [[nodiscard]] static constexpr auto Mouse(MouseButton button, KeyModifiers requiredMods = KeyModifiers::None) noexcept -> InputBinding {
        return InputBinding {
            .kind            = InputBindingKind::MouseButton,
            .mouseButton     = button,
            .modifiers       = requiredMods,
            .strictModifiers = (requiredMods != KeyModifiers::None),
        };
    }

    [[nodiscard]] static constexpr auto MouseDeltaX(float sensitivity = 1.0f) noexcept -> InputBinding {
        return InputBinding {.kind = InputBindingKind::MouseDeltaX, .scaleX = sensitivity};
    }

    [[nodiscard]] static constexpr auto MouseDeltaY(float sensitivity = 1.0f) noexcept -> InputBinding {
        return InputBinding {.kind = InputBindingKind::MouseDeltaY, .scaleX = sensitivity};
    }

    [[nodiscard]] static constexpr auto MouseDelta2D(float sensitivityX = 1.0f, float sensitivityY = 1.0f) noexcept -> InputBinding {
        return InputBinding {.kind = InputBindingKind::MouseDelta2D, .scaleX = sensitivityX, .scaleY = sensitivityY};
    }

    [[nodiscard]] static constexpr auto MouseWheel(float scale = 1.0f) noexcept -> InputBinding {
        return InputBinding {.kind = InputBindingKind::MouseWheel, .scaleX = scale};
    }

    [[nodiscard]] static constexpr auto GamepadBtn(GamepadButton button, float scale = 1.0f) noexcept -> InputBinding {
        return InputBinding {
            .kind           = InputBindingKind::GamepadButton,
            .gamepadButtons = {button, GamepadButton::South, GamepadButton::South, GamepadButton::South},
            .scaleX         = scale,
        };
    }

    [[nodiscard]] static constexpr auto GamepadChord(GamepadButton first, GamepadButton second) noexcept -> InputBinding {
        return InputBinding {
            .kind           = InputBindingKind::GamepadChord,
            .gamepadButtons = {first, second, GamepadButton::South, GamepadButton::South},
        };
    }

    [[nodiscard]] static constexpr auto GamepadAxis1D(GamepadAxis axis, float scale = 1.0f, float actuationThreshold = 0.50f) noexcept
        -> InputBinding {
        return InputBinding {
            .kind        = InputBindingKind::GamepadAxis1D,
            .gamepadAxes = {axis, GamepadAxis::LeftX},
            .scaleX      = scale,
            .threshold   = actuationThreshold,
        };
    }

    [[nodiscard]] static constexpr auto Composite1DKeys(KeyCode negative, KeyCode positive, float scale = 1.0f) noexcept -> InputBinding {
        return InputBinding {
            .kind   = InputBindingKind::Composite1DKeys,
            .keys   = {negative, positive, KeyCode::Unknown, KeyCode::Unknown},
            .scaleX = scale,
        };
    }

    [[nodiscard]] static constexpr auto Composite1DGamepad(GamepadButton negative, GamepadButton positive, float scale = 1.0f) noexcept
        -> InputBinding {
        return InputBinding {
            .kind           = InputBindingKind::Composite1DGamepad,
            .gamepadButtons = {negative, positive, GamepadButton::South, GamepadButton::South},
            .scaleX         = scale,
        };
    }

    [[nodiscard]] static constexpr auto Composite2DKeys(KeyCode up, KeyCode down, KeyCode left, KeyCode right, float scale = 1.0f) noexcept
        -> InputBinding {
        return InputBinding {
            .kind   = InputBindingKind::Composite2DKeys,
            .keys   = {up, down, left, right},
            .scaleX = scale,
            .scaleY = scale,
        };
    }

    [[nodiscard]] static constexpr auto WASD(float scale = 1.0f) noexcept -> InputBinding {
        return Composite2DKeys(KeyCode::W, KeyCode::S, KeyCode::A, KeyCode::D, scale);
    }

    [[nodiscard]] static constexpr auto ArrowKeys(float scale = 1.0f) noexcept -> InputBinding {
        return Composite2DKeys(KeyCode::Up, KeyCode::Down, KeyCode::Left, KeyCode::Right, scale);
    }

    [[nodiscard]] static constexpr auto
        Composite2DGamepad(GamepadButton up, GamepadButton down, GamepadButton left, GamepadButton right, float scale = 1.0f) noexcept
        -> InputBinding {
        return InputBinding {
            .kind           = InputBindingKind::Composite2DGamepad,
            .gamepadButtons = {up, down, left, right},
            .scaleX         = scale,
            .scaleY         = scale,
        };
    }

    [[nodiscard]] static constexpr auto DPad2D(float scale = 1.0f) noexcept -> InputBinding {
        return Composite2DGamepad(
            GamepadButton::DPadUp, GamepadButton::DPadDown, GamepadButton::DPadLeft, GamepadButton::DPadRight, scale
        );
    }

    [[nodiscard]] static constexpr auto
        GamepadStick2D(GamepadAxis xAxis, GamepadAxis yAxis, float scaleX = 1.0f, float scaleY = 1.0f) noexcept -> InputBinding {
        return InputBinding {
            .kind        = InputBindingKind::GamepadStick2D,
            .gamepadAxes = {xAxis, yAxis},
            .scaleX      = scaleX,
            .scaleY      = scaleY,
        };
    }

    [[nodiscard]] static constexpr auto LeftStick2D(float scaleX = 1.0f, float scaleY = 1.0f) noexcept -> InputBinding {
        return GamepadStick2D(GamepadAxis::LeftX, GamepadAxis::LeftY, scaleX, scaleY);
    }

    [[nodiscard]] static constexpr auto RightStick2D(float scaleX = 1.0f, float scaleY = 1.0f) noexcept -> InputBinding {
        return GamepadStick2D(GamepadAxis::RightX, GamepadAxis::RightY, scaleX, scaleY);
    }

    [[nodiscard]] constexpr auto WithBypassUiCapture(bool bypass = true) const noexcept -> InputBinding {
        InputBinding copy        = *this;
        copy.bypassUiCapture    = bypass;
        return copy;
    }

    [[nodiscard]] constexpr auto WithStrictModifiers(bool strict = true) const noexcept -> InputBinding {
        InputBinding copy    = *this;
        copy.strictModifiers = strict;
        return copy;
    }

    [[nodiscard]] constexpr auto Specificity() const noexcept -> uint32_t {
        uint32_t score = 1;
        if (kind == InputBindingKind::KeyChord || kind == InputBindingKind::GamepadChord) {
            score += 2;
        }
        for (const KeyModifiers bit : {KeyModifiers::Shift, KeyModifiers::Ctrl, KeyModifiers::Alt, KeyModifiers::Super}) {
            if (HasFlag(modifiers, bit)) {
                ++score;
            }
        }
        return score;
    }

    constexpr auto operator==(const InputBinding&) const noexcept -> bool = default;
};

// --- Chainable fixed-layout action map & routing descriptors -----------------

struct ActionMapConfig {
    String64 name {};
    int32_t  priority              = 0;
    SeatMask seatMask              = SeatMask::All();
    bool     enabled               = true;
    bool     blocksLowerPriority   = false;
    bool     consumePhysicalInputs = true;
};

struct ActionConfig {
    String64            mapName {};
    String64            actionName {};
    ActionKind          kind               = ActionKind::Digital;
    float               actuationThreshold = 0.50f;
    bool                clampUnitCircle    = true;
    bool                replaceBindings    = true;
    const InputBinding* bindings           = nullptr;
    uint32_t            bindingCount       = 0;
};

struct ActionRebindConfig {
    uint32_t     slotIndex = 0;
    InputBinding binding {};
};

struct InputRoutingConfig {
    SeatRoutingPolicy policy                = SeatRoutingPolicy::SharePrimarySeat;
    SeatMask          keyboardMouseSeatMask = SeatMask::FromSeat(kPrimarySeat);
};

static_assert(std::is_trivially_copyable_v<InputBinding>);
static_assert(std::is_standard_layout_v<ActionID> && std::is_trivially_copyable_v<ActionID>);
static_assert(std::is_standard_layout_v<ActionState> && std::is_trivially_copyable_v<ActionState>);
static_assert(std::is_standard_layout_v<Axis2DValue> && std::is_trivially_copyable_v<Axis2DValue>);
static_assert(std::is_standard_layout_v<InputBinding>);
static_assert(std::is_standard_layout_v<ActionMapConfig> && std::is_trivially_copyable_v<ActionMapConfig>);
static_assert(std::is_standard_layout_v<ActionConfig> && std::is_trivially_copyable_v<ActionConfig>);
static_assert(std::is_standard_layout_v<ActionRebindConfig> && std::is_trivially_copyable_v<ActionRebindConfig>);
static_assert(std::is_standard_layout_v<InputRoutingConfig> && std::is_trivially_copyable_v<InputRoutingConfig>);

} // namespace ZHLN
