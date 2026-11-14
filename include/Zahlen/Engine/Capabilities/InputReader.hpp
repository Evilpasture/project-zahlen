// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Zahlen/Engine/Capabilities/InputReader.hpp
//
// The read-only input capability: what a frame's input *is*, never what it is
// made of.
//
// This is the only input type a gameplay system, an extension or an
// application callback should ever name. It answers from the engine's input
// router -- it never hands one out. Every query returns a value; there is no
// InputState& and no GamepadState& on this surface, because a borrowed mutable
// device state is a way to write input, and writing input is the engine's job
// (WindowInputReceiver, wired once in Engine::InitInternal).
//
// What is deliberately absent: the frame lifecycle (BeginFrame,
// RefreshActionsForCurrentFrame, EvaluateFrame), haptics stepping, resize
// bookkeeping, and every Inject* entry point. Hardware input has exactly one
// door, and it is not this one.

#include <Zahlen/Common.h>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Input/Input.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

class InputContext;

class ZHLN_API InputReader {
  public:
    // --- Virtual actions ----------------------------------------------------

    [[nodiscard]] auto Action(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> ActionState;
    [[nodiscard]] auto ActionDown(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;
    [[nodiscard]] auto ActionPressed(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;
    [[nodiscard]] auto ActionHeld(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;
    [[nodiscard]] auto ActionReleased(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;
    [[nodiscard]] auto ActionAxis1D(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> float;
    [[nodiscard]] auto ActionAxis2D(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> Axis2DValue;

    // --- Physical keyboard & mouse, by value ---------------------------------
    //
    // These answer for the *device*, not for a policy about who currently owns
    // it: a key that is down reports down even on a frame the GUI has captured
    // the keyboard. That is the same answer the old
    // `SeatKeyboardMouse().IsKeyDown(...)` gave, so migrating a call site does
    // not silently change when a scene reacts. UiCapture() is here for a caller
    // that wants to gate on it, and the router applies it itself when it
    // evaluates actions -- which is where it actually matters.

    [[nodiscard]] auto KeyDown(KeyCode key, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;
    [[nodiscard]] auto KeyPressed(KeyCode key, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;
    [[nodiscard]] auto KeyHeld(KeyCode key, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;
    [[nodiscard]] auto KeyReleased(KeyCode key, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;

    [[nodiscard]] auto MouseButtonDown(MouseButton button, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;
    [[nodiscard]] auto MouseButtonPressed(MouseButton button, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;
    [[nodiscard]] auto MouseButtonHeld(MouseButton button, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;
    [[nodiscard]] auto MouseButtonReleased(MouseButton button, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;

    [[nodiscard]] auto Modifiers(PlayerSeat seat = kPrimarySeat) const noexcept -> KeyModifiers;
    [[nodiscard]] auto HasModifiers(KeyModifiers required, bool exact = true, PlayerSeat seat = kPrimarySeat) const noexcept -> bool;
    [[nodiscard]] auto MousePosition(PlayerSeat seat = kPrimarySeat) const noexcept -> Axis2DValue;
    [[nodiscard]] auto MouseDelta(PlayerSeat seat = kPrimarySeat) const noexcept -> Axis2DValue;
    [[nodiscard]] auto MouseWheel(PlayerSeat seat = kPrimarySeat) const noexcept -> float;
    [[nodiscard]] auto UiCapture(PlayerSeat seat = kPrimarySeat) const noexcept -> UiCaptureState;
    [[nodiscard]] auto LastActiveDevice(PlayerSeat seat = kPrimarySeat) const noexcept -> InputDeviceKind;

    // A copy of one seat's whole physical state, taken now.
    //
    // This exists for the one shape the per-query accessors cannot serve: a tool
    // that hands its input to a pure function taking `const InputState&` (picking
    // rays, an orbit camera, an inspector's drag math). It is a *copy* -- the
    // struct is trivially copyable and a few hundred bytes -- so it neither aliases
    // the router nor outlives the frame in any way that matters. Gameplay should
    // prefer the named queries: they say what a system reads.
    [[nodiscard]] auto Snapshot(PlayerSeat seat = kPrimarySeat) const noexcept -> InputState;

    // --- Gamepads, by value --------------------------------------------------

    [[nodiscard]] auto ConnectedGamepadCount() const noexcept -> size_t;
    [[nodiscard]] auto GamepadConnected(GamepadSlot gamepadSlot) const noexcept -> bool;
    [[nodiscard]] auto GamepadSeat(GamepadSlot gamepadSlot) const noexcept -> PlayerSeat;
    [[nodiscard]] auto GamepadButtonDown(GamepadSlot gamepadSlot, GamepadButton button) const noexcept -> bool;
    [[nodiscard]] auto GamepadButtonPressed(GamepadSlot gamepadSlot, GamepadButton button) const noexcept -> bool;
    [[nodiscard]] auto GamepadButtonReleased(GamepadSlot gamepadSlot, GamepadButton button) const noexcept -> bool;
    [[nodiscard]] auto GamepadAxisValue(GamepadSlot gamepadSlot, GamepadAxis axis) const noexcept -> float;
    [[nodiscard]] auto GamepadLeftStick(GamepadSlot gamepadSlot) const noexcept -> StickSample;
    [[nodiscard]] auto GamepadRightStick(GamepadSlot gamepadSlot) const noexcept -> StickSample;
    [[nodiscard]] auto GamepadLeftTrigger(GamepadSlot gamepadSlot) const noexcept -> float;
    [[nodiscard]] auto GamepadRightTrigger(GamepadSlot gamepadSlot) const noexcept -> float;

    // --- Read-only configuration & status ------------------------------------

    [[nodiscard]] auto HasActionMap(std::string_view mapName) const noexcept -> bool;
    [[nodiscard]] auto IsActionMapEnabled(std::string_view mapName) const noexcept -> bool;
    [[nodiscard]] auto RoutingPolicy() const noexcept -> SeatRoutingPolicy;
    [[nodiscard]] auto SeatReceivesKeyboardMouse(PlayerSeat seat) const noexcept -> bool;
    [[nodiscard]] auto ActiveRumble(GamepadSlot gamepadSlot) const noexcept -> GamepadRumble;
    [[nodiscard]] auto CaptureStatus() const noexcept -> BindingCaptureStatus;
    [[nodiscard]] auto SerializeBindings() const -> std::string;

  private:
    friend class Engine;
    friend class InputActions;
    explicit InputReader(const InputContext* context) noexcept: _context(context) {
    }

    const InputContext* _context;
};

} // namespace ZHLN
