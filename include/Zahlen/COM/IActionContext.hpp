// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IActionContext.hpp
//
// Input as the COM API sees it: action-map setup, virtual actions and physical
// state out. Configuration descriptors (ActionMapConfig, ActionConfig,
// InputBinding, ActionRebindConfig, InputRoutingConfig) travel via reflected
// ChainInNode / ChainOutNode lists so future input options extend by chaining
// rather than changing vtable slots.
//
// What is NOT here, and will not be: BeginFrame / RefreshActions /
// EvaluateFrame, and every Inject*. Device events arrive from the PlatformHost
// during IEngine::ProcessEvents(), and evaluation happens inside IEngine::Tick().
// A host that wants input to happen must run the frame, not poke the router --
// otherwise the two entry points race and the order of a frame is a property of
// the host instead of the engine.

#include <Zahlen/COM/Types.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Input/Input.hpp>

#include <cstdint>
#include <type_traits>

namespace ZHLN::COM {

static_assert(std::is_standard_layout_v<ActionID> && std::is_trivially_copyable_v<ActionID>);
static_assert(std::is_standard_layout_v<ActionState> && std::is_trivially_copyable_v<ActionState>);
static_assert(std::is_standard_layout_v<Axis2DValue> && std::is_trivially_copyable_v<Axis2DValue>);

struct IActionContext : IObject {
    // --- Action-map & binding setup -----------------------------------------

    // Creates or updates an action map. Supported node: ActionMapConfig.
    virtual Result<void> ConfigureActionMap(const ChainInNode* mapChain) noexcept = 0;
    virtual bool                       RemoveActionMap(StringView mapName) noexcept = 0;
    [[nodiscard]] virtual bool         HasActionMap(StringView mapName) const noexcept = 0;
    [[nodiscard]] virtual bool         IsActionMapEnabled(StringView mapName) const noexcept = 0;
    virtual bool                       SetActionMapEnabled(StringView mapName, bool enabled) noexcept = 0;
    virtual bool                       SetActionMapEnabledForSeat(StringView mapName, PlayerSeat seat,
                                                                  bool enabled) noexcept = 0;

    // Creates or updates an action within a map and returns its ActionID.
    // Supported node: ActionConfig.
    virtual Result<ActionID> ConfigureAction(const ChainInNode* actionChain) noexcept = 0;

    // Appends or rebinds a binding on an existing action.
    // Supported nodes: InputBinding (append) or ActionRebindConfig (replace slot).
    virtual Result<void> BindAction(StringView mapName, ActionID action,
                                                  const ChainInNode* bindingChain) noexcept = 0;
    virtual bool                       ClearBindings(StringView mapName, ActionID action) noexcept = 0;

    virtual void SnapshotDefaults() noexcept = 0;
    virtual bool ResetActionMapToDefaults(StringView mapName) noexcept = 0;
    virtual void ResetAllToDefaults() noexcept = 0;

    // Multi-seat routing configuration. Supported node: InputRoutingConfig.
    virtual Result<void> ConfigureRouting(const ChainInNode* routingChain) noexcept = 0;
    virtual Result<void> GetRouting(ChainOutNode* routingChain) const noexcept = 0;

    // --- Virtual actions ----------------------------------------------------

    // A zeroed state for an action no enabled map defines, which is the same
    // answer the facade gives: an unbound action is simply not actuated.
    [[nodiscard]] virtual ActionState Action(ActionID id, PlayerSeat seat) const noexcept = 0;

    // --- Physical state -----------------------------------------------------

    [[nodiscard]] virtual bool KeyDown(KeyCode key, PlayerSeat seat) const noexcept = 0;
    [[nodiscard]] virtual bool KeyPressed(KeyCode key, PlayerSeat seat) const noexcept = 0;
    [[nodiscard]] virtual bool KeyReleased(KeyCode key, PlayerSeat seat) const noexcept = 0;

    [[nodiscard]] virtual Axis2DValue MousePosition(PlayerSeat seat) const noexcept = 0;
    [[nodiscard]] virtual Axis2DValue MouseDelta(PlayerSeat seat) const noexcept = 0;
    [[nodiscard]] virtual float       MouseWheel(PlayerSeat seat) const noexcept = 0;

    [[nodiscard]] virtual InputDeviceKind LastActiveDevice(PlayerSeat seat) const noexcept = 0;

    [[nodiscard]] virtual uint32_t   ConnectedGamepadCount() const noexcept = 0;
    [[nodiscard]] virtual bool       GamepadConnected(GamepadSlot slot) const noexcept = 0;
    [[nodiscard]] virtual bool       GamepadButtonDown(GamepadSlot slot, GamepadButton button) const noexcept = 0;
    [[nodiscard]] virtual float      GamepadAxisValue(GamepadSlot slot, GamepadAxis axis) const noexcept = 0;
    [[nodiscard]] virtual PlayerSeat GamepadSeat(GamepadSlot slot) const noexcept = 0;
    virtual bool                     AssignGamepadToSeat(GamepadSlot slot, PlayerSeat seat) noexcept = 0;

    // Physical-state parity. These read devices even while UI owns capture.
    [[nodiscard]] virtual bool MouseButtonDown(MouseButton button, PlayerSeat seat) const noexcept = 0;
    [[nodiscard]] virtual bool MouseButtonPressed(MouseButton button, PlayerSeat seat) const noexcept = 0;
    [[nodiscard]] virtual bool MouseButtonReleased(MouseButton button, PlayerSeat seat) const noexcept = 0;
    [[nodiscard]] virtual KeyModifiers Modifiers(PlayerSeat seat) const noexcept = 0;
    [[nodiscard]] virtual bool HasModifiers(KeyModifiers required, bool exact, PlayerSeat seat) const noexcept = 0;
    [[nodiscard]] virtual bool GamepadButtonPressed(GamepadSlot slot, GamepadButton button) const noexcept = 0;
    [[nodiscard]] virtual bool GamepadButtonReleased(GamepadSlot slot, GamepadButton button) const noexcept = 0;
    [[nodiscard]] virtual StickSample GamepadLeftStick(GamepadSlot slot) const noexcept = 0;
    [[nodiscard]] virtual StickSample GamepadRightStick(GamepadSlot slot) const noexcept = 0;
    [[nodiscard]] virtual float GamepadLeftTrigger(GamepadSlot slot) const noexcept = 0;
    [[nodiscard]] virtual float GamepadRightTrigger(GamepadSlot slot) const noexcept = 0;

    virtual bool SetGamepadRumble(GamepadSlot slot, const GamepadRumble& rumble) noexcept = 0;
    virtual uint32_t RumbleSeat(PlayerSeat seat, const GamepadRumble& rumble) noexcept = 0;
    virtual void StopRumble(GamepadSlot slot) noexcept = 0;
    virtual void StopAllRumble() noexcept = 0;

    virtual void BeginBindingCapture(PlayerSeat seat, bool captureAxes, KeyCode cancelKey) noexcept = 0;
    virtual void CancelBindingCapture() noexcept = 0;
    [[nodiscard]] virtual Option<InputBinding> TakeCapturedBinding() noexcept = 0;
    [[nodiscard]] virtual BindingCaptureStatus CaptureStatus() const noexcept = 0;

    // Returns required byte count, excluding any terminator. Null/zero queries
    // size. A short buffer is left untouched; pass the returned size to retry.
    // Successful copies are exact bytes with no trailing NUL. No allocator or
    // std::string crosses the boundary. Null with nonzero capacity is invalid.
    virtual Result<uint32_t> SerializeBindings(char* buffer, uint32_t capacity) const noexcept = 0;
    virtual Result<void> DeserializeBindings(StringView text) noexcept = 0;

    // --- Ergonomics & derived reads: not in the vtable ----------------------

    auto CreateActionMap(StringView name, int32_t priority = 0, bool enabled = true) noexcept
        -> Result<void> {
        return ConfigureActionMap(InChain(ActionMapConfig {
            .name     = String64(std::string_view(name)),
            .priority = priority,
            .enabled  = enabled,
        }));
    }

    auto AddAction(StringView mapName, StringView actionName, ActionKind kind = ActionKind::Digital,
                                 Span<const InputBinding> bindings = {}) noexcept -> Result<ActionID> {
        return ConfigureAction(InChain(ActionConfig {
            .mapName         = String64(std::string_view(mapName)),
            .actionName      = String64(std::string_view(actionName)),
            .kind            = kind,
            .replaceBindings = true,
            .bindings        = bindings.data,
            .bindingCount    = static_cast<uint32_t>(bindings.size),
        }));
    }

    auto AddBinding(StringView mapName, ActionID action, const InputBinding& binding) noexcept
        -> Result<void> {
        return BindAction(mapName, action, InChain(binding));
    }

    auto Rebind(StringView mapName, ActionID action, uint32_t slotIndex,
                              const InputBinding& binding) noexcept -> Result<void> {
        return BindAction(mapName, action, InChain(ActionRebindConfig {.slotIndex = slotIndex, .binding = binding}));
    }

    void SetRoutingPolicy(SeatRoutingPolicy policy) noexcept {
        InputRoutingConfig current {};
        (void)GetRouting(OutChain(current));
        current.policy = policy;
        (void)ConfigureRouting(InChain(current));
    }

    [[nodiscard]] auto RoutingPolicy() const noexcept -> SeatRoutingPolicy {
        InputRoutingConfig current {};
        (void)GetRouting(OutChain(current));
        return current.policy;
    }

    auto SetKeyboardMouseSeat(PlayerSeat seat, bool enabled) noexcept -> Result<void> {
        if (!seat.IsValid()) return Error::InvalidArgument;
        InputRoutingConfig current {};
        (void)GetRouting(OutChain(current));
        if (enabled) {
            current.keyboardMouseSeatMask.Set(seat);
        } else {
            current.keyboardMouseSeatMask.Clear(seat);
        }
        return ConfigureRouting(InChain(current));
    }

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

protected:
    ~IActionContext() = default;
};

} // namespace ZHLN::COM
