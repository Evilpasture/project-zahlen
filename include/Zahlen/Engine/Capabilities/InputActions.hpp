// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Zahlen/Engine/Capabilities/InputActions.hpp
//
// Authoring input: maps, actions, bindings, seats, deadzones, capture and the
// persisted binding text. This is what a settings screen and a game's startup
// composition need, and it is deliberately *more* than a gameplay system
// should hold -- which is why the simulation graph provides InputReader and
// InputHaptics instead (see EngineServices.hpp).
//
// Nothing here returns a live object. There is no `CreateActionMap -> InputActionMap&`
// and no `AddAction -> InputAction&`, because a reference to the router's storage
// outlives the call that produced it and turns an authoring API into an aliasing
// one. Every mutation is a named operation; every read is a value.

#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Engine/Capabilities/InputHaptics.hpp>
#include <Zahlen/Engine/Capabilities/InputReader.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

class InputContext;

class ZHLN_API InputActions {
  public:
    // --- Action maps ---------------------------------------------------------

    // Creates the map, or returns the existing one's id when the name is taken.
    auto CreateActionMap(std::string_view name, int32_t priority = 0, bool enabled = true) noexcept -> bool;
    // The whole descriptor in one call, for a settings screen that loaded one.
    auto ConfigureActionMap(const ActionMapConfig& config) noexcept -> bool;
    auto RemoveActionMap(std::string_view name) noexcept -> bool;
    auto SetActionMapEnabled(std::string_view name, bool enabled) noexcept -> bool;
    auto SetActionMapEnabledForSeat(std::string_view name, PlayerSeat seat, bool enabled) noexcept -> bool;
    auto SetActionMapBlocksLowerPriority(std::string_view name, bool blocks) noexcept -> bool;
    auto SetActionMapConsumePhysicalInputs(std::string_view name, bool consume) noexcept -> bool;

    // --- Actions & bindings --------------------------------------------------

    [[nodiscard]] auto AddAction(std::string_view mapName, std::string_view actionName, ActionKind kind = ActionKind::Digital,
                                 bool clampUnitCircle = true, float actuationThreshold = 0.50f) noexcept -> ActionID;
    // Creates or updates an action from a descriptor. Returns a null ActionID
    // when the map does not exist or the descriptor is inconsistent.
    [[nodiscard]] auto ConfigureAction(const ActionConfig& config) noexcept -> ActionID;

    auto AddBinding(std::string_view mapName, std::string_view actionName, const InputBinding& binding) noexcept -> bool;
    auto Rebind(std::string_view mapName, std::string_view actionName, size_t slotIndex, const InputBinding& binding) noexcept -> bool;
    auto ClearBindings(std::string_view mapName, std::string_view actionName) noexcept -> bool;
    auto RemoveBinding(std::string_view mapName, std::string_view actionName, size_t slotIndex) noexcept -> bool;

    // The same four operations addressed by ActionID.
    //
    // An action's identity in the router *is* the hash of its name, so these are
    // the same lookups; they exist because a caller that already holds an ID --
    // the COM surface returns one from ConfigureAction, and a rebinding UI stores
    // one -- should not have to keep the spelling around to use it again.
    //
    // They are named ...Action rather than overloading the four above, and that
    // is not cosmetic: ActionID converts implicitly from a string (that is what
    // makes `ActionID("Jump")` readable), so `AddBinding("map", "jump", b)` would
    // be ambiguous between the two overload sets at every call site.
    auto BindAction(std::string_view mapName, ActionID action, const InputBinding& binding) noexcept -> bool;
    auto RebindAction(std::string_view mapName, ActionID action, size_t slotIndex, const InputBinding& binding) noexcept -> bool;
    auto ClearActionBindings(std::string_view mapName, ActionID action) noexcept -> bool;
    auto RemoveActionBinding(std::string_view mapName, ActionID action, size_t slotIndex) noexcept -> bool;

    // An owned copy: the caller may hold it across frames and the engine may
    // reallocate its storage in between.
    [[nodiscard]] auto BindingsFor(std::string_view mapName, std::string_view actionName) const -> std::vector<InputBinding>;
    [[nodiscard]] auto BindingsForAction(std::string_view mapName, ActionID action) const -> std::vector<InputBinding>;

    // --- Defaults, conflicts, capture, persistence ---------------------------

    void SnapshotAllDefaults();
    auto ResetActionMapToDefaults(std::string_view mapName) noexcept -> bool;
    void ResetAllToDefaults();

    [[nodiscard]] auto FindConflictingActions(std::string_view mapName, const InputBinding& candidate, std::string_view ignoreAction = {}) const
        -> std::vector<ActionID>;

    void BeginBindingCapture(PlayerSeat seat = kPrimarySeat, bool captureAxes = false, KeyCode cancelKey = KeyCode::Escape) noexcept;
    void CancelBindingCapture() noexcept;
    [[nodiscard]] auto TakeCapturedBinding() noexcept -> Optional<InputBinding>;

    [[nodiscard]] auto SerializeBindings() const -> std::string;
    [[nodiscard]] auto DeserializeBindings(std::string_view text) -> std::expected<void, ErrorCode>;

    // --- Routing & deadzones -------------------------------------------------

    void SetRoutingPolicy(SeatRoutingPolicy policy) noexcept;
    void SetKeyboardMouseSeat(PlayerSeat seat, bool enabled) noexcept;
    auto AssignGamepadToSeat(GamepadSlot gamepadSlot, PlayerSeat seat) noexcept -> bool;
    void UnassignGamepad(GamepadSlot gamepadSlot) noexcept;
    void SetGamepadDeadzones(GamepadSlot gamepadSlot, const DeadzoneConfig& leftStick, const DeadzoneConfig& rightStick,
                             float triggerDz = 0.05f) noexcept;
    void SetSeatDeadzones(PlayerSeat seat, const DeadzoneConfig& leftStick, const DeadzoneConfig& rightStick, float triggerDz = 0.05f) noexcept;

    // --- Sub-views & forwarding ----------------------------------------------
    //
    // A settings screen holds InputActions and needs to read what it is editing
    // and pulse a pad when a binding lands. Forcing it to hold three capability
    // values for that would be ceremony, so the read and haptic surfaces are
    // also reachable through here. The reverse is not true: InputReader cannot
    // reach InputActions, which is the whole point of splitting them.

    [[nodiscard]] auto Reader() const noexcept -> InputReader;
    [[nodiscard]] auto Haptics() noexcept -> InputHaptics;

    // --- Forwarded reads -----------------------------------------------------

    [[nodiscard]] auto Action(ActionID id, PlayerSeat seat = kPrimarySeat) const noexcept -> ActionState;
    [[nodiscard]] auto HasActionMap(std::string_view mapName) const noexcept -> bool;
    [[nodiscard]] auto IsActionMapEnabled(std::string_view mapName) const noexcept -> bool;
    [[nodiscard]] auto RoutingPolicy() const noexcept -> SeatRoutingPolicy;
    [[nodiscard]] auto SeatReceivesKeyboardMouse(PlayerSeat seat) const noexcept -> bool;
    [[nodiscard]] auto CaptureStatus() const noexcept -> BindingCaptureStatus;
    [[nodiscard]] auto ConnectedGamepadCount() const noexcept -> size_t;

    // --- Forwarded haptics ---------------------------------------------------

    [[nodiscard]] auto SetGamepadRumble(GamepadSlot gamepadSlot, const GamepadRumble& rumble) noexcept -> bool;
    auto               RumbleSeat(PlayerSeat seat, const GamepadRumble& rumble) noexcept -> uint32_t;
    void               StopRumble(GamepadSlot gamepadSlot) noexcept;
    void               StopAllRumble() noexcept;

  private:
    friend class Engine;
    explicit InputActions(InputContext* context) noexcept: _context(context) {
    }

    InputContext* _context;
};

} // namespace ZHLN
