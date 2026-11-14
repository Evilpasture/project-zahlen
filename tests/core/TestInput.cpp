// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EventBus.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <cmath>
#include <limits>
#include <vector>

// Pure value mechanics only.
//
// InputState, GamepadState, DeadzoneConfig, InputBinding specificity and
// ActionState are structs with no owner and no engine: they are exercised here
// directly, which is the whole reason the vocabulary is separate from the
// router. Anything that needs a live router (maps, rebinding, capture,
// serialization, routing) lives in TestInputLive.cpp and goes through the
// public engine capabilities -- there is no Inject* seam on this side of the
// boundary and there will not be one.

namespace ZHLN::Test {

namespace {
[[nodiscard]] auto Near(float a, float b, float eps = 1e-4f) noexcept -> bool {
    return std::abs(a - b) <= eps;
}

// Negative compile-time pins: InputState accepts typed KeyCode / MouseButton only, never raw integers.
// Dependent template parameters ensure SFINAE / concept evaluation on both GCC and Clang.
template <typename Arg>
concept InputStateAcceptsSetKey = requires(InputState s, Arg a) { s.SetKey(a, true); };
template <typename Arg>
concept InputStateAcceptsKeyQueries =
    requires(const InputState s, Arg a) {
        s.IsKeyDown(a);
        s.IsKeyPressed(a);
        s.IsKeyHeld(a);
        s.IsKeyReleased(a);
        s.IsKeyDown(a);
        s.IsKeyPressed(a);
    };
template <typename Arg>
concept InputStateAcceptsMouseQueries =
    requires(const InputState s, Arg a) {
        s.IsMouseButtonDown(a);
        s.IsMouseButtonPressed(a);
        s.IsMouseButtonHeld(a);
        s.IsMouseButtonReleased(a);
        s.IsMouseButtonDown(a);
    };

static_assert(InputStateAcceptsSetKey<KeyCode>);
static_assert(!InputStateAcceptsSetKey<uint8_t>);
static_assert(InputStateAcceptsKeyQueries<KeyCode>);
static_assert(!InputStateAcceptsKeyQueries<uint8_t>);
static_assert(InputStateAcceptsMouseQueries<MouseButton>);
static_assert(!InputStateAcceptsMouseQueries<uint8_t>);
static_assert(!InputStateAcceptsMouseQueries<KeyCode>);
} // namespace

void test_input_state_component_edges_and_subframe_taps() {
    InputState state {};

    // Frame 1: rising edge on W + accumulated mouse deltas + side button X1.
    state.BeginFrame();
    state.SetKey(KeyCode::W, true);
    state.SetMouseButton(MouseButton::X1, true);
    state.QueueKeyPress(KeyCode::Enter);
    state.QueueChar(U'Z');
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
    ExpectEq(state.queuedInputCount, size_t {2});
    Expect(!state.queuedInput[0].isChar && state.queuedInput[0].key == KeyCode::Enter);
    Expect(state.queuedInput[1].isChar && state.queuedInput[1].codepoint == U'Z');
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

    const UiCaptureState captured {true, false};
    Expect(!state.IsKeyDown(KeyCode::S, captured));
    Expect(!state.IsKeyPressed(KeyCode::S, captured));
    Expect(state.IsKeyDown(KeyCode::S));
    Expect(state.IsKeyPressed(KeyCode::S));
    Expect(state.Modifiers() == (KeyModifiers::Ctrl | KeyModifiers::Shift));
    Expect(state.Modifiers(captured) == KeyModifiers::None);
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

void test_binding_specificity_and_action_state_values() {
    // Specificity is what resolves two bindings on one axis of the same action:
    // a chord beats a single key, and each required modifier adds one. It is
    // pure arithmetic on the descriptor, so it is pinned here rather than
    // through a graph.
    ExpectEq(InputBinding::Key(KeyCode::R).Specificity(), 1u);
    ExpectEq(InputBinding::Key(KeyCode::R, KeyModifiers::Ctrl).Specificity(), 2u);
    ExpectEq(InputBinding::Key(KeyCode::R, KeyModifiers::Ctrl | KeyModifiers::Shift).Specificity(), 3u);
    ExpectEq(InputBinding::KeyChord(KeyCode::LControl, KeyCode::R).Specificity(), 3u);
    ExpectEq(InputBinding::GamepadChord(GamepadButton::LeftBumper, GamepadButton::RightBumper).Specificity(), 3u);
    Expect(InputBinding::Key(KeyCode::R, KeyModifiers::Ctrl).Specificity() > InputBinding::Key(KeyCode::R).Specificity());

    // ActionState is a plain value: the derived accessors must agree with the
    // fields, because every capability query is one of these projections.
    ActionState state {
        .x            = 0.75f,
        .y            = -0.25f,
        .down         = true,
        .pressed      = true,
        .held         = false,
        .released     = false,
        .activeDevice = InputDeviceKind::Gamepad,
    };
    Expect(Near(state.Value1D(), 0.75f));
    const Axis2DValue as2D = state.Value2D();
    Expect(Near(as2D.x, 0.75f));
    Expect(Near(as2D.y, -0.25f));
    Expect(state.Down() && state.Pressed() && !state.Held() && !state.Released());
    Expect(state.activeDevice == InputDeviceKind::Gamepad);

    // A default-constructed state is "not actuated", which is what the router
    // reports for an action no enabled map defines.
    const ActionState idle {};
    Expect(!idle.Down() && !idle.Pressed());
    Expect(Near(idle.x, 0.0f) && Near(idle.y, 0.0f));

    // ActionID hashes its name, so the same spelling resolves to the same id in
    // an authoring call and in a query -- and an absent name is invalid.
    Expect(ActionID("Jump") == ActionID(std::string_view("Jump")));
    Expect(ActionID("Jump") != ActionID("Jump2"));
    Expect(!ActionID("").Valid());
    Expect(ActionID("Jump").Valid());

    // BindingCaptureStatus / InputBindingParseError live in ActionValues.hpp now:
    // naming them must not require the router. If this compiles, that holds.
    constexpr BindingCaptureStatus captureState = BindingCaptureStatus::Idle;
    static_assert(captureState == BindingCaptureStatus::Idle);
}

} // namespace ZHLN::Test
