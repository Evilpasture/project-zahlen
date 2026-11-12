// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Opt-in local GPU test: whole application launches. Most stop before the
// first Tick; a failure probe runs a UI-only frame without GPU rendering.
#include <Zahlen/Engine/Internal/EngineAccess.hpp>
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
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
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
        // ProcessEvents begins an input frame. If update runs before it, the
        // deliberately stale mouse delta below is still visible to gameplay.
        auto input = ZHLN::EngineAccess::RegistryOf(engine).GetSingleton<Components::InputStateComponent>();
        input->mouseDeltaX = 123.0f;
        // A resize is consumed during Engine::ProcessEvents, not by App's loop.
        input->ApplyResize({.width = 800, .height = 600});
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
        const auto input = engine.Entities().GetSingleton<Components::InputStateComponent>();
        Expect(input.has_value());
        Expect(input->mouseDeltaX == 0.0f);
        Expect(!input->needsResize);
        Expect(input->newSize.width == 800u);
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
    auto& reg = EngineAccess::RegistryOf(engine);
    probe.valid = probe.valid && dt == kFixedStepDt && probe.preCount == probe.postCount;
    if (probe.preCount == 0) {
        probe.valid = probe.valid && reg.Get<Components::ImpulseCommand>(probe.bodyEntity).has_value();
    } else {
        // Previous substep must already have consumed its impulse. A new
        // command issued now belongs to this substep, not the next frame.
        probe.valid = probe.valid && !reg.Get<Components::ImpulseCommand>(probe.bodyEntity);
        reg.Add(probe.bodyEntity, Components::ImpulseCommand {.linear = {10.0f, 0.0f, 0.0f}});
    }
    ++probe.preCount;
}

void ProbePostStep(Engine& engine) {
    auto& probe = *activeSubstepProbe;
    auto& reg = EngineAccess::RegistryOf(engine);
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
        auto& reg = EngineAccess::RegistryOf(engine);
        const auto body = engine.Physics().CreateRigidBody(Physics::Shape<Physics::Sphere> {}, {0, 10, 0}, {0, 0, 0, 1},
                                                           Physics::MotionType::Dynamic, Physics::ObjectLayer::Moving);
        Expect(body != Physics::BodyID::Null());
        SubstepProbe probe {
            .bodyEntity = reg.Create(Components::PhysicsComponent {.physicsID = body, .isStatic = false},
                                     Components::ImpulseCommand {.linear = {10.0f, 0.0f, 0.0f}}),
            .zeroEntity = reg.Create(Components::PhysicsComponent {.physicsID = body, .isStatic = false}, Components::ImpulseCommand {}),
            .orphanEntity = reg.Create(Components::ImpulseCommand {.linear = {5.0f, 0.0f, 0.0f}}),
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
        auto& reg = EngineAccess::RegistryOf(engine);
        const auto cameraEntity = reg.Create(Components::CameraComponent {});
        const auto transformEntity = reg.Create(Components::WorldTransformComponent {});
        auto world = reg.Get<Components::WorldTransformComponent>(transformEntity);
        world->world = CreateTranslation(FVec3 {3.0f, 2.0f, 1.0f});
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
        auto& reg = EngineAccess::RegistryOf(engine);
        auto raw = reg.GetSingleton<Components::InputStateComponent>();
        auto cam = reg.GetSingleton<Components::CameraComponent>();
        Expect(raw && cam);
        if (!raw || !cam) return;
        raw->SetKey(KeyCode::W, true);
        cam->camera.yaw = DegF {0.0f};
        const auto character = engine.Physics().CreateCharacter({0.0, 10.0, 0.0});
        Expect(character != Physics::BodyID::Null());
        const auto entity = reg.Create(Character::InputComponent {}, Character::MovementComponent {.isGrounded = false, .wasGrounded = false},
                                       Components::PhysicsComponent {.physicsID = character, .isStatic = false});
        FrameContext frame;
        steps[inputStep].run(engine, 0.0f, frame);
        Expect(reg.Get<Character::InputComponent>(entity)->localMoveZ == 1.0f);
        steps[intentStep].run(engine, 0.0f, frame);
        Expect(reg.Get<Character::MovementComponent>(entity)->inputX > 0.99f);
        frame.fixedSteps = 2;
        steps[physics].run(engine, 0.0f, frame);
        const auto move = reg.Get<Character::MovementComponent>(entity);
        Expect(move->currentVelX > 0.0f && move->currentYVel < -0.5f && !move->isGrounded);
        raw->SetKey(KeyCode::W, false);
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
        auto& reg = EngineAccess::RegistryOf(engine);
        const Entity main = reg.SingletonEntity<Components::MainCameraTagComponent>();
        auto input = reg.GetSingleton<Components::InputStateComponent>();
        Expect(main != Entity::Null() && input);
        if (main == Entity::Null() || !input) return;
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
        const auto target = reg.Create(Components::TransformComponent {.position = {4.0f, 3.0f, 2.0f}});
        rig->target = target;
        rig->stiffness = 0.0f;
        rig->yaw = 0.0f;
        rig->pitch = 0.0f;
        rig->targetOffset = FVec3::Zero();
        rig->distance = rig->targetDistance = 2.0f;
        rigStep->run(engine, 0.1f, frame);
        Expect(camera->camera.position == originalPosition); // FreeCam owns the pose.
        FreeCam::Detach(engine);
        freeStep->run(engine, 0.1f, frame);
        Expect(!reg.Get<FreeCam::FreeCamTagComponent>(main)); // opt-out prevents reseeding
        rigStep->run(engine, 0.1f, frame);
        Expect((camera->camera.position - FVec3 {2.0f, 3.0f, 2.0f}).LengthSquared() < 1e-6f);
        FreeCam::Attach(engine, 4.0f, false);
        input->SetKey(KeyCode::W, true);
        const auto before = camera->camera.position;
        FreeCam::Apply(engine, 0.25f); // paused-editor direct call remains supported
        Expect((camera->camera.position - before).LengthSquared() > 0.9f);
        input->SetKey(KeyCode::W, false);
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
        auto& reg = EngineAccess::RegistryOf(engine);
        auto stats = reg.GetSingleton<Components::CullingStatsComponent>();
        auto camera = reg.GetSingleton<Components::CameraComponent>();
        Expect(present && present->run && stats && camera);
        if (!present || !present->run || !stats || !camera) return;
        auto& scene = EngineAccess::SceneOf(engine);
        FrameContext frame;
        stats->stats.FreezeFrustum = false;
        scene.Clear();
        present->run(engine, 0.0f, frame);
        Expect(scene.lines.empty());
        stats->stats.FreezeFrustum = true;
        camera->frozenViewProj = Mat4x4::Identity();
        scene.Clear();
        present->run(engine, 0.0f, frame);
        Expect(scene.lines.size() == 12);
        if (scene.lines.size() == 12) {
            Expect(scene.lines[0].start == FVec3 {-1.0f, -1.0f, 0.0f});
            Expect(scene.lines[0].end == FVec3 {1.0f, -1.0f, 0.0f});
        }
        stats->stats.FreezeFrustum = false;
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
    auto& reg = EngineAccess::RegistryOf(engine);
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
        auto& reg = EngineAccess::RegistryOf(engine);
        auto createBody = [&] {
            return engine.Physics().CreateRigidBody(Physics::Shape<Physics::Sphere> {}, {0, 10, 0}, {0, 0, 0, 1},
                                                     Physics::MotionType::Dynamic, Physics::ObjectLayer::Moving);
        };
        const auto body1 = createBody(), body2 = createBody(), body3 = createBody();
        Expect(body1 != Physics::BodyID::Null() && body2 != Physics::BodyID::Null() && body3 != Physics::BodyID::Null());
        CleanupProbe probe;
        probe.parent = reg.Create(Components::PhysicsComponent {.physicsID = body1});
        probe.child = reg.Create(Components::HierarchyComponent {.parent = probe.parent}, Components::PhysicsComponent {.physicsID = body2});
        probe.survivor = reg.Create(Components::PhysicsComponent {.physicsID = body3});
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

static RenderContext* observedRenderContext = nullptr;
void ObserveRenderContext(ECS::ResMut<RenderContext> render) { observedRenderContext = &*render; }

void test_render_graph_rebinds_renderer_after_device_recovery() {
    auto app = App(EngineConfig {.appName = "RenderServiceRecoveryTest", .validationMode = ValidationMode::Off, .headless = true});
    bool ran = false;
    app.OnStart([&](Engine& engine) {
        auto& graph = engine.GetRenderGraph();
        const auto frame = engine.MakeFrame(0.0f, FrameAlpha {0.0f});
        graph.InvokeSequential<&ObserveRenderContext>(frame);
        Expect(observedRenderContext == &EngineAccess::RenderOf(engine));
        const auto recovered = engine.HandleDeviceLost();
        Expect(recovered.has_value());
        if (!recovered) return;
        observedRenderContext = nullptr;
        graph.InvokeSequential<&ObserveRenderContext>(frame);
        Expect(observedRenderContext == &EngineAccess::RenderOf(engine));
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
        // Replace the plan with a single deterministic failing UI step. This
        // tests the frame-error result and cleanup without a GPU frame.
        // Failure injection is in-tree tooling: the mutable plan is reachable
        // only through the build-tree-only seam, never from the public API.
        auto& plan = ZHLN::EngineAccess::FramePlanOf(engine);
        plan.Clear();
        const auto placed = plan.Append(FramePhase::UI, "EmitUIAndFail", +[](Engine& active, float, FrameContext& frame) {
            if (const auto cb = active.GetUICallback()) {
                (*cb)(active);
            }
            frame.error = ZHLN::ErrorCode(ZHLN::FrameError::StepFailed);
        });
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

} // namespace ZHLN::Test
