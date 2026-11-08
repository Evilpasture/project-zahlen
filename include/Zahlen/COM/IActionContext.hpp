// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IActionContext.hpp
//
// Input as the public API sees it: raw device events in, virtual actions out.
// Both halves are here deliberately. A consumer that owns its own window loop
// needs to feed the router, and a consumer that only reads actions needs the
// queries -- splitting them would leave neither side able to prove the other
// works.
//
// GamepadState does not cross. It holds std::bitset, whose layout libstdc++
// builds out of a base class, so it is not standard layout and the ABI audit
// is right to refuse it. The scalar queries below answer the same questions
// without shipping a container across the line.
//
// The derived reads -- down, pressed, held, released, 1D and 2D axes -- are
// non-virtual inline wrappers over the single Action() dispatch, exactly as
// the InputContext facade spells them. Six fewer vtable slots, and the caller
// pays one virtual call instead of one per question.

#include <Zahlen/COM/COMTypes.hpp>
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

    virtual void InjectGamepadConnection(uint8_t slot, bool connected, StringView name) noexcept = 0;
    virtual void InjectGamepadButton(uint8_t slot, GamepadButton button, bool down) noexcept = 0;
    virtual void InjectGamepadAxis(uint8_t slot, GamepadAxis axis, float value) noexcept = 0;

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
    [[nodiscard]] virtual bool       GamepadConnected(uint8_t slot) const noexcept = 0;
    [[nodiscard]] virtual bool       GamepadButtonDown(uint8_t slot, GamepadButton button) const noexcept = 0;
    [[nodiscard]] virtual float      GamepadAxisValue(uint8_t slot, GamepadAxis axis) const noexcept = 0;
    [[nodiscard]] virtual PlayerSeat GamepadSeat(uint8_t slot) const noexcept = 0;
    virtual bool                     AssignGamepadToSeat(uint8_t slot, PlayerSeat seat) noexcept = 0;

    // --- Derived reads: not in the vtable -----------------------------------

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
