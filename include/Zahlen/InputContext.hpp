// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Input.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN::ECS {
class EventBus;
}

namespace ZHLN {

struct ActionId {
    uint64_t value = 0;

    constexpr ActionId() noexcept = default;
    constexpr explicit ActionId(uint64_t rawHash) noexcept : value(rawHash) {}
    constexpr ActionId(std::string_view actionName) noexcept : value(actionName.empty() ? 0ULL : Hash64(actionName)) {}
    constexpr ActionId(const char* actionName) noexcept
        : value((actionName == nullptr || actionName[0] == '\0') ? 0ULL : Hash64(std::string_view(actionName))) {}

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return value != 0;
    }

    constexpr auto operator==(const ActionId&) const noexcept -> bool = default;
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
    InputBindingKind kind            = InputBindingKind::None;
    uint8_t          code0           = 0;
    uint8_t          code1           = 0;
    uint8_t          code2           = 0;
    uint8_t          code3           = 0;
    KeyModifiers     modifiers       = KeyModifiers::None;
    bool             strictModifiers = false;
    bool             rawBypassUI     = false;
    float            scaleX          = 1.0f;
    float            scaleY          = 1.0f;
    float            threshold       = 0.50f;

    [[nodiscard]] static constexpr auto Key(KeyCode key, KeyModifiers requiredMods = KeyModifiers::None, float scale = 1.0f) noexcept
        -> InputBinding {
        return InputBinding {
            .kind            = InputBindingKind::Key,
            .code0           = static_cast<uint8_t>(key),
            .modifiers       = requiredMods,
            .strictModifiers = (requiredMods != KeyModifiers::None),
            .scaleX          = scale,
        };
    }

    [[nodiscard]] static constexpr auto KeyChord(KeyCode first, KeyCode second, KeyModifiers requiredMods = KeyModifiers::None) noexcept
        -> InputBinding {
        return InputBinding {
            .kind            = InputBindingKind::KeyChord,
            .code0           = static_cast<uint8_t>(first),
            .code1           = static_cast<uint8_t>(second),
            .modifiers       = requiredMods,
            .strictModifiers = (requiredMods != KeyModifiers::None),
        };
    }

    [[nodiscard]] static constexpr auto Mouse(MouseButton button, KeyModifiers requiredMods = KeyModifiers::None) noexcept -> InputBinding {
        return InputBinding {
            .kind            = InputBindingKind::MouseButton,
            .code0           = static_cast<uint8_t>(button),
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
            .kind   = InputBindingKind::GamepadButton,
            .code0  = static_cast<uint8_t>(button),
            .scaleX = scale,
        };
    }

    [[nodiscard]] static constexpr auto GamepadChord(GamepadButton first, GamepadButton second) noexcept -> InputBinding {
        return InputBinding {
            .kind  = InputBindingKind::GamepadChord,
            .code0 = static_cast<uint8_t>(first),
            .code1 = static_cast<uint8_t>(second),
        };
    }

    [[nodiscard]] static constexpr auto GamepadAxis1D(GamepadAxis axis, float scale = 1.0f, float actuationThreshold = 0.50f) noexcept
        -> InputBinding {
        return InputBinding {
            .kind      = InputBindingKind::GamepadAxis1D,
            .code0     = static_cast<uint8_t>(axis),
            .scaleX    = scale,
            .threshold = actuationThreshold,
        };
    }

    [[nodiscard]] static constexpr auto Composite1DKeys(KeyCode negative, KeyCode positive, float scale = 1.0f) noexcept -> InputBinding {
        return InputBinding {
            .kind   = InputBindingKind::Composite1DKeys,
            .code0  = static_cast<uint8_t>(negative),
            .code1  = static_cast<uint8_t>(positive),
            .scaleX = scale,
        };
    }

    [[nodiscard]] static constexpr auto Composite1DGamepad(GamepadButton negative, GamepadButton positive, float scale = 1.0f) noexcept
        -> InputBinding {
        return InputBinding {
            .kind   = InputBindingKind::Composite1DGamepad,
            .code0  = static_cast<uint8_t>(negative),
            .code1  = static_cast<uint8_t>(positive),
            .scaleX = scale,
        };
    }

    [[nodiscard]] static constexpr auto Composite2DKeys(KeyCode up, KeyCode down, KeyCode left, KeyCode right, float scale = 1.0f) noexcept
        -> InputBinding {
        return InputBinding {
            .kind   = InputBindingKind::Composite2DKeys,
            .code0  = static_cast<uint8_t>(up),
            .code1  = static_cast<uint8_t>(down),
            .code2  = static_cast<uint8_t>(left),
            .code3  = static_cast<uint8_t>(right),
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
            .kind   = InputBindingKind::Composite2DGamepad,
            .code0  = static_cast<uint8_t>(up),
            .code1  = static_cast<uint8_t>(down),
            .code2  = static_cast<uint8_t>(left),
            .code3  = static_cast<uint8_t>(right),
            .scaleX = scale,
            .scaleY = scale,
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
            .kind   = InputBindingKind::GamepadStick2D,
            .code0  = static_cast<uint8_t>(xAxis),
            .code1  = static_cast<uint8_t>(yAxis),
            .scaleX = scaleX,
            .scaleY = scaleY,
        };
    }

    [[nodiscard]] static constexpr auto LeftStick2D(float scaleX = 1.0f, float scaleY = 1.0f) noexcept -> InputBinding {
        return GamepadStick2D(GamepadAxis::LeftX, GamepadAxis::LeftY, scaleX, scaleY);
    }

    [[nodiscard]] static constexpr auto RightStick2D(float scaleX = 1.0f, float scaleY = 1.0f) noexcept -> InputBinding {
        return GamepadStick2D(GamepadAxis::RightX, GamepadAxis::RightY, scaleX, scaleY);
    }

    [[nodiscard]] constexpr auto WithBypassUICapture(bool bypass = true) const noexcept -> InputBinding {
        InputBinding copy = *this;
        copy.rawBypassUI  = bypass;
        return copy;
    }

    [[nodiscard]] constexpr auto WithStrictModifiers(bool strict = true) const noexcept -> InputBinding {
        InputBinding copy    = *this;
        copy.strictModifiers = strict;
        return copy;
    }

    [[nodiscard]] constexpr auto Specificity() const noexcept -> uint8_t {
        uint8_t score = 1;
        if (kind == InputBindingKind::KeyChord || kind == InputBindingKind::GamepadChord) {
            score += 2;
        }
        const auto modBits = static_cast<uint8_t>(modifiers);
        for (uint8_t b = 0; b < 4; ++b) {
            if ((modBits & (1U << b)) != 0) {
                ++score;
            }
        }
        return score;
    }

    constexpr auto operator==(const InputBinding&) const noexcept -> bool = default;
};

static_assert(std::is_trivially_copyable_v<InputBinding>);

class ZHLN_API InputAction {
  public:
    InputAction(std::string_view name, ActionKind kind) noexcept;

    auto Bind(const InputBinding& binding) -> InputAction&;
    auto SetClampUnitCircle(bool clamp) noexcept -> InputAction&;
    auto SetActuationThreshold(float threshold) noexcept -> InputAction&;
    void ClearBindings() noexcept;
    void SnapshotDefaults();
    void ResetToDefaults();

    [[nodiscard]] auto Id() const noexcept -> ActionId {
        return _id;
    }
    [[nodiscard]] auto Name() const noexcept -> std::string_view {
        return _name;
    }
    [[nodiscard]] auto Kind() const noexcept -> ActionKind {
        return _kind;
    }
    [[nodiscard]] auto ClampUnitCircle() const noexcept -> bool {
        return _clampUnitCircle;
    }
    [[nodiscard]] auto ActuationThreshold() const noexcept -> float {
        return _actuationThreshold;
    }
    [[nodiscard]] auto Bindings() const noexcept -> std::span<const InputBinding> {
        return _bindings;
    }
    [[nodiscard]] auto MutableBindings() noexcept -> std::span<InputBinding> {
        return _bindings;
    }

    auto Rebind(size_t slotIndex, const InputBinding& binding) -> bool;
    void AppendBinding(const InputBinding& binding);
    auto ReplaceBindings(std::span<const InputBinding> bindings) -> void;
    auto RemoveBinding(size_t slotIndex) -> bool;

  private:
    ActionId                  _id {};
    String64                  _name {};
    ActionKind                _kind               = ActionKind::Digital;
    bool                      _clampUnitCircle    = true;
    float                     _actuationThreshold = 0.50f;
    std::vector<InputBinding> _bindings;
    std::vector<InputBinding> _defaultBindings;
};

class ZHLN_API InputActionMap {
  public:
    static constexpr uint8_t kAllSeatsMask = static_cast<uint8_t>((1U << kMaxPlayerSeats) - 1U);

    InputActionMap(std::string_view name, int32_t priority = 0) noexcept;

    auto AddAction(std::string_view name, ActionKind kind = ActionKind::Digital) -> InputAction&;
    [[nodiscard]] auto FindAction(ActionId id) noexcept -> Optional<InputAction&>;
    [[nodiscard]] auto FindAction(ActionId id) const noexcept -> Optional<const InputAction&>;

    void SnapshotDefaults();
    void ResetToDefaults();

    [[nodiscard]] auto Name() const noexcept -> std::string_view {
        return _name;
    }
    [[nodiscard]] auto Id() const noexcept -> uint64_t {
        return _id;
    }
    [[nodiscard]] auto Priority() const noexcept -> int32_t {
        return _priority;
    }
    void SetPriority(int32_t priority) noexcept {
        _priority = priority;
    }

    [[nodiscard]] auto Enabled() const noexcept -> bool {
        return _enabled;
    }
    void SetEnabled(bool enabled) noexcept {
        _enabled = enabled;
    }

    [[nodiscard]] auto EnabledForSeat(PlayerSeat seat) const noexcept -> bool {
        return _enabled && seat < kMaxPlayerSeats && ((_seatMask & (1U << seat)) != 0U);
    }
    void SetEnabledForSeat(PlayerSeat seat, bool enabled) noexcept {
        if (seat >= kMaxPlayerSeats) {
            return;
        }
        if (enabled) {
            _seatMask = static_cast<uint8_t>(_seatMask | (1U << seat));
        } else {
            _seatMask = static_cast<uint8_t>(_seatMask & ~(1U << seat));
        }
    }
    [[nodiscard]] auto SeatMask() const noexcept -> uint8_t {
        return _seatMask;
    }
    void SetSeatMask(uint8_t mask) noexcept {
        _seatMask = static_cast<uint8_t>(mask & kAllSeatsMask);
    }

    [[nodiscard]] auto BlocksLowerPriority() const noexcept -> bool {
        return _blocksLowerPriority;
    }
    void SetBlocksLowerPriority(bool blocks) noexcept {
        _blocksLowerPriority = blocks;
    }

    [[nodiscard]] auto ConsumePhysicalInputs() const noexcept -> bool {
        return _consumePhysicalInputs;
    }
    void SetConsumePhysicalInputs(bool consume) noexcept {
        _consumePhysicalInputs = consume;
    }

    [[nodiscard]] auto Actions() const noexcept -> std::span<const InputAction> {
        return _actions;
    }
    [[nodiscard]] auto MutableActions() noexcept -> std::span<InputAction> {
        return _actions;
    }

  private:
    uint64_t                 _id                    = 0;
    String64                 _name {};
    int32_t                  _priority              = 0;
    uint8_t                  _seatMask              = kAllSeatsMask;
    bool                     _enabled               = true;
    bool                     _blocksLowerPriority   = false;
    bool                     _consumePhysicalInputs = true;
    std::vector<InputAction> _actions;
};

enum class BindingCaptureStatus : uint8_t {
    Idle = 0,
    Waiting,
    Captured,
    Cancelled,
};

enum class InputBindingParseError : uint8_t {
    InvalidHeader       ZHLN_ANNOTATION(ZHLN::Description<"Input bindings text header is missing or unsupported"> {})       = 1,
    MalformedLine       ZHLN_ANNOTATION(ZHLN::Description<"Input bindings line is malformed"> {}),
    UnknownBindingKind  ZHLN_ANNOTATION(ZHLN::Description<"Input bindings line names an unknown binding kind"> {}),
    UnknownEnumerator   ZHLN_ANNOTATION(ZHLN::Description<"Input bindings line names an unknown key, button, or axis"> {}),
    InvalidNumericValue ZHLN_ANNOTATION(ZHLN::Description<"Input bindings line contains an invalid numeric parameter"> {}),
};

using GamepadRumbleSinkFn = void (*)(void* userdata, uint8_t gamepadSlot, const GamepadRumble& rumble) noexcept;

// Core multi-seat input router, gamepad manager, haptics controller, and virtual action stack.
class ZHLN_API InputContext {
  public:
    InputContext();
    ~InputContext() = default;

    InputContext(const InputContext&)                    = delete;
    auto operator=(const InputContext&) -> InputContext& = delete;
    InputContext(InputContext&&) noexcept                = default;
    auto operator=(InputContext&&) noexcept -> InputContext& = default;

    // --- Frame Lifecycle & Raw Device Ingestion ------------------------------

    // Advances the frame boundary (snapshots previous physical and virtual action states).
    void BeginFrame() noexcept;

    void InjectKey(KeyCode key, bool down, PlayerSeat targetSeat = kPrimarySeat) noexcept;
    void InjectMouseMove(float x, float y, PlayerSeat targetSeat = kPrimarySeat) noexcept;
    void InjectMouseScroll(float delta, PlayerSeat targetSeat = kPrimarySeat) noexcept;
    void InjectChar(uint32_t codepoint, PlayerSeat targetSeat = kPrimarySeat) noexcept;
    void SyncCaptureFlags(bool wantCaptureKeyboard, bool wantCaptureMouse, PlayerSeat seat = kPrimarySeat) noexcept;
    void SyncFromInputStateComponent(const Components::InputStateComponent& component) noexcept;

    void InjectGamepadConnection(uint8_t slot, bool connected, std::string_view name = {}) noexcept;
    void InjectGamepadSample(uint8_t slot, const GamepadRawSample& sample) noexcept;
    void InjectGamepadButton(uint8_t slot, GamepadButton button, bool down) noexcept;
    void InjectGamepadAxis(uint8_t slot, GamepadAxis axis, float value) noexcept;

    // Evaluates all enabled action maps in priority order against the previous frame's
    // action snapshot and flushes queued `GamepadConnectionEvent`s to `eventBus` (if non-null).
    // Safe to call multiple times within the same frame (e.g., after `PollLateInput` or UI capture updates).
    void RefreshActionsForCurrentFrame(ECS::EventBus* eventBus = nullptr);

    // Steps active rumble timers by `dt` seconds and dispatches zero-rumble stops on expiry.
    void StepHaptics(float dt) noexcept;

    // Convenience helper combining `RefreshActionsForCurrentFrame(eventBus)` + `StepHaptics(dt)`.
    void EvaluateFrame(float dt, ECS::EventBus* eventBus = nullptr);

    // --- Multi-Seat & Device Routing -----------------------------------------

    void SetRoutingPolicy(SeatRoutingPolicy policy) noexcept;
    [[nodiscard]] auto RoutingPolicy() const noexcept -> SeatRoutingPolicy {
        return _routingPolicy;
    }

    void SetKeyboardMouseSeat(PlayerSeat seat, bool enabled) noexcept;
    [[nodiscard]] auto SeatReceivesKeyboardMouse(PlayerSeat seat) const noexcept -> bool;

    [[nodiscard]] auto SeatKeyboardMouse(PlayerSeat seat = kPrimarySeat) const noexcept -> const Components::InputStateComponent&;
    [[nodiscard]] auto MutableSeatKeyboardMouse(PlayerSeat seat = kPrimarySeat) noexcept -> Components::InputStateComponent&;

    auto AssignGamepadToSeat(uint8_t gamepadSlot, PlayerSeat seat) noexcept -> bool;
    void UnassignGamepad(uint8_t gamepadSlot) noexcept;
    [[nodiscard]] auto GamepadSeat(uint8_t gamepadSlot) const noexcept -> PlayerSeat;

    [[nodiscard]] auto Gamepad(uint8_t gamepadSlot) const noexcept -> Optional<const GamepadState&>;
    [[nodiscard]] auto MutableGamepad(uint8_t gamepadSlot) noexcept -> Optional<GamepadState&>;
    [[nodiscard]] auto PrimaryGamepadForSeat(PlayerSeat seat = kPrimarySeat) const noexcept -> Optional<const GamepadState&>;
    [[nodiscard]] auto ConnectedGamepadCount() const noexcept -> size_t;

    [[nodiscard]] auto LastActiveDevice(PlayerSeat seat = kPrimarySeat) const noexcept -> InputDeviceKind;

    void SetGamepadDeadzones(uint8_t gamepadSlot, const DeadzoneConfig& leftStick, const DeadzoneConfig& rightStick, float triggerDz = 0.05f) noexcept;
    void SetSeatDeadzones(PlayerSeat seat, const DeadzoneConfig& leftStick, const DeadzoneConfig& rightStick, float triggerDz = 0.05f) noexcept;

    // --- Gamepad Haptics / Rumble --------------------------------------------

    void SetRumbleSink(GamepadRumbleSinkFn sink, void* userdata = nullptr) noexcept;
    auto SetGamepadRumble(uint8_t gamepadSlot, const GamepadRumble& rumble) noexcept -> bool;
    auto RumbleSeat(PlayerSeat seat, const GamepadRumble& rumble) noexcept -> uint32_t;
    void StopRumble(uint8_t gamepadSlot) noexcept;
    void StopAllRumble() noexcept;
    [[nodiscard]] auto ActiveRumble(uint8_t gamepadSlot) const noexcept -> GamepadRumble;

    // --- Action Map Priority Stack -------------------------------------------

    auto CreateActionMap(std::string_view name, int32_t priority = 0) -> InputActionMap&;
    auto RemoveActionMap(std::string_view name) -> bool;
    [[nodiscard]] auto FindActionMap(std::string_view name) noexcept -> Optional<InputActionMap&>;
    [[nodiscard]] auto FindActionMap(std::string_view name) const noexcept -> Optional<const InputActionMap&>;
    [[nodiscard]] auto ActionMaps() const noexcept -> std::span<const InputActionMap> {
        return _actionMaps;
    }

    auto SetActionMapEnabled(std::string_view name, bool enabled) noexcept -> bool;
    auto SetActionMapEnabledForSeat(std::string_view name, PlayerSeat seat, bool enabled) noexcept -> bool;

    // --- Virtual Action Queries (Per-Seat) -----------------------------------

    [[nodiscard]] auto Action(ActionId id, PlayerSeat seat = kPrimarySeat) const noexcept -> ActionState;

    [[nodiscard]] auto ActionDown(ActionId id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool {
        return Action(id, seat).down;
    }

    [[nodiscard]] auto ActionPressed(ActionId id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool {
        return Action(id, seat).pressed;
    }

    [[nodiscard]] auto ActionHeld(ActionId id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool {
        return Action(id, seat).held;
    }

    [[nodiscard]] auto ActionReleased(ActionId id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool {
        return Action(id, seat).released;
    }

    [[nodiscard]] auto ActionAxis1D(ActionId id, PlayerSeat seat = kPrimarySeat) const noexcept -> float {
        return Action(id, seat).x;
    }

    [[nodiscard]] auto ActionAxis2D(ActionId id, PlayerSeat seat = kPrimarySeat) const noexcept -> Axis2DValue {
        return Action(id, seat).Value2D();
    }

    // --- Runtime Rebinding, Conflict Checks, Capture & Serialization ---------

    auto Rebind(std::string_view mapName, std::string_view actionName, size_t slotIndex, const InputBinding& binding) -> bool;
    auto AddBinding(std::string_view mapName, std::string_view actionName, const InputBinding& binding) -> bool;
    auto ClearBindings(std::string_view mapName, std::string_view actionName) -> bool;
    void SnapshotAllDefaults();
    auto ResetActionMapToDefaults(std::string_view mapName) -> bool;
    void ResetAllToDefaults();

    [[nodiscard]] auto
        FindConflictingActions(std::string_view mapName, const InputBinding& candidate, std::string_view ignoreAction = {}) const
        -> std::vector<ActionId>;

    void BeginBindingCapture(PlayerSeat seat = kPrimarySeat, bool captureAxes = false, KeyCode cancelKey = KeyCode::Escape) noexcept;
    void CancelBindingCapture() noexcept;
    [[nodiscard]] auto CaptureStatus() const noexcept -> BindingCaptureStatus {
        return _capture.status;
    }
    [[nodiscard]] auto TakeCapturedBinding() noexcept -> Optional<InputBinding>;

    [[nodiscard]] auto SerializeBindings() const -> std::string;
    [[nodiscard]] auto DeserializeBindings(std::string_view text) -> std::expected<void, ErrorCode>;

  private:
    struct EvaluatedActionEntry {
        ActionId    id {};
        ActionState previous {};
        ActionState current {};
    };

    struct SeatRuntime {
        Components::InputStateComponent kbm {};
        bool                            receivesKeyboardMouse = false;
        InputDeviceKind                 lastActiveDevice      = InputDeviceKind::KeyboardMouse;
        std::vector<EvaluatedActionEntry> actions;
    };

    struct RumbleChannel {
        GamepadRumble current {};
        float         remainingSeconds = 0.0f;
        bool          active           = false;
    };

    struct CaptureSession {
        BindingCaptureStatus   status      = BindingCaptureStatus::Idle;
        PlayerSeat             seat        = kPrimarySeat;
        bool                   captureAxes = false;
        KeyCode                cancelKey   = KeyCode::Escape;
        KeyCode                latchedMod  = KeyCode::Unknown;
        Optional<InputBinding> result {};
    };

    auto SelectAutoSeatForGamepad(uint8_t slot) const noexcept -> PlayerSeat;
    void UpdateCaptureSession() noexcept;
    void FlushPendingHotplugEvents(ECS::EventBus* eventBus);

    SeatRoutingPolicy                           _routingPolicy = SeatRoutingPolicy::SharePrimarySeat;
    Components::InputStateComponent             _nullKbm {};
    std::array<SeatRuntime, kMaxPlayerSeats>    _seats {};
    std::array<GamepadState, kMaxGamepads>      _gamepads {};
    std::array<RumbleChannel, kMaxGamepads>     _rumble {};
    std::vector<GamepadConnectionEvent>         _pendingHotplugEvents;
    std::vector<InputActionMap>                 _actionMaps;
    CaptureSession                              _capture {};
    GamepadRumbleSinkFn                         _rumbleSink         = nullptr;
    void*                                       _rumbleSinkUserdata = nullptr;
};

} // namespace ZHLN
