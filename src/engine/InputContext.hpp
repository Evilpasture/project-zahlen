// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/InputContext.hpp
//
// The input router's storage. Engine-private on purpose: this header used to be
// include/Zahlen/Input/InputContext.hpp, which made InputActionMap&,
// InputAction&, InputState& and every Inject* entry point part of the engine's
// public API. Anything a caller actually needs is a value, and values live in
// <Zahlen/Input/Input.hpp> and <Zahlen/Input/ActionValues.hpp>.
//
// Exactly two translation units include this file:
//
//   * Engine.cpp      -- lifecycle and OS wiring: the WindowInputReceiver
//                        callbacks, the rumble sink, the frame boundary.
//   * Capabilities.cpp -- the bodies of InputReader / InputHaptics /
//                        InputActions, which are the only shapes the router is
//                        ever seen in from outside.
//
// No ZHLN_API appears anywhere below. Nothing here is exported, so nothing here
// can be named by a consumer that links libzahlen_engine.

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
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

class InputAction {
  public:
    InputAction(std::string_view name, ActionKind kind) noexcept;

    auto Bind(const InputBinding& binding) -> InputAction&;
    auto SetClampUnitCircle(bool clamp) noexcept -> InputAction&;
    auto SetActuationThreshold(float threshold) noexcept -> InputAction&;
    void ClearBindings() noexcept;
    void SnapshotDefaults();
    void ResetToDefaults();

    [[nodiscard]] auto Id() const noexcept -> ActionID {
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
    ActionID                  _id {};
    String64                  _name {};
    ActionKind                _kind               = ActionKind::Digital;
    bool                      _clampUnitCircle    = true;
    float                     _actuationThreshold = 0.50f;
    std::vector<InputBinding> _bindings;
    std::vector<InputBinding> _defaultBindings;
};

class InputActionMap {
  public:
    static constexpr SeatMask kAllSeatsMask = SeatMask::All();

    InputActionMap(std::string_view name, int32_t priority = 0) noexcept;

    auto AddAction(std::string_view name, ActionKind kind = ActionKind::Digital) -> InputAction&;
    [[nodiscard]] auto FindAction(ActionID id) noexcept -> Optional<InputAction&>;
    [[nodiscard]] auto FindAction(ActionID id) const noexcept -> Optional<const InputAction&>;

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
        return _enabled && _seatMask.Contains(seat);
    }
    void SetEnabledForSeat(PlayerSeat seat, bool enabled) noexcept {
        if (!seat.IsValid()) {
            return;
        }
        if (enabled) {
            _seatMask.Set(seat);
        } else {
            _seatMask.Clear(seat);
        }
    }
    [[nodiscard]] auto seatMask() const noexcept -> SeatMask {
        return _seatMask;
    }
    void SetSeatMask(SeatMask mask) noexcept {
        _seatMask = SeatMask {static_cast<uint8_t>(mask.bits & kAllSeatsMask.bits)};
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
    SeatMask                 _seatMask              = kAllSeatsMask;
    bool                     _enabled               = true;
    bool                     _blocksLowerPriority   = false;
    bool                     _consumePhysicalInputs = true;
    std::vector<InputAction> _actions;
};

using GamepadRumbleSinkFn = void (*)(void* userdata, GamepadSlot gamepadSlot, const GamepadRumble& rumble) noexcept;

// Core multi-seat input router, gamepad manager, haptics controller, and virtual action stack.
class InputContext {
  public:
    InputContext();
    ~InputContext() = default;

    InputContext(const InputContext&)                    = delete;
    auto operator=(const InputContext&) -> InputContext& = delete;
    InputContext(InputContext&&) noexcept                = default;
    auto operator=(InputContext&&) noexcept -> InputContext& = default;

    // --- Frame Lifecycle & Hardware Device Ingestion -----------------------

    // Advances the frame boundary (snapshots previous physical and virtual action states).
    void BeginFrame() noexcept;

    void InjectKey(KeyCode key, bool down, PlayerSeat targetSeat = kPrimarySeat) noexcept;
    void InjectMouseButton(MouseButton button, bool down, PlayerSeat targetSeat = kPrimarySeat) noexcept;
    void InjectMouseMove(float x, float y, PlayerSeat targetSeat = kPrimarySeat) noexcept;
    void InjectMouseRelative(float x, float y, float dx, float dy, PlayerSeat targetSeat = kPrimarySeat) noexcept;
    void InjectMouseScroll(float delta, PlayerSeat targetSeat = kPrimarySeat) noexcept;
    void InjectChar(uint32_t codepoint, PlayerSeat targetSeat = kPrimarySeat) noexcept;
    void InjectResize(Extent2D extent) noexcept;
    [[nodiscard]] auto ConsumePendingResize() noexcept -> Optional<Extent2D>;
    [[nodiscard]] auto HasPendingResize() const noexcept -> bool;
    void SyncCaptureFlags(bool wantCaptureKeyboard, bool wantCaptureMouse, PlayerSeat seat = kPrimarySeat) noexcept;

    void InjectGamepadConnection(GamepadSlot slot, bool connected, std::string_view name = {}) noexcept;
    void InjectGamepadSample(GamepadSlot slot, const GamepadHardwareSample& sample) noexcept;
    void InjectGamepadButton(GamepadSlot slot, GamepadButton button, bool down) noexcept;
    void InjectGamepadAxis(GamepadSlot slot, GamepadAxis axis, float value) noexcept;

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

    [[nodiscard]] auto SeatKeyboardMouse(PlayerSeat seat = kPrimarySeat) const noexcept -> const InputState&;
    [[nodiscard]] auto MutableSeatKeyboardMouse(PlayerSeat seat = kPrimarySeat) noexcept -> InputState&;
    [[nodiscard]] auto UiCapture(PlayerSeat seat = kPrimarySeat) const noexcept -> UiCaptureState;

    auto AssignGamepadToSeat(GamepadSlot gamepadSlot, PlayerSeat seat) noexcept -> bool;
    void UnassignGamepad(GamepadSlot gamepadSlot) noexcept;
    [[nodiscard]] auto GamepadSeat(GamepadSlot gamepadSlot) const noexcept -> PlayerSeat;

    [[nodiscard]] auto Gamepad(GamepadSlot gamepadSlot) const noexcept -> Optional<const GamepadState&>;
    [[nodiscard]] auto MutableGamepad(GamepadSlot gamepadSlot) noexcept -> Optional<GamepadState&>;
    [[nodiscard]] auto PrimaryGamepadForSeat(PlayerSeat seat = kPrimarySeat) const noexcept -> Optional<const GamepadState&>;
    [[nodiscard]] auto ConnectedGamepadCount() const noexcept -> size_t;

    [[nodiscard]] auto LastActiveDevice(PlayerSeat seat = kPrimarySeat) const noexcept -> InputDeviceKind;

    void SetGamepadDeadzones(GamepadSlot gamepadSlot, const DeadzoneConfig& leftStick, const DeadzoneConfig& rightStick, float triggerDz = 0.05f) noexcept;
    void SetSeatDeadzones(PlayerSeat seat, const DeadzoneConfig& leftStick, const DeadzoneConfig& rightStick, float triggerDz = 0.05f) noexcept;

    // --- Gamepad Haptics / Rumble --------------------------------------------

    void SetRumbleSink(GamepadRumbleSinkFn sink, void* userdata = nullptr) noexcept;
    auto SetGamepadRumble(GamepadSlot gamepadSlot, const GamepadRumble& rumble) noexcept -> bool;
    auto RumbleSeat(PlayerSeat seat, const GamepadRumble& rumble) noexcept -> uint32_t;
    void StopRumble(GamepadSlot gamepadSlot) noexcept;
    void StopAllRumble() noexcept;
    [[nodiscard]] auto ActiveRumble(GamepadSlot gamepadSlot) const noexcept -> GamepadRumble;

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

    [[nodiscard]] auto Action(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> ActionState;

    [[nodiscard]] auto ActionDown(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool {
        return Action(id, seat).down;
    }

    [[nodiscard]] auto ActionPressed(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool {
        return Action(id, seat).pressed;
    }

    [[nodiscard]] auto ActionHeld(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool {
        return Action(id, seat).held;
    }

    [[nodiscard]] auto ActionReleased(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool {
        return Action(id, seat).released;
    }

    [[nodiscard]] auto ActionAxis1D(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> float {
        return Action(id, seat).x;
    }

    [[nodiscard]] auto ActionAxis2D(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> Axis2DValue {
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
        -> std::vector<ActionID>;

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
        ActionID    id {};
        ActionState previous {};
        ActionState current {};
    };

    struct SeatRuntime {
        InputState                      kbm {};
        UiCaptureState                uiCapture {};
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

    auto SelectAutoSeatForGamepad(GamepadSlot slot) const noexcept -> PlayerSeat;
    void UpdateCaptureSession() noexcept;
    void FlushPendingHotplugEvents(ECS::EventBus* eventBus);

    SeatRoutingPolicy                           _routingPolicy = SeatRoutingPolicy::SharePrimarySeat;
    InputState                                  _nullKbm {};
    Optional<Extent2D>                          _pendingResize {};
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
