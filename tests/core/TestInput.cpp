// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/InputContext.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EventBus.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <cmath>
#include <limits>
#include <vector>

namespace ZHLN::Test {

namespace {
[[nodiscard]] auto Near(float a, float b, float eps = 1e-4f) noexcept -> bool {
    return std::abs(a - b) <= eps;
}
} // namespace

struct InputGraphServices {
    InputContext&  input;
    ECS::EventBus& events;
};

struct ObservedInputSummary {
    uint32_t connectedEvents = 0;
    bool     jumpPressed     = false;
    float    moveX           = 0.0f;
};

inline void ObserveInputSystem(
    ECS::Res<InputContext>                   input,
    ECS::EventReader<GamepadConnectionEvent> hotplugEvents,
    ECS::Query<ObservedInputSummary&>        query
) {
    uint32_t connects = 0;
    for (const GamepadConnectionEvent& ev : hotplugEvents) {
        if (ev.connected) {
            ++connects;
        }
    }
    const bool        jumped = input->ActionPressed("Jump");
    const Axis2DValue move   = input->ActionAxis2D("Move");
    query.ForEach([&](Entity, ObservedInputSummary& out) {
        out.connectedEvents += connects;
        out.jumpPressed = jumped;
        out.moveX       = move.x;
    });
}

void test_input_state_component_edges_and_subframe_taps() {
    Components::InputStateComponent state {};

    // Frame 1: rising edge on W + accumulated mouse deltas + side button X1.
    state.BeginFrame();
    state.SetKey(KeyCode::W, true);
    state.SetKey(ToKeyCode(MouseButton::X1), true);
    state.ApplyLocalMotion(100.0f, 200.0f);
    state.ApplyLocalMotion(112.0f, 195.0f);
    state.ApplyLocalMotion(120.0f, 190.0f);
    state.ApplyWheel(1.0f);
    state.ApplyWheel(0.5f);

    Expect(state.IsKeyDown(KeyCode::W));
    Expect(state.IsKeyPressed(KeyCode::W));
    Expect(!state.IsKeyHeld(KeyCode::W));
    Expect(!state.IsKeyReleased(KeyCode::W));
    Expect(state.IsMouseButtonDown(MouseButton::X1));
    Expect(state.IsMouseButtonPressed(MouseButton::X1));
    Expect(Near(state.GetMouseDeltaX(), 20.0f));
    Expect(Near(state.GetMouseDeltaY(), -10.0f));
    Expect(Near(state.GetMouseWheel(), 1.5f));

    // Frame 2: OS key-repeat sends SetKey(W, true) again while already held -> Held, NOT Pressed.
    state.BeginFrame();
    state.SetKey(KeyCode::W, true);
    Expect(state.IsKeyDown(KeyCode::W));
    Expect(!state.IsKeyPressed(KeyCode::W));
    Expect(state.IsKeyHeld(KeyCode::W));
    Expect(!state.IsKeyReleased(KeyCode::W));
    Expect(Near(state.GetMouseDeltaX(), 0.0f));
    Expect(Near(state.GetMouseWheel(), 0.0f));

    // Frame 3: release W + sub-frame tap on E (both down and up arrive inside the same poll).
    state.BeginFrame();
    state.SetKey(KeyCode::W, false);
    state.SetKey(KeyCode::E, true);
    state.SetKey(KeyCode::E, false);

    Expect(!state.IsKeyDown(KeyCode::W));
    Expect(state.IsKeyReleased(KeyCode::W));
    Expect(!state.IsKeyDown(KeyCode::E));
    Expect(state.IsKeyPressed(KeyCode::E));
    Expect(state.IsKeyReleased(KeyCode::E));

    // UI capture suppresses non-raw queries while preserving Raw queries, and tracks modifiers.
    state.BeginFrame();
    state.SetKey(KeyCode::LControl, true);
    state.SetKey(KeyCode::RShift, true);
    state.SetKey(KeyCode::S, true);
    Expect(state.HasModifiers(KeyModifiers::Ctrl | KeyModifiers::Shift));
    Expect(!state.HasModifiers(KeyModifiers::Ctrl, /*exact=*/true));
    Expect(state.HasModifiers(KeyModifiers::Ctrl, /*exact=*/false));

    state.wantCaptureKeyboard = true;
    Expect(!state.IsKeyDown(KeyCode::S));
    Expect(!state.IsKeyPressed(KeyCode::S));
    Expect(state.IsKeyDownRaw(KeyCode::S));
    Expect(state.IsKeyPressedRaw(KeyCode::S));
    Expect(state.Modifiers() == KeyModifiers::None);
    Expect(state.ModifiersRaw() == (KeyModifiers::Ctrl | KeyModifiers::Shift));
}

void test_gamepad_deadzone_curves_and_trigger_edges() {
    // Below inner radial deadzone -> exact zero.
    const DeadzoneConfig radial {.mode = DeadzoneMode::ScaledRadial, .inner = 0.20f, .outer = 0.90f, .curveExponent = 1.0f};
    const StickSample    quiet = ApplyStickDeadzone(0.10f, 0.10f, radial);
    Expect(Near(quiet.x, 0.0f, 1e-5f));
    Expect(Near(quiet.y, 0.0f, 1e-5f));
    Expect(Near(quiet.magnitude, 0.0f, 1e-5f));

    // Midpoint (0.55 on X is halfway between 0.20 and 0.90) -> 0.50 linear, 0.25 quadratic.
    const StickSample halfLinear = ApplyStickDeadzone(0.55f, 0.0f, radial);
    Expect(Near(halfLinear.x, 0.50f));
    Expect(Near(halfLinear.magnitude, 0.50f));

    const DeadzoneConfig quadratic {.mode = DeadzoneMode::ScaledRadial, .inner = 0.20f, .outer = 0.90f, .curveExponent = 2.0f};
    const StickSample    halfQuad = ApplyStickDeadzone(0.55f, 0.0f, quadratic);
    Expect(Near(halfQuad.x, 0.25f));

    // Diagonal preservation in ScaledRadial and outer clamping to unit circle.
    const StickSample diagFull = ApplyStickDeadzone(0.95f, 0.95f, radial);
    Expect(Near(diagFull.magnitude, 1.0f));
    Expect(Near(diagFull.x, diagFull.y));

    // Axial mode zeroes small cross-axis jitter independently, Hybrid gates bow-tie drift.
    const DeadzoneConfig axial {.mode = DeadzoneMode::Axial, .inner = 0.20f, .outer = 1.0f, .curveExponent = 1.0f};
    const StickSample    axialSample = ApplyStickDeadzone(0.60f, 0.15f, axial);
    Expect(Near(axialSample.x, 0.50f));
    Expect(Near(axialSample.y, 0.0f, 1e-5f));

    const DeadzoneConfig hybrid {.mode = DeadzoneMode::Hybrid, .inner = 0.20f, .outer = 1.0f, .curveExponent = 1.0f};
    const StickSample    hybridSample = ApplyStickDeadzone(0.60f, 0.03f, hybrid);
    Expect(Near(hybridSample.y, 0.0f, 1e-5f));
    Expect(hybridSample.x > 0.45f);

    // NaN / Inf safety.
    const StickSample nanSample = ApplyStickDeadzone(std::numeric_limits<float>::quiet_NaN(), 0.5f, radial);
    Expect(Near(nanSample.magnitude, 0.0f, 1e-6f));

    // Trigger deadzone + digital actuation edges on GamepadState.
    GamepadState pad {};
    pad.connected = true;
    pad.BeginFrame();
    pad.SetAxis(GamepadAxis::RightTrigger, 0.75f);
    Expect(pad.RightTrigger() > 0.70f);
    Expect(pad.IsTriggerDown(GamepadAxis::RightTrigger));
    Expect(pad.IsTriggerPressed(GamepadAxis::RightTrigger));

    pad.BeginFrame();
    pad.SetAxis(GamepadAxis::RightTrigger, 0.80f);
    Expect(pad.IsTriggerDown(GamepadAxis::RightTrigger));
    Expect(!pad.IsTriggerPressed(GamepadAxis::RightTrigger));

    pad.BeginFrame();
    pad.SetAxis(GamepadAxis::RightTrigger, 0.0f);
    Expect(!pad.IsTriggerDown(GamepadAxis::RightTrigger));
    Expect(pad.IsTriggerReleased(GamepadAxis::RightTrigger));
}

struct RumbleRecord {
    uint8_t       slot = 0;
    GamepadRumble rumble {};
};

void test_multiseat_routing_hotplug_events_and_haptics() {
    InputContext  input;
    ECS::EventBus bus;

    std::vector<RumbleRecord> rumbleLog;
    input.SetRumbleSink(
        [](void* ud, uint8_t slot, const GamepadRumble& r) noexcept {
            static_cast<std::vector<RumbleRecord>*>(ud)->push_back(RumbleRecord {.slot = slot, .rumble = r});
        },
        &rumbleLog
    );

    // DedicatedGamepadSeats: Seat 0 owns KB/M, so Gamepad 0 claims Seat 1 and Gamepad 1 claims Seat 2.
    input.SetRoutingPolicy(SeatRoutingPolicy::DedicatedGamepadSeats);
    input.BeginFrame();
    input.InjectGamepadConnection(0, true, "DualSense Wireless");
    input.InjectGamepadConnection(1, true, "Xbox Series Controller");
    input.RefreshActionsForCurrentFrame(&bus);

    ExpectEq(input.ConnectedGamepadCount(), size_t {2});
    ExpectEq(input.GamepadSeat(0), PlayerSeat {1});
    ExpectEq(input.GamepadSeat(1), PlayerSeat {2});
    ExpectEq(bus.Reader<GamepadConnectionEvent>().Size(), size_t {2});

    // Rumble Seat 1 for 0.20s, step 0.10s (still active), step another 0.15s (auto-expires & sends zero stop).
    const uint32_t rumbledCount = input.RumbleSeat(1, GamepadRumble {.lowFrequency = 0.8f, .highFrequency = 0.4f, .durationSeconds = 0.20f});
    ExpectEq(rumbledCount, 1u);
    ExpectEq(rumbleLog.size(), size_t {1});
    Expect(Near(input.ActiveRumble(0).lowFrequency, 0.8f));

    input.StepHaptics(0.10f);
    Expect(Near(input.ActiveRumble(0).lowFrequency, 0.8f));
    ExpectEq(rumbleLog.size(), size_t {1});

    input.StepHaptics(0.15f);
    Expect(input.ActiveRumble(0).IsZero());
    ExpectEq(rumbleLog.size(), size_t {2});
    Expect(rumbleLog.back().rumble.IsZero());

    // Disconnect while rumbling immediately stops rumble and emits a disconnect event.
    bus.ClearEvents();
    Expect(input.SetGamepadRumble(1, GamepadRumble {.lowFrequency = 1.0f, .durationSeconds = 1.0f}));
    ExpectEq(rumbleLog.size(), size_t {3});

    input.BeginFrame();
    input.InjectGamepadConnection(1, false);
    input.RefreshActionsForCurrentFrame(&bus);
    ExpectEq(rumbleLog.size(), size_t {4});
    Expect(rumbleLog.back().slot == 1 && rumbleLog.back().rumble.IsZero());

    const auto reader = bus.Reader<GamepadConnectionEvent>();
    ExpectEq(reader.Size(), size_t {1});
    Expect(!reader.View()[0].connected);
    ExpectEq(reader.View()[0].assignedSeat, PlayerSeat {2});
}

void test_virtual_actions_context_priority_chords_and_composites() {
    InputContext input;
    // SharePrimarySeat by default: Seat 0 gets KB/M + Gamepad 0; Seat 1 gets Gamepad 1.
    input.InjectGamepadConnection(0, true, "Pad0");
    input.InjectGamepadConnection(1, true, "Pad1");

    auto& gameplay = input.CreateActionMap("Gameplay", /*priority=*/0);
    gameplay.AddAction("Move", ActionKind::Axis2D)
        .Bind(InputBinding::WASD())
        .Bind(InputBinding::LeftStick2D());
    gameplay.AddAction("Jump", ActionKind::Digital)
        .Bind(InputBinding::Key(KeyCode::Space))
        .Bind(InputBinding::GamepadBtn(GamepadButton::South));
    gameplay.AddAction("QuickSave", ActionKind::Digital)
        .Bind(InputBinding::Key(KeyCode::S, KeyModifiers::Ctrl));
    gameplay.AddAction("Ultimate", ActionKind::Digital)
        .Bind(InputBinding::GamepadChord(GamepadButton::LeftBumper, GamepadButton::RightBumper));
    gameplay.AddAction("ShoulderPing", ActionKind::Digital)
        .Bind(InputBinding::GamepadBtn(GamepadButton::LeftBumper));

    auto& pauseMenu = input.CreateActionMap("PauseMenu", /*priority=*/100);
    pauseMenu.SetEnabled(false);
    pauseMenu.SetBlocksLowerPriority(true);
    pauseMenu.AddAction("Confirm", ActionKind::Digital)
        .Bind(InputBinding::Key(KeyCode::Space))
        .Bind(InputBinding::GamepadBtn(GamepadButton::South));

    // Frame 1: Diagonal W+D on Seat 0 normalizes to unit length; partial stick on Seat 1 preserves ~0.53 magnitude.
    input.BeginFrame();
    input.InjectKey(KeyCode::W, true);
    input.InjectKey(KeyCode::D, true);
    input.InjectKey(KeyCode::Space, true);
    input.InjectGamepadAxis(1, GamepadAxis::LeftX, 0.57f);
    input.RefreshActionsForCurrentFrame();

    const Axis2DValue move0 = input.ActionAxis2D("Move", 0);
    Expect(Near(move0.Length(), 1.0f));
    Expect(move0.x > 0.70f && move0.y > 0.70f);
    Expect(input.ActionPressed("Jump", 0));
    ExpectEq(input.LastActiveDevice(0), InputDeviceKind::KeyboardMouse);

    const Axis2DValue move1 = input.ActionAxis2D("Move", 1);
    Expect(move1.x > 0.45f && move1.x < 0.65f);
    ExpectEq(input.LastActiveDevice(1), InputDeviceKind::Gamepad);

    // Frame 2: Enable PauseMenu ONLY for Seat 0 -> Seat 0's held Gameplay Jump transitions cleanly to Released,
    // Confirm fires in PauseMenu, while Seat 1 continues unblocked in Gameplay!
    input.BeginFrame();
    pauseMenu.SetEnabled(true);
    pauseMenu.SetSeatMask(1U << 0U);
    input.InjectGamepadButton(1, GamepadButton::South, true);
    input.RefreshActionsForCurrentFrame();

    Expect(!input.ActionDown("Jump", 0));
    Expect(input.ActionReleased("Jump", 0));
    Expect(input.ActionDown("Confirm", 0));
    Expect(input.ActionPressed("Jump", 1));

    // Frame 3: Disable PauseMenu; test chord specificity preemption (`Ctrl+S` fires QuickSave AND consumes `S`
    // so `Move` does NOT walk backward; `LB+RB` fires `Ultimate` and consumes `LB` before `ShoulderPing`).
    pauseMenu.SetEnabled(false);
    input.BeginFrame();
    input.InjectKey(KeyCode::W, false);
    input.InjectKey(KeyCode::D, false);
    input.InjectKey(KeyCode::Space, false);
    input.InjectKey(KeyCode::LControl, true);
    input.InjectKey(KeyCode::S, true);
    input.InjectGamepadButton(0, GamepadButton::LeftBumper, true);
    input.InjectGamepadButton(0, GamepadButton::RightBumper, true);
    input.RefreshActionsForCurrentFrame();

    Expect(input.ActionPressed("QuickSave", 0));
    Expect(Near(input.ActionAxis2D("Move", 0).y, 0.0f, 1e-5f));
    Expect(input.ActionPressed("Ultimate", 0));
    Expect(!input.ActionDown("ShoulderPing", 0));

    // Strict modifiers: adding Shift (`Ctrl+Shift+S`) must NOT trigger `Ctrl+S` QuickSave.
    input.BeginFrame();
    input.InjectKey(KeyCode::LShift, true);
    input.RefreshActionsForCurrentFrame();
    Expect(!input.ActionDown("QuickSave", 0));
}

void test_runtime_rebinding_conflict_detection_capture_and_serialization() {
    InputContext input;
    input.InjectGamepadConnection(0, true, "Pad0");

    auto& map = input.CreateActionMap("Combat", 10);
    map.AddAction("Fire", ActionKind::Digital).Bind(InputBinding::Mouse(MouseButton::Left));
    map.AddAction("Reload", ActionKind::Digital).Bind(InputBinding::Key(KeyCode::R));
    map.AddAction("Dash", ActionKind::Digital).Bind(InputBinding::Key(KeyCode::LShift));
    input.SnapshotAllDefaults();

    // Conflict detection finds Reload when probing Key(R), unless Reload itself is ignored.
    const auto conflicts = input.FindConflictingActions("Combat", InputBinding::Key(KeyCode::R));
    ExpectEq(conflicts.size(), size_t {1});
    Expect(conflicts[0] == ActionId("Reload"));
    Expect(input.FindConflictingActions("Combat", InputBinding::Key(KeyCode::R), "Reload").empty());

    // Interactive binding capture:
    // 1) Cancel via Escape.
    input.BeginBindingCapture(kPrimarySeat);
    input.BeginFrame();
    input.InjectKey(KeyCode::Escape, true);
    input.RefreshActionsForCurrentFrame();
    Expect(input.CaptureStatus() == BindingCaptureStatus::Cancelled);

    // 2) Capture modifier chord `Ctrl + G`.
    input.BeginFrame();
    input.InjectKey(KeyCode::Escape, false);
    input.RefreshActionsForCurrentFrame();

    input.BeginBindingCapture(kPrimarySeat);
    input.BeginFrame();
    input.InjectKey(KeyCode::LControl, true);
    input.RefreshActionsForCurrentFrame();
    Expect(input.CaptureStatus() == BindingCaptureStatus::Waiting);

    input.BeginFrame();
    input.InjectKey(KeyCode::G, true);
    input.RefreshActionsForCurrentFrame();
    Expect(input.CaptureStatus() == BindingCaptureStatus::Captured);
    const auto capturedChord = input.TakeCapturedBinding();
    Expect(capturedChord.has_value());
    Expect(*capturedChord == InputBinding::Key(KeyCode::G, KeyModifiers::Ctrl));

    // Apply captured rebinding to Reload, serialize to text, reset defaults, then deserialize back!
    Expect(input.Rebind("Combat", "Reload", 0, *capturedChord));
    const std::string serialized = input.SerializeBindings();
    Expect(!serialized.empty());

    input.ResetAllToDefaults();
    Expect(map.FindAction("Reload")->Bindings()[0] == InputBinding::Key(KeyCode::R));

    InputContext restored;
    const auto   loadRes = restored.DeserializeBindings(serialized);
    Expect(loadRes.has_value());
    const auto restoredMap = restored.FindActionMap("Combat");
    Expect(restoredMap.has_value());
    ExpectEq(restoredMap->Priority(), 10);
    const auto restoredReload = restoredMap->FindAction("Reload");
    Expect(restoredReload.has_value());
    ExpectEq(restoredReload->Bindings().size(), size_t {1});
    Expect(restoredReload->Bindings()[0] == InputBinding::Key(KeyCode::G, KeyModifiers::Ctrl));

    // Reject malformed header cleanly via ErrorCode without exceptions.
    const auto badHeader = restored.DeserializeBindings("NOT_A_HEADER\n");
    Expect(!badHeader.has_value());
    Expect(badHeader.error().Is(InputBindingParseError::InvalidHeader));
}

void test_ecs_system_graph_input_context_service_resolution() {
    ECS::Registry reg;
    InputContext  input;
    ECS::EventBus events;

    auto& map = input.CreateActionMap("Gameplay", 0);
    map.AddAction("Jump", ActionKind::Digital).Bind(InputBinding::Key(KeyCode::Space));
    map.AddAction("Move", ActionKind::Axis2D).Bind(InputBinding::WASD());

    const Entity observer = reg.Create(ObservedInputSummary {});

    InputGraphServices                   services {.input = input, .events = events};
    ECS::SystemGraph<InputGraphServices> graph(reg, services);
    graph.AddSystem<&ObserveInputSystem>();
    graph.Compile();

    input.BeginFrame();
    input.InjectGamepadConnection(0, true, "TestPad");
    input.InjectKey(KeyCode::Space, true);
    input.InjectKey(KeyCode::D, true);
    input.RefreshActionsForCurrentFrame(&events);

    graph.Execute(Frame {.frame = 1, .alpha = 1.0f, .dt = 1.0f / 60.0f, .scratch = nullptr});

    const auto summary = reg.Get<ObservedInputSummary>(observer);
    Expect(summary.has_value());
    ExpectEq(summary->connectedEvents, 1u);
    Expect(summary->jumpPressed);
    Expect(Near(summary->moveX, 1.0f));
}

} // namespace ZHLN::Test
