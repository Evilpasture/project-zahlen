// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Opt-in local GPU test: whole application launches. Most stop before the
// first Tick; a failure probe runs a UI-only frame without GPU rendering.
#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include "TestsFramework.hpp"
#include <Zahlen/App.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/FramePlan.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/Engine/Kernel.hpp>
#include <Zahlen/Render/SceneData.hpp>
#include <Zahlen/KernelApp.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/Engine/Capabilities/EngineState.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/InputReader.hpp>
#include <Zahlen/Window/WindowInput.hpp>
#include <Zahlen/Engine/Scene.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#if defined(ZHLN_HAS_EDITOR)
#include <editor/GUIEditor.hpp>
#endif
#if defined(ZHLN_HAS_ANIMATION)
#include <Animation/AnimationSystem.hpp>
#endif
#if defined(ZHLN_HAS_TERRAIN)
#include <Terrain/TerrainSystem.hpp>
#endif
#if defined(ZHLN_HAS_CHARACTER_CONTROLLER)
#include <CharacterController/CharacterController.hpp>
#endif
#if defined(ZHLN_HAS_CAMERA_MODULES)
#include <Camera/TargetCamera.hpp>
#include <FreeCam/FreeCam.hpp>
#endif
#include <algorithm>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN::Test {

// Both the simulation and render graphs can resolve canonical engine configuration
// through their existing extension-service slots (without an Engine field).
static_assert(ECS::ResolvableFrom<SystemContext, ECS::Res<EngineConfig>>);
static_assert(ECS::ResolvableFrom<ECS::Carrier<RenderServices>, ECS::Res<EngineConfig>>);

// Steps() is a read-only span; tests look a named step up by value.
static auto FindStep(Engine& engine, std::string_view name) -> const FrameStep* {
    for (const FrameStep& step: engine.Steps()) {
        if (step.name == name) {
            return &step;
        }
    }
    return nullptr;
}

void ExpectUserClosed(const AppRunResult& result) {
    Expect(result.has_value());
    if (result) Expect(*result == AppExitReason::UserClosed);
}

struct StartPhaseLayer {
    std::vector<std::string_view>* calls = nullptr;
    void Start(Engine&) { calls->push_back("start-only"); }
};

struct TickPhaseLayer {
    std::vector<std::string_view>* calls = nullptr;
    void Tick(Engine&, float) { calls->push_back("tick-only"); }
};

struct RecordingLayer {
    std::vector<std::string_view>* calls = nullptr;
    void Start(Engine& engine) {
        calls->push_back("layer-start");
        Expect(engine.Entities().GetSingleton<Components::CameraComponent>().has_value());
    }
    void Tick(Engine& engine, float dt) {
        calls->push_back("layer-tick");
        Expect(engine.GetCurrentFrame() == 0);
        Expect(dt >= 0.0f);
    }
};

void test_app_composes_before_scene_and_runs_gameplay_after_events() {
    std::vector<std::string_view> calls;
    int                           updates = 0;

    EngineConfig config {
        .appName        = "ApplicationTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    };
    auto application = App(config);
    application.AddLayer(StartPhaseLayer {.calls = &calls});
    application.AddLayer(RecordingLayer {.calls = &calls});
    application.AddLayer(TickPhaseLayer {.calls = &calls});
    application.Install([&](Engine& engine) {
        calls.push_back("install");
        Expect(engine.Service<EngineConfig>().has_value()); // no-CLI uses the same canonical service
        Expect(!engine.Entities().GetSingleton<Components::CameraComponent>());
        const auto installed = engine.InstallModule(Module {.name = "AppTestModule"});
        Expect(installed.has_value());
    });
    application.OnStart([&](Engine& engine) {
        calls.push_back("start");
        Expect(engine.Entities().GetSingleton<Components::CameraComponent>().has_value());
        Expect(FindStep(engine, "PhysicsSystem") != nullptr);
        Expect(FindStep(engine, "UpdateGraph") != nullptr);
        Expect(FindStep(engine, "GameplayModule") == nullptr);
        // The event phase is what clears a frame's edges, and what consumes a
        // resize. Both facts are asserted from the public read capability: this
        // callback runs in OnStart, before the first ProcessEvents, so the seat
        // it reads is a device that has never reported anything and there is no
        // resize waiting. (The old test wrote a stale delta and injected a resize
        // through the router to assert the same two things; there is no public
        // way to write either now, which is the change.)
        const auto reader = std::as_const(engine).Input();
        Expect(reader.MouseDelta().x == 0.0f);
        Expect(reader.MouseDelta().y == 0.0f);
        Expect(!reader.KeyDown(KeyCode::W));
        const auto modules = engine.GetInstalledModules();
        Expect(std::ranges::any_of(modules, [](const Module& module) { return module.name == "AppTestModule"; }));
        Expect(!std::ranges::any_of(modules, [](const Module& module) { return module.name == "FallbackScene"; }));
        // Application's default pass installs nothing: app/CMakeLists.txt leaves
        // optional composition to the consuming application, so the first module
        // is exactly the one this test's installer added. Anything ahead of it
        // would mean a default module crept back in behind the composition API.
        Expect(modules.front().name == "AppTestModule");
    });

    // A move-only callback proves Run does not copy its user's game logic.
    auto state = std::make_unique<int>(42);
    const auto result = application.Run([&, state = std::move(state)](Engine& engine, float dt) {
        ++updates;
        Expect(*state == 42);
        Expect(dt >= 0.0f);
        // One real frame has run: ProcessEvents began it (clearing the previous
        // frame's deltas), SynchronizeResize consumed nothing because the headless
        // host reported no size change, and the renderer holds the extent it
        // started with.
        const auto reader = std::as_const(engine).Input();
        Expect(reader.MouseDelta().x == 0.0f);
        Expect(reader.MouseDelta().y == 0.0f);
        const auto fbSize = engine.Graphics().FramebufferSize();
        Expect(fbSize.has_value() && fbSize->width == 800u && fbSize->height == 600u);
        Expect(engine.GetCurrentFrame() == 0);
        calls.push_back("update");
        engine.GetPlatformHost().Close();
    });

    ExpectUserClosed(result);
    Expect(updates == 1);
    Expect(calls == std::vector<std::string_view> {"install", "start-only", "layer-start", "start", "layer-tick", "tick-only", "update"});
}

// Move-only state must survive the UI callback's type erasure and remain
// available to Stop while the Engine and its scene still exist.
struct UILifecycleLayer {
    std::unique_ptr<int>             state;
    std::vector<std::string_view>* calls = nullptr;
    void Start(Engine&) {
        ++*state;
        calls->push_back("layer-start");
    }
    void Tick(Engine&, float) {
        ++*state;
        calls->push_back("layer-tick");
    }
    void UI(Engine& engine) {
        Expect(*state == 3);
        Expect(engine.GetCurrentFrame() == 0);
        calls->push_back("layer-ui");
    }
    void Stop(Engine& engine) {
        Expect(*state == 3);
        Expect(engine.Entities().GetSingleton<Components::CameraComponent>().has_value());
        calls->push_back("layer-stop");
    }
};

struct UIOnlyPhaseLayer {
    std::vector<std::string_view>* calls = nullptr;
    void UI(Engine&) { calls->push_back("ui-only"); }
};

struct StopOnlyPhaseLayer {
    std::vector<std::string_view>* calls = nullptr;
    void Stop(Engine&) { calls->push_back("stop-only"); }
};

struct HUDPhaseLayer {
    std::vector<std::string_view>* calls = nullptr;
    void UI(Engine&) { calls->push_back("hud-ui"); }
};

void RunHostUIPhaseForTest(Engine& engine) {
    // Exercise the actual Core UI phase without running GPU/render steps.
    const auto* uiStep = FindStep(engine, "HostUICallback");
    Expect(uiStep != nullptr);
    if (uiStep != nullptr) {
        Expect(uiStep->phase == FramePhase::UI);
        Expect(uiStep->run != nullptr);
        if (uiStep->run != nullptr) {
            FrameContext frame;
            uiStep->run(engine, 0.0f, frame);
            Expect(!frame.error.has_value());
        }
    }
}

// Observe the real fixed-step frame callback, without running a render frame.
struct SubstepProbe {
    Entity bodyEntity {};
    Entity zeroEntity {};
    Entity orphanEntity {};
    Physics::BodyID body {};
    int preCount = 0;
    int postCount = 0;
    bool valid = true;
    double firstX = 0.0;
    double secondX = 0.0;
};
struct PendingPhaseMarker {};
static SubstepProbe* activeSubstepProbe = nullptr;

void ProbePreStep(Engine& engine, float dt) {
    auto& probe = *activeSubstepProbe;
    auto reg = engine.Entities();
    probe.valid = probe.valid && dt == kFixedStepDt && probe.preCount == probe.postCount;
    if (probe.preCount == 0) {
        probe.valid = probe.valid && reg.Get<Components::ImpulseCommand>(probe.bodyEntity).has_value();
    } else {
        // Previous substep must already have consumed its impulse. A new
        // command issued now belongs to this substep, not the next frame.
        probe.valid = probe.valid && !reg.Get<Components::ImpulseCommand>(probe.bodyEntity);
        reg.Attach(probe.bodyEntity, Components::ImpulseCommand {.linear = {10.0f, 0.0f, 0.0f}});
    }
    ++probe.preCount;
}

void ProbePostStep(Engine& engine) {
    auto& probe = *activeSubstepProbe;
    auto reg = engine.Entities();
    probe.valid = probe.valid && probe.preCount == probe.postCount + 1 &&
        !reg.Get<Components::ImpulseCommand>(probe.bodyEntity) &&
        !reg.Get<Components::ImpulseCommand>(probe.zeroEntity) &&
        !reg.Get<Components::ImpulseCommand>(probe.orphanEntity);
    const auto pos = engine.Physics().BodyPosition(probe.body);
    probe.valid = probe.valid && pos.has_value();
    if (pos && probe.postCount == 0) probe.firstX = pos->x;
    if (pos && probe.postCount == 1) probe.secondX = pos->x;
    ++probe.postCount;
}

void test_physics_substeps_consume_impulses_before_step_and_keep_main_buffer_deferred() {
    auto app = App(EngineConfig {.appName = "PhysicsSubstepTest", .validationMode = ValidationMode::Off, .headless = true});
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        const auto* physicsStep = FindStep(engine, "PhysicsSystem");
        Expect(physicsStep && physicsStep->phase == FramePhase::Physics && physicsStep->run);
        if (!physicsStep || !physicsStep->run) return;
        auto reg = engine.Entities();
        const auto body = engine.Physics().CreateRigidBody(Physics::Shape<Physics::Sphere> {}, {0, 10, 0}, {0, 0, 0, 1},
                                                           Physics::MotionType::Dynamic, Physics::ObjectLayer::Moving);
        Expect(body != Physics::BodyID::Null());
        SubstepProbe probe {
            .bodyEntity = reg.Spawn(Components::PhysicsComponent {.physicsID = body, .isStatic = false},
                                    Components::ImpulseCommand {.linear = {10.0f, 0.0f, 0.0f}}),
            .zeroEntity = reg.Spawn(Components::PhysicsComponent {.physicsID = body, .isStatic = false}, Components::ImpulseCommand {}),
            .orphanEntity = reg.Spawn(Components::ImpulseCommand {.linear = {5.0f, 0.0f, 0.0f}}),
            .body = body,
        };
        const auto unrelated = reg.Create();
        engine.Commands().AddComponent(unrelated, PendingPhaseMarker {});
        activeSubstepProbe = &probe;
        engine.SetPhysicsSubstepHooks({.preStep = &ProbePreStep, .postStep = &ProbePostStep});
        FrameContext frame {.fixedSteps = 0};
        physicsStep->run(engine, 0.0f, frame);
        Expect(probe.preCount == 0 && probe.postCount == 0 && reg.Get<Components::ImpulseCommand>(probe.bodyEntity));
        frame.fixedSteps = 2;
        physicsStep->run(engine, 0.0f, frame);
        engine.SetPhysicsSubstepHooks({});
        activeSubstepProbe = nullptr;
        Expect(probe.valid && probe.preCount == 2 && probe.postCount == 2);
        Expect(probe.firstX > 0.0 && probe.secondX > probe.firstX);
        Expect(!reg.Get<PendingPhaseMarker>(unrelated) && !engine.Commands().Empty());
        engine.Commands().Playback();
        Expect(reg.Get<PendingPhaseMarker>(unrelated).has_value());
        ran = true;
    });
    const auto result = app.Run([](Engine& engine, float) { engine.GetPlatformHost().Close(); });
    ExpectUserClosed(result);
    Expect(ran);
}

void test_camera_and_history_phase_calls_preserve_order_and_live_data() {
    auto app = App(EngineConfig {.appName = "TypedFramePhaseTest", .validationMode = ValidationMode::Off, .headless = true});
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        const auto steps = engine.Steps();
        const auto index = [&](std::string_view name) {
            for (size_t i = 0; i < steps.size(); ++i) if (steps[i].name == name) return i;
            return steps.size();
        };
        const auto cleanup = index("SceneCleanup"), camera = index("CameraSystems"), visibility = index("RenderGraph");
        const auto present = index("RenderSystem"), history = index("TransformHistory");
        Expect(cleanup < camera && camera < visibility && visibility < present && present < history && history < steps.size());
        if (history >= steps.size() || camera >= steps.size()) return;

        // The renderer creates scene targets on its first BeginFrame. The
        // effective viewport is empty until then, so initialize and cancel a
        // frame before testing nonzero viewport projection.
        auto began = engine.Graphics().BeginFrame();
        Expect(began && began->has_value());
        if (!began || !began->has_value()) return;
        (**began).Cancel();
        auto reg = engine.Entities();
        const auto cameraEntity = reg.Spawn(Components::CameraComponent {});
        const auto transformEntity = reg.Spawn(Components::WorldTransformComponent {.world = CreateTranslation(FVec3 {3.0f, 2.0f, 1.0f})});
        auto world = reg.Get<Components::WorldTransformComponent>(transformEntity);
        const auto savedViewport = engine.Graphics().Viewport();
        engine.Graphics().SetViewport({.width = 320, .height = 200});
        FrameContext context;
        steps[camera].run(engine, 0.0f, context);
        auto cam = reg.Get<Components::CameraComponent>(cameraEntity);
        const auto first = cam->camera.GetProjectionMatrix(1.6f) * cam->camera.GetViewMatrix();
        Expect(cam->frameCounter == 1 && cam->viewProj == first && cam->prevUnjitteredViewProj == first);
        engine.Graphics().SetViewport({.width = 200, .height = 200});
        steps[camera].run(engine, 0.0f, context);
        const auto second = cam->camera.GetProjectionMatrix(1.0f) * cam->camera.GetViewMatrix();
        Expect(cam->frameCounter == 2 && cam->viewProj == second && cam->prevUnjitteredViewProj == first && first != second);
        Expect(world->previous != world->world);
        steps[history].run(engine, 0.0f, context);
        Expect(world->previous == world->world);
        engine.Graphics().SetViewport(savedViewport);
        ran = true;
    });
    const auto result = app.Run([](Engine& engine, float) { engine.GetPlatformHost().Close(); });
    ExpectUserClosed(result);
    Expect(ran);
}

#if defined(ZHLN_HAS_CHARACTER_CONTROLLER)
void test_character_frame_steps_and_fixed_hooks_use_live_world() {
    auto app = App(EngineConfig {.appName = "TypedCharacterPhaseTest", .validationMode = ValidationMode::Off, .headless = true});
    app.Install([](Engine& engine) { Character::Install(engine); });
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        const auto steps = engine.Steps();
        const auto index = [&](std::string_view name) {
            for (size_t i = 0; i < steps.size(); ++i) if (steps[i].name == name) return i;
            return steps.size();
        };
        const auto inputStep = index("CharacterInput"), ui = index("HostUICallback");
        const auto hotReload = index("StateAndShaderReload"), intentStep = index("PlayerInputTranslate"), physics = index("PhysicsSystem");
        Expect(inputStep < ui && hotReload < intentStep && intentStep < physics && physics < steps.size());
        if (inputStep >= steps.size() || intentStep >= steps.size() || physics >= steps.size()) return;
        auto reg = engine.Entities();
        Expect(reg.GetSingleton<Components::CameraComponent>().has_value());
        if (!reg.GetSingleton<Components::CameraComponent>()) return;
        // The step under test reads the seat's physical state through the input
        // capability, and a headless engine has no device to report W. Rather than
        // fabricate a keystroke, drive the step's own seam: the seat translation
        // is a pure function (TranslateSeatIntent), and this asserts that the
        // *step* consumes its output, which is what the frame ordering is about.
        reg.PatchSingleton<Components::CameraComponent>([](auto& cam) { cam.camera.yaw = DegF {0.0f}; });
        const auto character = engine.Physics().CreateCharacter({0.0, 10.0, 0.0});
        Expect(character != Physics::BodyID::Null());
        const auto entity = reg.Spawn(Character::InputComponent {}, Character::MovementComponent {.isGrounded = false, .wasGrounded = false},
                                      Components::PhysicsComponent {.physicsID = character, .isStatic = false});
        FrameContext frame;
        steps[inputStep].run(engine, 0.0f, frame);
        // No device, so the system correctly latches no intent at all.
        Expect(reg.Get<Character::InputComponent>(entity)->localMoveZ == 0.0f);
        Expect(reg.Get<Character::InputComponent>(entity)->localMoveX == 0.0f);
        Expect(!reg.Get<Character::InputComponent>(entity)->wantsToJump);

        // Forward, and the same latch with an intent the translation produced --
        // the input the step above would have latched had the device reported W.
        const auto        reader   = std::as_const(engine).Input();
        InputState        pressed {};
        pressed.SetKey(KeyCode::W, true);
        const auto        intent =
            Character::TranslateSeatIntent(pressed, /*seatHasKeyboardMouse=*/true, reader, GamepadSlot {static_cast<uint8_t>(kMaxGamepads)});
        ExpectEq(intent.moveZ, 1.0f);
        reg.Patch<Character::InputComponent>(entity, [&](Character::InputComponent& ic) {
            ic.localMoveX    = intent.moveX;
            ic.localMoveZ    = intent.moveZ;
            ic.wantsToJump   = intent.jump;
            ic.wantsToSprint = intent.sprint;
        });
        steps[intentStep].run(engine, 0.0f, frame);
        Expect(reg.Get<Character::MovementComponent>(entity)->inputX > 0.99f);
        frame.fixedSteps = 2;
        steps[physics].run(engine, 0.0f, frame);
        const auto move = reg.Get<Character::MovementComponent>(entity);
        Expect(move->currentVelX > 0.0f && move->currentYVel < -0.5f && !move->isGrounded);
        ran = true;
    });
    const auto result = app.Run([](Engine& engine, float) { engine.GetPlatformHost().Close(); });
    ExpectUserClosed(result);
    Expect(ran);
}
#endif

#if defined(ZHLN_HAS_CAMERA_MODULES)
void test_camera_modules_seed_before_drive_and_respect_opt_out() {
    auto app = App(EngineConfig {.appName = "TypedCameraModulesTest", .validationMode = ValidationMode::Off, .headless = true});
    app.Install([](Engine& engine) { FreeCam::Install(engine); CameraRig::Install(engine); });
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        const auto* freeStep = FindStep(engine, "FreeCamSystem");
        const auto* rigStep = FindStep(engine, "TargetCameraSystem");
        Expect(freeStep && rigStep && freeStep->run && rigStep->run);
        if (!freeStep || !rigStep || !freeStep->run || !rigStep->run) return;
        auto reg = engine.Entities();
        const Entity main = reg.SingletonEntity<Components::MainCameraTagComponent>();
        Expect(main != Entity::Null());
        if (main == Entity::Null()) return;
        auto camera = reg.Get<Components::CameraComponent>(main);
        Expect(camera.has_value());
        if (!camera) return;
        const auto unrelated = reg.Create();
        engine.Commands().AddComponent(unrelated, PendingPhaseMarker {});

        FrameContext frame;
        // Even a paused frame seeds FreeCam, but does not drive its pose.
        const auto originalPosition = camera->camera.position;
        freeStep->run(engine, 0.0f, frame);
        Expect(reg.Get<FreeCam::FreeCamTagComponent>(main) && reg.Get<FreeCam::FreeCamComponent>(main));
        Expect(camera->camera.position == originalPosition);
        rigStep->run(engine, 0.1f, frame);
        auto rig = reg.Get<CameraRig::TargetCameraComponent>(main);
        Expect(rig.has_value() && !reg.Get<PendingPhaseMarker>(unrelated) && !engine.Commands().Empty());
        if (!rig) return;
        const auto target = reg.Spawn(Components::TransformComponent {.position = {4.0f, 3.0f, 2.0f}});
        reg.Patch<CameraRig::TargetCameraComponent>(main, [&](auto& r) {
            r.target = target;
            r.stiffness = 0.0f;
            r.yaw = 0.0f;
            r.pitch = 0.0f;
            r.targetOffset = FVec3::Zero();
            r.distance = r.targetDistance = 2.0f;
        });
        rigStep->run(engine, 0.1f, frame);
        Expect(camera->camera.position == originalPosition); // FreeCam owns the pose.
        FreeCam::Detach(engine);
        freeStep->run(engine, 0.1f, frame);
        Expect(!reg.Get<FreeCam::FreeCamTagComponent>(main)); // opt-out prevents reseeding
        rigStep->run(engine, 0.1f, frame);
        Expect((camera->camera.position - FVec3 {2.0f, 3.0f, 2.0f}).LengthSquared() < 1e-6f);
        FreeCam::Attach(engine, 4.0f, false);
        const auto before = camera->camera.position;
        // FreeCam::Apply drives the pose from the seat's physical state, and a
        // headless engine has no device: a camera that stays put is the correct
        // answer here, and the frame-order assertions above are what this test is
        // for. (Movement-driven fly was covered by writing to the router; there is
        // no public write any more, so the direct-call path is asserted to run.)
        FreeCam::Apply(engine, 0.25f); // paused-editor direct call remains supported
        Expect((camera->camera.position - before).LengthSquared() < 1e-6f);
        Expect(!reg.Get<PendingPhaseMarker>(unrelated));
        engine.Commands().Playback();
        Expect(reg.Get<PendingPhaseMarker>(unrelated).has_value());
        ran = true;
    });
    const auto result = app.Run([](Engine& engine, float) { engine.GetPlatformHost().Close(); });
    ExpectUserClosed(result);
    Expect(ran);
}
#endif

void test_presentation_draws_frozen_frustum_from_camera_query() {
    auto app = App(EngineConfig {.appName = "DebugFrustumQueryTest", .validationMode = ValidationMode::Off, .headless = true});
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        const auto* present = FindStep(engine, "RenderSystem");
        auto reg = engine.Entities();
        auto state = engine.State();
        auto camera = reg.GetSingleton<Components::CameraComponent>();
        Expect(present && present->run && camera);
        if (!present || !present->run || !camera) return;
        auto debug = engine.Debug();
        FrameContext frame;
        state.SetFreezeFrustum(false);
        debug.Clear();
        present->run(engine, 0.0f, frame);
        Expect(debug.Lines().empty());
        state.SetFreezeFrustum(true);
        reg.PatchSingleton<Components::CameraComponent>([](auto& c) { c.frozenViewProj = Mat4x4::Identity(); });
        debug.Clear();
        present->run(engine, 0.0f, frame);
        const auto lines = debug.Lines();
        Expect(lines.size() == 12);
        if (lines.size() == 12) {
            Expect(lines[0].start == FVec3 {-1.0f, -1.0f, 0.0f});
            Expect(lines[0].end == FVec3 {1.0f, -1.0f, 0.0f});
        }
        state.SetFreezeFrustum(false);
        ran = true;
    });
    const auto result = app.Run([](Engine& engine, float) { engine.GetPlatformHost().Close(); });
    ExpectUserClosed(result);
    Expect(ran);
}

struct CleanupProbe {
    Entity parent {};
    Entity child {};
    Entity survivor {};
    int pendingCalls = 0;
    int clearCalls = 0;
    bool valid = true;
};
static CleanupProbe* activeCleanupProbe = nullptr;

void ProbeCleanup(Engine& engine, bool all) {
    if (!activeCleanupProbe) return;
    auto& probe = *activeCleanupProbe;
    auto reg = engine.Entities();
    if (!all) {
        ++probe.pendingCalls;
        const auto parent = reg.Get<Components::PhysicsComponent>(probe.parent);
        const auto child = reg.Get<Components::PhysicsComponent>(probe.child);
        const auto survivor = reg.Get<Components::PhysicsComponent>(probe.survivor);
        probe.valid = probe.valid && reg.IsAlive(probe.parent) && reg.IsAlive(probe.child) &&
            reg.Get<Components::PendingDestroy>(probe.child) && parent && child && survivor &&
            parent->physicsID == Physics::BodyID::Null() && child->physicsID == Physics::BodyID::Null() &&
            survivor->physicsID != Physics::BodyID::Null();
    } else {
        ++probe.clearCalls;
        const auto survivor = reg.Get<Components::PhysicsComponent>(probe.survivor);
        probe.valid = probe.valid && reg.IsAlive(probe.survivor) && survivor &&
            survivor->physicsID == Physics::BodyID::Null();
    }
}

void test_cleanup_releases_expanded_hierarchy_before_callbacks_and_clears_commands() {
    auto app = App(EngineConfig {.appName = "TypedCleanupTest", .validationMode = ValidationMode::Off, .headless = true});
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        auto reg = engine.Entities();
        auto createBody = [&] {
            return engine.Physics().CreateRigidBody(Physics::Shape<Physics::Sphere> {}, {0, 10, 0}, {0, 0, 0, 1},
                                                     Physics::MotionType::Dynamic, Physics::ObjectLayer::Moving);
        };
        const auto body1 = createBody(), body2 = createBody(), body3 = createBody();
        Expect(body1 != Physics::BodyID::Null() && body2 != Physics::BodyID::Null() && body3 != Physics::BodyID::Null());
        CleanupProbe probe;
        probe.parent = reg.Spawn(Components::PhysicsComponent {.physicsID = body1});
        probe.child = reg.Spawn(Components::HierarchyComponent {.parent = probe.parent}, Components::PhysicsComponent {.physicsID = body2});
        probe.survivor = reg.Spawn(Components::PhysicsComponent {.physicsID = body3});
        activeCleanupProbe = &probe;
        Expect(engine.AddSceneCleanupPass(&ProbeCleanup));
        engine.Commands().DestroyEntity(probe.parent);
        engine.Commands().Playback();
        engine.ProcessPendingDestroy();
        Expect(probe.valid && probe.pendingCalls == 1 && !reg.IsAlive(probe.parent) && !reg.IsAlive(probe.child));
        Expect(reg.IsAlive(probe.survivor));
        engine.ProcessPendingDestroy();
        Expect(probe.pendingCalls == 1);
        engine.Commands().AddComponent(probe.survivor, PendingPhaseMarker {});
        engine.ClearScene();
        Expect(probe.valid && probe.clearCalls == 1 && !reg.IsAlive(probe.survivor) && engine.Commands().Empty());
        activeCleanupProbe = nullptr;
        ran = true;
    });
    const auto result = app.Run([](Engine& engine, float) { engine.GetPlatformHost().Close(); });
    ExpectUserClosed(result);
    Expect(ran);
}

#if defined(ZHLN_HAS_ANIMATION)
void test_animation_cleanup_callback_releases_only_pending_then_clears_all() {
    auto app = App(EngineConfig {.appName = "AnimationQueryCleanupTest", .validationMode = ValidationMode::Off, .headless = true});
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        Animation::Install(engine);
        auto animations = engine.Service<Animation::AnimationContext>();
        Expect(animations.has_value());
        if (!animations) return;
        auto registry = engine.Entities();
        const auto deadID = animations->Create(1);
        const auto liveID = animations->Create(2);
        const auto orphanID = animations->Create(3);
        const auto dead = registry.Spawn(Components::AnimatorComponent {.animationID = deadID});
        const auto live = registry.Spawn(Components::AnimatorComponent {.animationID = liveID});
        engine.Commands().DestroyEntity(dead);
        engine.Commands().Playback();
        engine.ProcessPendingDestroy();
        Expect(!registry.IsAlive(dead) && registry.IsAlive(live));
        Expect(!animations->Valid(deadID) && animations->Valid(liveID) && animations->Valid(orphanID));
        engine.ClearScene();
        Expect(!registry.IsAlive(live) && !animations->Valid(liveID) && !animations->Valid(orphanID));
        ran = true;
    });
    const auto result = app.Run([](Engine& engine, float) { engine.GetPlatformHost().Close(); });
    ExpectUserClosed(result);
    Expect(ran);
}
#endif

#if defined(ZHLN_HAS_EDITOR)
void test_editor_capability_crud_is_immediate_and_destruction_is_deferred() {
    auto app = App(EngineConfig {.appName = "EditorCapabilityTest", .validationMode = ValidationMode::Off, .headless = true});
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        auto entities = engine.Entities();
        Editor::EditorState state;
        const Entity entity = Editor::CreateEntity(entities, "Edited");
        Expect(entities.IsAlive(entity));
        Expect(entities.Get<Components::NameComponent>(entity).has_value());
        Expect(entities.Get<Components::TransformComponent>(entity).has_value());
        Expect(entities.Get<Components::WorldTransformComponent>(entity).has_value());
        bool foundPBR = false;
        for (const auto& kind: Editor::ComponentKinds()) {
            if (kind.name == "PBRComponent") {
                foundPBR = true;
                Expect(!kind.has(entities, entity));
                kind.add(entities, entity);
                Expect(kind.has(entities, entity));
                kind.remove(entities, entity);
                Expect(!kind.has(entities, entity));
            }
        }
        Expect(foundPBR);
        state.selectedEntity = entity;
        Editor::DestroySelected(engine, state);
        Expect(state.selectedEntity == Entity::Null());
        Expect(entities.IsAlive(entity));
        Expect(entities.Get<Components::PendingDestroy>(entity).has_value());
        engine.ProcessPendingDestroy();
        Expect(!entities.IsAlive(entity));
        ran = true;
    });
    ExpectUserClosed(app.Run([](Engine& engine, float) { engine.GetPlatformHost().Close(); }));
    Expect(ran);
}
#endif

#if defined(ZHLN_HAS_TERRAIN)
static float observedTerrainHeight = 0.0f;
void ObserveTerrainHeight(Terrain::TerrainHeightQuery query) {
    observedTerrainHeight = Terrain::TerrainSystem::SampleHeightAtWithQuery(query, 10.0f, 20.0f);
}

void test_terrain_typed_sampling_and_scene_cleanup() {
    auto app = App(EngineConfig {.appName = "TerrainQueryBoundaryTest", .validationMode = ValidationMode::Off, .headless = true});
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        using namespace Terrain;
        auto registry = engine.Entities();
        TerrainSystem::RegisterCleanup(engine);
        TerrainData data;
        data.sampleCount = 2;
        data.worldSize = 10.0f;
        data.heights.resize(4);
        data.heights[0] = 1.0f;
        data.heights[1] = 3.0f;
        data.heights[2] = 5.0f;
        data.heights[3] = 7.0f;
        const auto retired = TerrainSystem::RegisterTerrainData(std::move(data));
        const auto survivor = TerrainSystem::RegisterTerrainData({});
        Expect(retired != TerrainID::Invalid && survivor != TerrainID::Invalid);
        if (retired == TerrainID::Invalid || survivor == TerrainID::Invalid) {
            TerrainSystem::UnregisterTerrainData(retired);
            TerrainSystem::UnregisterTerrainData(survivor);
            return;
        }
        const Entity dead = registry.Spawn(TerrainComponent {.terrainID = retired}, Components::TransformComponent {.position = {10.0f, 4.0f, 20.0f}});
        const Entity live = registry.Spawn(TerrainComponent {.terrainID = survivor}, Components::TransformComponent {.position = {500.0f, 0.0f, 0.0f}});
        const Entity attached = registry.Create();
        const auto first = TerrainSystem::RegisterTerrainData({});
        const auto replacement = TerrainSystem::RegisterTerrainData({});
        TerrainSystem::Attach(engine, attached, TerrainComponent {.terrainID = first});
        TerrainSystem::Attach(engine, attached, TerrainComponent {.terrainID = replacement});
        Expect(TerrainSystem::GetTerrainData(first) == nullptr);
        Expect(TerrainSystem::GetTerrainData(replacement) != nullptr);
        TerrainSystem::Detach(engine, attached);
        Expect(TerrainSystem::GetTerrainData(replacement) == nullptr);
        Expect(!registry.Get<TerrainComponent>(attached));
        observedTerrainHeight = 0.0f;
        engine.GetUpdateGraph().InvokeSequential<&ObserveTerrainHeight>(Frame {});
        Expect(observedTerrainHeight == 8.0f);
        engine.Commands().DestroyEntity(dead);
        engine.Commands().Playback();
        engine.ProcessPendingDestroy();
        Expect(!registry.IsAlive(dead) && registry.IsAlive(live));
        Expect(TerrainSystem::GetTerrainData(retired) == nullptr && TerrainSystem::GetTerrainData(survivor) != nullptr);
        engine.ClearScene();
        Expect(!registry.IsAlive(live) && TerrainSystem::GetTerrainData(survivor) == nullptr);
        ran = true;
    });
    const auto result = app.Run([](Engine& engine, float) { engine.GetPlatformHost().Close(); });
    ExpectUserClosed(result);
    Expect(ran);
}
#endif

static RenderContext* observedRenderContext = nullptr;
void ObserveRenderContext(ECS::ResMut<RenderContext> render) { observedRenderContext = &*render; }

void test_render_graph_rebinds_renderer_after_device_recovery() {
    auto app = App(EngineConfig {.appName = "RenderServiceRecoveryTest", .validationMode = ValidationMode::Off, .headless = true});
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        auto& graph = engine.GetRenderGraph();
        const auto frame = engine.MakeFrame(0.0f, FrameAlpha {0.0f});
        const AssetID beforeProbe = HashAssetID("render_service_recovery_before");
        engine.Graphics().RegisterMesh(beforeProbe, Mesh {});
        graph.InvokeSequential<&ObserveRenderContext>(frame);
        Expect(observedRenderContext != nullptr && observedRenderContext->GetGPUMesh(beforeProbe).has_value());
        engine.Graphics().UnregisterMesh(beforeProbe);
        const uint64_t genBefore = engine.DeviceGeneration();
        const auto recovered = engine.HandleDeviceLost();
        Expect(recovered.has_value());
        if (!recovered) return;
        Expect(engine.DeviceGeneration() == genBefore + 1);
        const AssetID afterProbe = HashAssetID("render_service_recovery_after");
        engine.Graphics().RegisterMesh(afterProbe, Mesh {});
        observedRenderContext = nullptr;
        graph.InvokeSequential<&ObserveRenderContext>(frame);
        Expect(observedRenderContext != nullptr && observedRenderContext->GetGPUMesh(afterProbe).has_value() &&
               !observedRenderContext->GetGPUMesh(beforeProbe).has_value());
        engine.Graphics().UnregisterMesh(afterProbe);
        observedRenderContext = nullptr;
        ran = true;
    });
    const auto result = app.Run([](Engine& engine, float) { engine.GetPlatformHost().Close(); });
    ExpectUserClosed(result);
    Expect(ran);
}

void test_app_ui_only_layer_uses_engine_ui_phase() {
    std::vector<std::string_view> calls;
    auto app = App(EngineConfig {
        .appName        = "UIOnlyAppLayerTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    });
    app.AddLayer(UIOnlyPhaseLayer {.calls = &calls});
    const auto result = app.Run([&](Engine& engine, float) {
        RunHostUIPhaseForTest(engine);
        engine.GetPlatformHost().Close();
    });
    ExpectUserClosed(result);
    Expect(calls == std::vector<std::string_view> {"ui-only"});
}

void test_app_layers_render_ui_in_order_and_stop_before_engine_teardown() {
    std::vector<std::string_view> calls;
    auto app = App(EngineConfig {
        .appName        = "AppLayerLifecycleTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    });
    app.AddLayer(UILifecycleLayer {.state = std::make_unique<int>(1), .calls = &calls});
    app.AddLayer(UIOnlyPhaseLayer {.calls = &calls});
    app.AddLayer(StopOnlyPhaseLayer {.calls = &calls});
    app.AddLayer(HUDPhaseLayer {.calls = &calls});
    app.OnStart([&](Engine&) { calls.push_back("app-start"); });

    const auto result = app.Run([&](Engine& engine, float) {
        calls.push_back("update");
        RunHostUIPhaseForTest(engine);
        engine.GetPlatformHost().Close();
    });
    ExpectUserClosed(result);
    Expect(calls == std::vector<std::string_view> {
        "layer-start", "app-start", "layer-tick", "update",
        "layer-ui", "ui-only", "hud-ui", "stop-only", "layer-stop"
    });
}

void test_app_stops_layers_on_typed_frame_failure() {
    std::vector<std::string_view> calls;
    auto app = App(EngineConfig {
        .appName        = "AppLayerFrameFailureTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    });
    app.AddLayer(UILifecycleLayer {.state = std::make_unique<int>(1), .calls = &calls});
    app.OnStart([&](Engine& engine) {
        // Compose a deterministic failing step right after HostUICallback in
        // FramePhase::UI. FramePlan::Execute aborts at the first engaged error,
        // so UI runs and no GPU frame executes.
        static constexpr ModuleStep kFailSteps[] {
            ModuleStep {
                .phase     = FramePhase::UI,
                .name      = "AbortAfterHostUI",
                .run       = +[](Engine&, float, FrameContext& frame) { frame.error = ZHLN::ErrorCode(ZHLN::FrameError::StepFailed); },
                .placement = StepPlacement::After,
                .anchor    = "HostUICallback",
            },
        };
        const auto placed = engine.InstallModule(Module {.name = "FailAfterUI", .frameSteps = kFailSteps});
        Expect(placed.has_value());
    });
    const auto result = app.Run([&](Engine&, float) { calls.push_back("update"); });
    Expect(!result.has_value());
    if (!result) Expect(result.error().Is(AppError::FrameTickFailed));
    Expect(calls == std::vector<std::string_view> {"layer-start", "layer-tick", "update", "layer-ui", "layer-stop"});
}

void test_app_engine_config_service_is_available_before_install_and_scene_start() {
    EngineConfig config {
        .appName        = "ApplicationCliTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    };
    bool             actionRan = false;
    bool             prepared  = false;
    bool             installed = false;
    bool             started   = false;
    bool             updated   = false;
    std::string_view region;

    auto app = App(config);
    app.AddCliOption(
        "--region", "<name>", "Initial region",
        [&](std::string_view value) -> std::expected<void, CommandLineError> {
            region    = value;
            actionRan = true;
            return {};
        }
    );
    app.BeforeEngineCreate([&]() -> std::expected<void, ErrorCode> {
        Expect(actionRan && region == "atrium");
        Expect(!installed && !started);
        prepared = true;
        return {};
    });
    app.Install([&](Engine& engine) {
        Expect(prepared);
        auto engineConfig = engine.Service<EngineConfig>();
        Expect(engineConfig.has_value());
        if (engineConfig) {
            Expect(!engineConfig->vsync);
            Expect(engineConfig->headless);
            Expect(engineConfig->appName == config.appName);
        }
        Expect(actionRan);
        Expect(region == "atrium");
        installed = true;
    });
    app.OnStart([&](Engine& engine) {
        Expect(installed);
        Expect(engine.Service<EngineConfig>().has_value());
        started = true;
    });

    char program[] = "test-application";
    char option[]  = "--region=atrium";
    char vsync[]   = "--vsync=off";
    char* args[]   = {program, option, vsync};
    const auto result = app.Run(std::span<char* const>(args), [&](Engine& engine) {
        Expect(started);
        Expect(engine.Service<EngineConfig>().has_value());
        updated = true;
        engine.GetPlatformHost().Close();
    });
    ExpectUserClosed(result);
    Expect(prepared && installed && started && updated);
}

void test_kernel_app_owns_process_input_and_shutdown_without_an_ecs_world() {
    bool started = false;
    bool inputReset = false;
    bool updated = false;
    bool rendered = false;
    bool shutdown = false;
    const auto result = KernelApp(EngineConfig {
        .appName        = "KernelApplicationTest",
        .width          = 320,
        .height         = 180,
        .validationMode = ValidationMode::Off,
        .headless       = true,
    })
        .OnStart([&](Kernel&) { started = true; })
        .OnInputFrame([&](Kernel&, float) { inputReset = true; })
        .OnUpdate([&](Kernel& kernel, float dt) {
            Expect(started && inputReset);
            Expect(dt >= 0.0f);
            updated = true;
            kernel.GetPlatformHost().Close(); // no GPU frame or Window required
        })
        .OnRender([&](Kernel&, FrameScope&) { rendered = true; })
        .OnShutdown([&](Kernel&) { shutdown = true; })
        .Run();
    ExpectUserClosed(result);
    Expect(started && inputReset && updated && shutdown);
    Expect(!rendered);
}

struct ModuleSceneProbeLocal {
    int runs = 0;
};
static int g_moduleSceneProbeRuns = 0;
void ModuleSceneProbeSystem(ECS::Local<ModuleSceneProbeLocal> local) {
    g_moduleSceneProbeRuns = ++local->runs;
}
void AddModuleSceneProbe(ECS::SimGraph& updateGraph, ECS::RenderGraph&) {
    updateGraph.AddSystem<&ModuleSceneProbeSystem>();
}

void test_clear_scene_preserves_graphs_resets_locals_and_scene_instantiate_recreates_camera() {
    g_moduleSceneProbeRuns = 0;
    auto app = App(EngineConfig {
        .appName        = "SceneLifecycleSplitTest",
        .width          = 320,
        .height         = 180,
        .validationMode = ValidationMode::Off,
        .headless       = true,
    });
    app.Install([](Engine& engine) {
        Expect(engine.InstallModule(Module {
            .name             = "SceneLifecycleProbe",
            .addGraphSystems  = &AddModuleSceneProbe,
        }).has_value());
    });
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        const size_t updateCount = engine.GetUpdateGraph().GetSystemCount();
        Expect(updateCount > 0);

        engine.GetUpdateGraph().Execute(engine.MakeFrame(0.016f, FrameAlpha {0.0f}));
        engine.GetUpdateGraph().Execute(engine.MakeFrame(0.016f, FrameAlpha {0.0f}));
        Expect(g_moduleSceneProbeRuns == 2);

        engine.State().SetFreezeFrustum(true);
        engine.State().SetPhysicsDrawMode(2);
        Expect(engine.State().CullingStats().FreezeFrustum);
        Expect(engine.State().PhysicsDrawMode() == 2);

        // ClearScene keeps the compiled graphs intact, resets Local<T> blocks,
        // resets World culling/debug state, and re-seeds the baseline camera entity.
        engine.ClearScene();
        Expect(engine.GetUpdateGraph().GetSystemCount() == updateCount);
        Expect(!engine.State().CullingStats().FreezeFrustum);
        Expect(engine.State().PhysicsDrawMode() == 0);
        Expect(engine.Entities().SingletonEntity<Components::MainCameraTagComponent>() != Entity::Null());

        engine.GetUpdateGraph().Execute(engine.MakeFrame(0.016f, FrameAlpha {0.0f}));
        Expect(g_moduleSceneProbeRuns == 1);

        // Even if a caller despawns the main camera entity explicitly,
        // Scene::Instantiate recreates it and applies the authored pose.
        const Entity oldCam = engine.Entities().SingletonEntity<Components::MainCameraTagComponent>();
        engine.Entities().Despawn(oldCam);
        engine.ProcessPendingDestroy();
        Expect(engine.Entities().SingletonEntity<Components::MainCameraTagComponent>() == Entity::Null());

        Scene::Scene desc {};
        desc.camera.position = {3.0f, 4.0f, 5.0f};
        desc.camera.fov      = 72.0f;
        const auto instantiated = Scene::Instantiate(engine, desc);
        Expect(instantiated.has_value());
        const auto restoredCam = engine.Entities().GetSingleton<Components::CameraComponent>();
        Expect(restoredCam.has_value() && restoredCam->camera.position == FVec3 {3.0f, 4.0f, 5.0f});
        ran = true;
    });
    ExpectUserClosed(app.Run([](Engine& engine, float) { engine.GetPlatformHost().Close(); }));
    Expect(ran);
}

} // namespace ZHLN::Test
