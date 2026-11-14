// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// The router, exercised through the public capabilities only.
//
// What this file is for: everything in the old TestInput.cpp that needed a live
// InputContext -- authoring maps, conflict detection, rebinding, defaults,
// routing configuration and binding serialization -- is still tested, but
// against `Engine::Input()` (authoring) and `Engine::Input() const` (reading).
//
// What this file deliberately does NOT do: inject input. There is no Inject* on
// Engine, on InputActions, on InputReader, or on the COM boundary, and this test
// will not acquire one. Live OS delivery is what the windowing tests exercise;
// the thing under test here is the authoring and the value plumbing, which does
// not need a device to be true.
//
// Hardware-independent by design: every case runs headless, because none of it
// touches a swapchain, a pad or a cursor.

#include "TestsFramework.hpp"
#include <Zahlen/Engine/Capabilities/InputActions.hpp>
#include <Zahlen/Engine/Capabilities/InputHaptics.hpp>
#include <Zahlen/Engine/Capabilities/InputReader.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/GUI/GUI.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EventBus.hpp>

#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Test {

namespace {

[[nodiscard]] auto Near(float a, float b, float eps = 1e-4f) noexcept -> bool {
    return std::abs(a - b) <= eps;
}

// A headless engine, with nothing installed beyond core. Everything below is
// authoring, so no module, scene or graphics device is involved.
[[nodiscard]] auto MakeHeadlessEngine() -> std::unique_ptr<Engine> {
    auto created = Engine::Create(EngineConfig {.appName = "InputCapabilityTest", .validationMode = ValidationMode::Off, .headless = true});
    Expect(created.has_value());
    if (!created) {
        return nullptr;
    }
    return std::move(*created);
}

// One map with the bindings the old router test used, authored entirely through
// value calls. There is no `map.AddAction(...).Bind(...)` chain anywhere any
// more: an action is created by name and a binding is added by name, because
// holding a reference into the router's storage was the thing being removed.
auto AuthorCombatMap(InputActions actions) -> void {
    Expect(actions.CreateActionMap("Combat", 10));
    ExpectEq(actions.AddAction("Combat", "Fire", ActionKind::Digital).Valid(), true);
    ExpectEq(actions.AddAction("Combat", "Reload", ActionKind::Digital).Valid(), true);
    ExpectEq(actions.AddAction("Combat", "Dash", ActionKind::Digital).Valid(), true);
    ExpectEq(actions.AddAction("Combat", "Move", ActionKind::Axis2D).Valid(), true);
    ExpectEq(actions.AddAction("Combat", "AimZoom", ActionKind::Axis1D).Valid(), true);

    Expect(actions.AddBinding("Combat", "Fire", InputBinding::Mouse(MouseButton::Left)));
    Expect(actions.AddBinding("Combat", "Reload", InputBinding::Key(KeyCode::R)));
    Expect(actions.AddBinding("Combat", "Dash", InputBinding::Key(KeyCode::LShift)));
    Expect(actions.AddBinding("Combat", "Move", InputBinding::WASD()));
    Expect(actions.AddBinding("Combat", "Move", InputBinding::LeftStick2D()));
    Expect(actions.AddBinding("Combat", "AimZoom", InputBinding::GamepadAxis1D(GamepadAxis::RightTrigger)));
}

// --- negative surface pins ----------------------------------------------------
//
// Each of these asks "does this type have that member?" at compile time. `&T::name`
// in an unevaluated operand is a substitution failure when the member is absent,
// which is what makes the answer usable in a concept; a requires-expression body
// would be a hard error instead.

template <typename T>
concept HasMemberCreateActionMapAsReference = requires { &T::CreateActionMap; };

template <typename T>
concept HasMutableSeatAccess = requires(T t) { t.MutableSeatKeyboardMouse(); };

template <typename T>
concept HasInjectKey = requires(T t) { &T::InjectKey; };

template <typename T>
concept HasInjectResize = requires(T t) { &T::InjectResize; };

template <typename T>
concept HasInjectGamepadSample = requires(T t) { &T::InjectGamepadSample; };

template <typename T>
concept HasFrameLifecycle = requires(T t) { &T::BeginFrame; };

template <typename T>
concept HasHapticsStep = requires(T t) { &T::StepHaptics; };

template <typename T>
concept HasRumbleSetter = requires(T t) { &T::SetGamepadRumble; };

template <typename T>
concept HasMapAuthoring = requires(T t) { &T::CreateActionMap; };

template <typename T>
concept HasRoutingSetter = requires(T t) { &T::SetRoutingPolicy; };

// The two accessor-shape questions, asked as concepts for the same reason: a
// requires-expression written directly in a static_assert is not a substitution
// context, so it would be a hard error rather than `false`.
template <typename T>
concept HasHapticsAccessor = requires(T& engine) { engine.Haptics(); };

template <typename T>
concept HasAuthoringThroughInput = requires(T& engine) { engine.Input().CreateActionMap("m"); };

} // namespace

void test_capability_surface_has_no_lifecycle_or_injection() {
    // Compile-time pins on the shape of the split.
    //
    // These go through a concept rather than `!requires { ... }` because a
    // missing member is a hard error inside a requires-expression's body, not a
    // substitution failure -- so the concept is what makes "this member does not
    // exist" an answerable question. If somebody adds an Inject* back to the
    // authoring capability, a BeginFrame, or a mutable seat accessor, the
    // static_assert below stops the build with a message that names the reason.
    static_assert(!HasMutableSeatAccess<InputActions>);
    // CreateActionMap exists and returns a value, not a map reference.
    static_assert(HasMapAuthoring<InputActions>);
    static_assert(!HasInjectKey<InputActions>);
    static_assert(!HasInjectResize<InputActions>);
    static_assert(!HasInjectGamepadSample<InputActions>);
    static_assert(!HasFrameLifecycle<InputActions>);
    static_assert(!HasHapticsStep<InputActions>);

    // The read capability cannot author or mutate anything.
    static_assert(!HasMemberCreateActionMapAsReference<InputReader>);
    static_assert(!HasMutableSeatAccess<InputReader>);
    static_assert(!HasInjectKey<InputReader>);
    static_assert(!HasFrameLifecycle<InputReader>);
    static_assert(!HasRumbleSetter<InputReader>);
    static_assert(!HasMapAuthoring<InputReader>);
    static_assert(!HasRoutingSetter<InputReader>);

    // Haptics is rumble and nothing else: no reader underneath it, no authoring.
    static_assert(!HasMapAuthoring<InputHaptics>);
    static_assert(!HasMutableSeatAccess<InputHaptics>);

    // The authoring calls that used to hand back a reference now hand back a
    // value: a bool (did it happen) or an ActionID (which action).
    static_assert(std::is_same_v<decltype(std::declval<InputActions&>().CreateActionMap("m")), bool>);
    static_assert(std::is_same_v<decltype(std::declval<InputActions&>().AddAction("m", "a")), ActionID>);
    static_assert(std::is_same_v<decltype(std::declval<InputActions&>().AddBinding("m", "a", InputBinding {})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<InputActions&>().BindingsFor("m", "a")), std::vector<InputBinding>>);
    static_assert(std::is_same_v<decltype(std::declval<InputHaptics&>().SetGamepadRumble(GamepadSlot {0}, GamepadRumble {})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<InputHaptics&>().RumbleSeat(kPrimarySeat, GamepadRumble {})), uint32_t>);
}

void test_action_map_authoring_conflicts_defaults_and_reads() {
    auto engine = MakeHeadlessEngine();
    if (!engine) return;

    auto actions = engine->Input();
    AuthorCombatMap(actions);
    actions.SnapshotAllDefaults();

    // The read capability sees exactly what the authoring capability wrote.
    const auto reader = std::as_const(*engine).Input();
    Expect(reader.HasActionMap("Combat"));
    Expect(reader.IsActionMapEnabled("Combat"));
    Expect(!reader.HasActionMap("Missing"));

    // BindingsFor returns an owned copy, so it stays valid across further
    // authoring -- which is precisely what a reference into the router's
    // storage would not.
    const std::vector<InputBinding> moveBindings = actions.BindingsFor("Combat", "Move");
    ExpectEq(moveBindings.size(), size_t {2});
    if (moveBindings.size() == 2) {
        Expect(moveBindings[0] == InputBinding::WASD());
        Expect(moveBindings[1] == InputBinding::LeftStick2D());
    }

    // Conflict detection finds Reload when probing Key(R), unless Reload is the
    // action being rebound -- the same question a rebinding UI asks.
    const auto conflicts = actions.FindConflictingActions("Combat", InputBinding::Key(KeyCode::R));
    ExpectEq(conflicts.size(), size_t {1});
    if (!conflicts.empty()) Expect(conflicts[0] == ActionID("Reload"));
    Expect(actions.FindConflictingActions("Combat", InputBinding::Key(KeyCode::R), "Reload").empty());
    // An unused key conflicts with nothing.
    Expect(actions.FindConflictingActions("Combat", InputBinding::Key(KeyCode::F7)).empty());

    // Rebind is a value call by name; the slot is an index, not a handle.
    Expect(actions.Rebind("Combat", "Reload", 0, InputBinding::Key(KeyCode::G, KeyModifiers::Ctrl)));
    const auto rebound = actions.BindingsFor("Combat", "Reload");
    ExpectEq(rebound.size(), size_t {1});
    if (!rebound.empty()) Expect(rebound[0] == InputBinding::Key(KeyCode::G, KeyModifiers::Ctrl));

    // ResetAllToDefaults puts back what SnapshotAllDefaults captured -- which is
    // the whole reason the snapshot is a separate, explicit call.
    actions.ResetAllToDefaults();
    const auto restoredDefaults = actions.BindingsFor("Combat", "Reload");
    ExpectEq(restoredDefaults.size(), size_t {1});
    if (!restoredDefaults.empty()) Expect(restoredDefaults[0] == InputBinding::Key(KeyCode::R));

    // Removing and clearing are value operations that report whether they hit
    // anything, so a settings screen can tell a stale config from a real edit.
    Expect(actions.RemoveBinding("Combat", "Dash", 0));
    Expect(actions.BindingsFor("Combat", "Dash").empty());
    Expect(!actions.RemoveBinding("Combat", "Dash", 0));
    Expect(actions.ClearBindings("Combat", "AimZoom"));
    Expect(actions.BindingsFor("Combat", "AimZoom").empty());
    Expect(!actions.ClearBindings("Combat", "NoSuchAction"));

    // Map-level policy is a value setter, and the reader reports it.
    Expect(actions.SetActionMapBlocksLowerPriority("Combat", true));
    Expect(actions.SetActionMapConsumePhysicalInputs("Combat", false));
    Expect(actions.SetActionMapEnabledForSeat("Combat", kPrimarySeat, false));
    // The map exists, so disabling it lands and the reader reports it disabled.
    Expect(actions.SetActionMapEnabled("Combat", false));
    Expect(!reader.IsActionMapEnabled("Combat"));
    // A map that is not there reports false rather than asserting.
    Expect(!actions.SetActionMapEnabled("NoSuchMap", false));
    Expect(!actions.SetActionMapBlocksLowerPriority("NoSuchMap", true));
    Expect(!actions.SetActionMapConsumePhysicalInputs("NoSuchMap", true));

    Expect(actions.SetActionMapEnabled("Combat", true));
    Expect(reader.IsActionMapEnabled("Combat"));
    Expect(actions.RemoveActionMap("Combat"));
    Expect(!reader.HasActionMap("Combat"));
}

void test_configure_action_and_map_from_descriptors() {
    auto engine = MakeHeadlessEngine();
    if (!engine) return;
    auto actions = engine->Input();

    // A settings screen loads descriptors, not call sequences.
    const ActionMapConfig mapConfig {
        .name                    = String64("Menus"),
        .priority                = 100,
        .seatMask                = SeatMask::FromSeat(kPrimarySeat),
        .enabled                 = true,
        .blocksLowerPriority     = true,
        .consumePhysicalInputs   = true,
    };
    Expect(actions.ConfigureActionMap(mapConfig));
    Expect(std::as_const(*engine).Input().HasActionMap("Menus"));

    const InputBinding bindings[] = {InputBinding::Key(KeyCode::Enter), InputBinding::GamepadBtn(GamepadButton::South)};
    const ActionConfig actionConfig {
        .mapName            = String64("Menus"),
        .actionName         = String64("Confirm"),
        .kind               = ActionKind::Digital,
        .actuationThreshold = 0.40f,
        .clampUnitCircle    = false,
        .replaceBindings    = true,
        .bindings           = bindings,
        .bindingCount       = 2,
    };
    const ActionID confirm = actions.ConfigureAction(actionConfig);
    Expect(confirm.Valid());
    Expect(confirm == ActionID("Confirm"));

    const auto authored = actions.BindingsFor("Menus", "Confirm");
    ExpectEq(authored.size(), size_t {2});
    if (authored.size() == 2) {
        Expect(authored[0] == InputBinding::Key(KeyCode::Enter));
        Expect(authored[1] == InputBinding::GamepadBtn(GamepadButton::South));
    }

    // A descriptor that names a map nobody created is refused, not asserted: a
    // stale config file is a normal event.
    const ActionConfig orphaned {
        .mapName    = String64("NoSuchMap"),
        .actionName = String64("Nope"),
    };
    Expect(!actions.ConfigureAction(orphaned).Valid());
    // As is one whose binding array does not match its count.
    const ActionConfig inconsistent {
        .mapName      = String64("Menus"),
        .actionName   = String64("Broken"),
        .bindings     = nullptr,
        .bindingCount = 3,
    };
    Expect(!actions.ConfigureAction(inconsistent).Valid());
}

void test_binding_serialization_round_trip() {
    auto engine = MakeHeadlessEngine();
    if (!engine) return;
    auto actions = engine->Input();
    AuthorCombatMap(actions);

    const std::string serialized = actions.SerializeBindings();
    Expect(!serialized.empty());

    // Round-trip through the text and nothing else. The source map is removed
    // first, so everything the checks below find has to have come back out of
    // the serialized string: there is no second InputContext to construct
    // (work item 1), and a second Engine in one process is not available to a
    // test either. Clearing and reloading is the stronger assertion anyway --
    // it proves the document is complete rather than that two live routers
    // happen to agree.
    Expect(actions.RemoveActionMap("Combat"));
    Expect(!actions.HasActionMap("Combat"));

    const auto loaded = actions.DeserializeBindings(serialized);
    Expect(loaded.has_value());
    if (!loaded) return;

    const auto reader = std::as_const(*engine).Input();
    Expect(reader.HasActionMap("Combat"));
    Expect(reader.IsActionMapEnabled("Combat"));

    const auto fire = actions.BindingsFor("Combat", "Fire");
    ExpectEq(fire.size(), size_t {1});
    if (!fire.empty()) Expect(fire[0] == InputBinding::Mouse(MouseButton::Left));

    const auto move = actions.BindingsFor("Combat", "Move");
    ExpectEq(move.size(), size_t {2});
    if (move.size() == 2) {
        Expect(move[0] == InputBinding::WASD());
        Expect(move[1] == InputBinding::LeftStick2D());
    }

    const auto aim = actions.BindingsFor("Combat", "AimZoom");
    ExpectEq(aim.size(), size_t {1});
    if (!aim.empty()) Expect(aim[0] == InputBinding::GamepadAxis1D(GamepadAxis::RightTrigger));

    // The parse error is a value enum from ActionValues.hpp -- naming it does
    // not require the router's storage, which is why it moved.
    const auto badHeader = actions.DeserializeBindings("NOT_A_HEADER\n");
    Expect(!badHeader.has_value());
    if (!badHeader) Expect(badHeader.error().Is(InputBindingParseError::InvalidHeader));
}

void test_routing_and_deadzone_configuration() {
    auto engine = MakeHeadlessEngine();
    if (!engine) return;
    auto actions = engine->Input();
    const auto reader = std::as_const(*engine).Input();

    // Defaults first, so a change is observable rather than assumed.
    Expect(reader.RoutingPolicy() == SeatRoutingPolicy::SharePrimarySeat);
    Expect(reader.SeatReceivesKeyboardMouse(kPrimarySeat));

    actions.SetRoutingPolicy(SeatRoutingPolicy::DedicatedGamepadSeats);
    Expect(reader.RoutingPolicy() == SeatRoutingPolicy::DedicatedGamepadSeats);

    actions.SetKeyboardMouseSeat(kPrimarySeat, false);
    Expect(!reader.SeatReceivesKeyboardMouse(kPrimarySeat));
    actions.SetKeyboardMouseSeat(kPrimarySeat, true);
    Expect(reader.SeatReceivesKeyboardMouse(kPrimarySeat));

    // No pads are connected headless, so an assignment is accepted but reports
    // no seat change: routing is per-pad, and a pad that is not there has none.
    Expect(actions.AssignGamepadToSeat(GamepadSlot {0}, PlayerSeat {1}));
    ExpectEq(actions.ConnectedGamepadCount(), size_t {0});
    Expect(!reader.GamepadConnected(GamepadSlot {0}));
    // The assignment is recorded even though no device is attached, so a pad
    // that appears later lands on the seat that was asked for.
    Expect(reader.GamepadSeat(GamepadSlot {0}) == PlayerSeat {1});
    actions.UnassignGamepad(GamepadSlot {0});
    Expect(reader.GamepadSeat(GamepadSlot {0}) == kUnassignedSeat);

    // Deadzone configuration is a value setter; the router keeps it. Idle
    // queries stay zero-valued whether or not a pad exists, which is the
    // contract a gameplay system can rely on without a null check.
    const DeadzoneConfig radial {.mode = DeadzoneMode::ScaledRadial, .inner = 0.20f, .outer = 0.90f, .curveExponent = 1.0f};
    actions.SetGamepadDeadzones(GamepadSlot {0}, radial, radial, 0.10f);
    actions.SetSeatDeadzones(PlayerSeat {1}, radial, radial, 0.10f);

    Expect(Near(reader.GamepadAxisValue(GamepadSlot {0}, GamepadAxis::LeftX), 0.0f));
    Expect(Near(reader.GamepadLeftStick(GamepadSlot {0}).magnitude, 0.0f));
    Expect(Near(reader.GamepadRightStick(GamepadSlot {0}).magnitude, 0.0f));
    Expect(Near(reader.GamepadLeftTrigger(GamepadSlot {0}), 0.0f));
    Expect(Near(reader.GamepadRightTrigger(GamepadSlot {0}), 0.0f));
    Expect(!reader.GamepadButtonDown(GamepadSlot {0}, GamepadButton::South));
    Expect(!reader.GamepadButtonPressed(GamepadSlot {0}, GamepadButton::South));
    Expect(!reader.GamepadButtonReleased(GamepadSlot {0}, GamepadButton::South));
}

void test_physical_state_reads_by_value_on_a_fresh_frame() {
    auto engine = MakeHeadlessEngine();
    if (!engine) return;
    const auto reader = std::as_const(*engine).Input();

    // A frame that has not run has no key down, no motion and no wheel. Every
    // query returns a value, so this reads as one expression per question.
    Expect(!reader.KeyDown(KeyCode::W));
    Expect(!reader.KeyPressed(KeyCode::W));
    Expect(!reader.KeyHeld(KeyCode::W));
    Expect(!reader.KeyReleased(KeyCode::W));
    Expect(!reader.MouseButtonDown(MouseButton::Left));
    Expect(reader.Modifiers() == KeyModifiers::None);
    Expect(!reader.HasModifiers(KeyModifiers::Shift));
    Expect(Near(reader.MousePosition().x, 0.0f));
    Expect(Near(reader.MouseDelta().x, 0.0f));
    Expect(Near(reader.MouseWheel(), 0.0f));
    Expect(reader.LastActiveDevice() == InputDeviceKind::KeyboardMouse);

    // UiCapture is reported as a value, not as a flag to keep in sync.
    const UiCaptureState capture = reader.UiCapture();
    Expect(!capture.keyboard && !capture.mouse);

    // Every seat answers independently, and an out-of-range seat answers as an
    // empty seat rather than as a crash.
    // Every seat answers independently of every other: no seat reports a key it
    // was never given, and the seat a caller names is the seat that answers.
    for (size_t i = 0; i < kMaxPlayerSeats; ++i) {
        const PlayerSeat seat {static_cast<uint8_t>(i)};
        Expect(!reader.KeyDown(KeyCode::W, seat));
        Expect(!reader.KeyPressed(KeyCode::W, seat));
        Expect(Near(reader.MouseDelta(seat).x, 0.0f));
        Expect(Near(reader.MouseWheel(seat), 0.0f));
    }
    const PlayerSeat invalidSeat {static_cast<uint8_t>(kMaxPlayerSeats + 3)};
    Expect(!reader.KeyDown(KeyCode::W, invalidSeat));

    // Snapshot is a copy: mutating it must not write back into the router.
    InputState snapshot = reader.Snapshot();
    snapshot.SetKey(KeyCode::W, true);
    Expect(snapshot.IsKeyDown(KeyCode::W));
    Expect(!std::as_const(*engine).Input().KeyDown(KeyCode::W));

    // An action no map defines is not actuated, which is the same answer the
    // COM facade gives.
    const ActionState missing = reader.Action(ActionID("NotMapped"));
    Expect(!missing.Down() && !missing.Pressed());
    Expect(!reader.ActionDown(ActionID("NotMapped")));
    Expect(Near(reader.ActionAxis1D(ActionID("NotMapped")), 0.0f));

    // Capture is idle until somebody begins one, and TakeCapturedBinding on an
    // idle session is empty rather than undefined.
    Expect(reader.CaptureStatus() == BindingCaptureStatus::Idle);
    Expect(!engine->Input().TakeCapturedBinding().has_value());
}

void test_binding_capture_requires_a_device_edge() {
    auto engine = MakeHeadlessEngine();
    if (!engine) return;
    auto actions = engine->Input();

    // A capture session is a *wait*: it reports Waiting until a device edge
    // arrives, and there is no Inject* here to manufacture one. What this pins
    // is that the session starts, cancels and reports status as values, and that
    // nothing about it is reachable except through the capability.
    actions.BeginBindingCapture(kPrimarySeat);
    Expect(actions.CaptureStatus() == BindingCaptureStatus::Waiting);
    Expect(std::as_const(*engine).Input().CaptureStatus() == BindingCaptureStatus::Waiting);

    actions.CancelBindingCapture();
    Expect(actions.CaptureStatus() == BindingCaptureStatus::Cancelled);
    Expect(!actions.TakeCapturedBinding().has_value());

    // A began-then-cancelled session is startable again: cancelling is a state,
    // not a terminal one.
    actions.BeginBindingCapture(kPrimarySeat, /*captureAxes=*/true, KeyCode::F1);
    Expect(actions.CaptureStatus() == BindingCaptureStatus::Waiting);
    actions.CancelBindingCapture();
    Expect(actions.CaptureStatus() == BindingCaptureStatus::Cancelled);
}

// The UI seam is the one place a caller hands input *in*, so it is the one
// place a borrowed buffer could outlive its owner. SubmitUIInput is documented
// as replacing the next ConsumeUIInput, which means it is read on a different
// call than the one that supplied it -- so the engine has to own the contents.
void test_submitted_ui_input_owns_its_spans() {
    auto engine = MakeHeadlessEngine();
    if (!engine) return;

    // The caller's storage stays alive but is overwritten after the call, which
    // is what a host's frame-local vector looks like one statement later. If the
    // engine kept either span instead of a copy, the checks below would report
    // the overwrite rather than the submission.
    std::vector<KeyCode>  keys {KeyCode::A, KeyCode::B};
    std::vector<uint32_t> characters {U'a', U'b'};

    GUI::UIInput ui {};
    ui.mouseX          = 12.0f;
    ui.mouseY          = 34.0f;
    ui.pressedKeys     = keys;
    ui.characterStream = characters;
    engine->SubmitUIInput(ui);

    keys.assign(1, KeyCode::Z);
    characters.assign(1, U'z');

    const GUI::UIInput consumed = engine->ConsumeUIInput(1.0f / 60.0f);
    ExpectEq(consumed.pressedKeys.size(), size_t {2});
    ExpectEq(consumed.characterStream.size(), size_t {2});
    if (consumed.pressedKeys.size() == 2) {
        Expect(consumed.pressedKeys[0] == KeyCode::A);
        Expect(consumed.pressedKeys[1] == KeyCode::B);
    }
    if (consumed.characterStream.size() == 2) {
        ExpectEq(consumed.characterStream[0], uint32_t {U'a'});
        ExpectEq(consumed.characterStream[1], uint32_t {U'b'});
    }
    // The frame clock and viewport are the engine's, not the host's, even on the
    // submitted path.
    ExpectEq(consumed.mouseX, 12.0f);
    ExpectEq(consumed.mouseY, 34.0f);
    Expect(Near(consumed.dt, 1.0f / 60.0f));

    // A submission is consumed exactly once: the next call falls through to the
    // device path, where a headless engine has nothing queued.
    const GUI::UIInput next = engine->ConsumeUIInput(1.0f / 60.0f);
    ExpectEq(next.pressedKeys.size(), size_t {0});
    ExpectEq(next.characterStream.size(), size_t {0});

    // A second submission replaces the first wholesale, and an empty one does
    // not leave the previous frame's vector behind.
    engine->SubmitUIInput(GUI::UIInput {.pressedKeys = keys});
    engine->SubmitUIInput(GUI::UIInput {});
    const GUI::UIInput replaced = engine->ConsumeUIInput(0.0f);
    ExpectEq(replaced.pressedKeys.size(), size_t {0});
}

void test_sub_capabilities_and_forwarding() {
    auto engine = MakeHeadlessEngine();
    if (!engine) return;
    auto actions = engine->Input();

    // Reader() is the read-only view of the same router, so authoring through
    // the parent is visible through the child immediately.
    Expect(actions.CreateActionMap("Shared", 0));
    Expect(actions.Reader().HasActionMap("Shared"));
    Expect(actions.HasActionMap("Shared")); // forwarded read agrees with the child

    // Haptics() is the rumble view. No pad is connected, so the request does
    // not land -- and it reports that rather than pretending.
    Expect(!actions.Haptics().SetGamepadRumble(GamepadSlot {0}, GamepadRumble {.lowFrequency = 1.0f, .durationSeconds = 1.0f}));
    ExpectEq(actions.Haptics().RumbleSeat(PlayerSeat {0}, GamepadRumble {.lowFrequency = 1.0f, .durationSeconds = 1.0f}), 0u);
    Expect(actions.Haptics().ActiveRumble(GamepadSlot {0}).IsZero());
    actions.StopAllRumble();
    actions.StopRumble(GamepadSlot {0});

    // The engine's Haptics() accessor is the same router: nothing above changed
    // it, and an idle rumble channel stays idle.
    Expect(engine->Haptics().ActiveRumble(GamepadSlot {0}).IsZero());
    Expect(!engine->Haptics().SetGamepadRumble(GamepadSlot {1}, GamepadRumble {.highFrequency = 0.5f, .durationSeconds = 0.1f}));

    // A const Engine has no authoring accessor at all: Input() const is a
    // reader, Haptics() is non-const only.
    static_assert(std::is_same_v<decltype(std::declval<Engine&>().Input()), InputActions>);
    static_assert(std::is_same_v<decltype(std::declval<const Engine&>().Input()), InputReader>);
    static_assert(std::is_same_v<decltype(std::declval<Engine&>().Haptics()), InputHaptics>);
    static_assert(!HasHapticsAccessor<const Engine>);
    static_assert(!HasAuthoringThroughInput<const Engine>);
}

} // namespace ZHLN::Test
