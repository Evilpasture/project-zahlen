// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IActionContext.hpp
//
// Input as the public API sees it: action-map setup, raw device events in,
// virtual actions and physical state out. Configuration descriptors
// (ActionMapConfig, ActionConfig, InputBinding, ActionRebindConfig,
// InputRoutingConfig) travel via reflected ChainInNode / ChainOutNode lists so
// future input options extend by chaining rather than changing vtable slots.

#include <Zahlen/COM/COMTypes.hpp>
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
    // --- Frame lifecycle ----------------------------------------------------

    // Snapshots the previous physical and virtual state. Everything injected
    // after this call belongs to the new frame.
    virtual void BeginFrame() noexcept = 0;

    // Re-evaluates every enabled action map against what has been injected so
    // far and flushes queued gamepad hotplug events to the engine's event bus.
    // Safe to call more than once in a frame.
    virtual void RefreshActions() noexcept = 0;

    // RefreshActions plus haptics stepping, which is what a frame normally
    // wants.
    virtual void EvaluateFrame(float deltaSeconds) noexcept = 0;

    // --- Raw device ingestion -----------------------------------------------

    virtual void InjectKey(KeyCode key, bool down, PlayerSeat seat) noexcept = 0;
    virtual void InjectMouseMove(float x, float y, PlayerSeat seat) noexcept = 0;
    virtual void InjectMouseRelative(float x, float y, float dx, float dy, PlayerSeat seat) noexcept = 0;
    virtual void InjectMouseScroll(float delta, PlayerSeat seat) noexcept = 0;

    virtual void InjectGamepadConnection(GamepadSlot slot, bool connected, StringView name) noexcept = 0;
    virtual void InjectGamepadButton(GamepadSlot slot, GamepadButton button, bool down) noexcept = 0;
    virtual void InjectGamepadAxis(GamepadSlot slot, GamepadAxis axis, float value) noexcept = 0;

    // --- Action-map & binding setup -----------------------------------------

    // Creates or updates an action map. Supported node: ActionMapConfig.
    [[nodiscard]] virtual Result<void> ConfigureActionMap(const ChainInNode* mapChain) noexcept = 0;
    virtual bool                       RemoveActionMap(StringView mapName) noexcept = 0;
    [[nodiscard]] virtual bool         HasActionMap(StringView mapName) const noexcept = 0;
    [[nodiscard]] virtual bool         IsActionMapEnabled(StringView mapName) const noexcept = 0;
    virtual bool                       SetActionMapEnabled(StringView mapName, bool enabled) noexcept = 0;
    virtual bool                       SetActionMapEnabledForSeat(StringView mapName, PlayerSeat seat,
                                                                  bool enabled) noexcept = 0;

    // Creates or updates an action within a map and returns its ActionID.
    // Supported node: ActionConfig.
    [[nodiscard]] virtual Result<ActionID> ConfigureAction(const ChainInNode* actionChain) noexcept = 0;

    // Appends or rebinds a binding on an existing action.
    // Supported nodes: InputBinding (append) or ActionRebindConfig (replace slot).
    [[nodiscard]] virtual Result<void> BindAction(StringView mapName, ActionID action,
                                                  const ChainInNode* bindingChain) noexcept = 0;
    virtual bool                       ClearBindings(StringView mapName, ActionID action) noexcept = 0;

    virtual void SnapshotDefaults() noexcept = 0;
    virtual bool ResetActionMapToDefaults(StringView mapName) noexcept = 0;
    virtual void ResetAllToDefaults() noexcept = 0;

    // Multi-seat routing configuration. Supported node: InputRoutingConfig.
    [[nodiscard]] virtual Result<void> ConfigureRouting(const ChainInNode* routingChain) noexcept = 0;
    [[nodiscard]] virtual Result<void> GetRouting(ChainOutNode* routingChain) const noexcept = 0;

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

    // --- Ergonomics & derived reads: not in the vtable ----------------------

    [[nodiscard]] auto CreateActionMap(StringView name, int32_t priority = 0, bool enabled = true) noexcept
        -> Result<void> {
        return ConfigureActionMap(InChain(ActionMapConfig {
            .name     = String64(std::string_view(name)),
            .priority = priority,
            .enabled  = enabled,
        }));
    }

    [[nodiscard]] auto AddAction(StringView mapName, StringView actionName, ActionKind kind = ActionKind::Digital,
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

    [[nodiscard]] auto AddBinding(StringView mapName, ActionID action, const InputBinding& binding) noexcept
        -> Result<void> {
        return BindAction(mapName, action, InChain(binding));
    }

    [[nodiscard]] auto Rebind(StringView mapName, ActionID action, uint32_t slotIndex,
                              const InputBinding& binding) noexcept -> Result<void> {
        return BindAction(mapName, action, InChain(ActionRebindConfig {.slotIndex = slotIndex, .binding = binding}));
    }

    [[nodiscard]] auto SetRoutingPolicy(SeatRoutingPolicy policy) noexcept -> Result<void> {
        InputRoutingConfig current {};
        (void)GetRouting(OutChain(current));
        current.policy = policy;
        return ConfigureRouting(InChain(current));
    }

    [[nodiscard]] auto RoutingPolicy() const noexcept -> SeatRoutingPolicy {
        InputRoutingConfig current {};
        (void)GetRouting(OutChain(current));
        return current.policy;
    }

    [[nodiscard]] auto SetKeyboardMouseSeat(PlayerSeat seat, bool enabled) noexcept -> Result<void> {
        if (!seat.IsValid()) return ComError::InvalidArgument;
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
