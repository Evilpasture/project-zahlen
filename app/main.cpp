// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: app/main.cpp
//
// The composition root. It lives outside src/ because wiring an engine together
// means naming the optional layers it runs with, and the core library may not
// know optional layers exist. See include/ARCHITECTURE.md 1.2.
#include "engine/Platform.hpp"
#include "engine/system/GraphicsSettingsSync.hpp"
// clang-format off
#include <Zahlen/Physics/Jolt.hpp>
// clang-format on
#include <Jolt/Physics/Collision/CastResult.h>
#include <CommandLine/EngineCommandLine.hpp>
#include <Signal/CrashHandler.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/FramePlan.hpp>
#include <Zahlen/GameModule.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/gui/GUI.hpp>
#if defined(ZHLN_HAS_EDITOR)
#include <editor/GUIEditor.hpp>
#endif
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render/Info.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#if defined(ZHLN_HAS_SCENE_TOML)
// The document layer is an optional plugin, and the composition root is the one
// place allowed to name it: Core may not reach into any optional layer
// (configure/check_core_layer_boundary.py). SceneTOML.hpp is what turns a core
// ZHLN::Scene::Scene into a document, via its Jolt vector bindings.
#include <toml/SceneTOML.hpp>
#include <toml/TOML.hpp>
#endif
#if defined(ZHLN_HAS_LUA)
#include <lua/ReflectLua.hpp>
#include <lua/SceneLua.hpp>
#endif
#if defined(ZHLN_HAS_CHARACTER_CONTROLLER)
#include <CharacterController/CharacterController.hpp>
#endif
#if defined(ZHLN_HAS_CAMERA)
#include <Camera/TargetCamera.hpp>
#endif
#if defined(ZHLN_HAS_INTERACTION)
#include <Interaction/InteractionSystem.hpp>
#endif
#if defined(ZHLN_HAS_TERRAIN)
#include <Terrain/TerrainSystem.hpp>
#endif
#if defined(ZHLN_HAS_ANIMATION)
#include <Animation/IK.hpp>
#endif
#if defined(ZHLN_HAS_FALLBACK_SCENE)
#include <FallbackScene/FallbackScene.hpp>
#endif
#if defined(ZHLN_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif
#if defined(ZHLN_HAS_FREECAM)
#include <FreeCam/FreeCam.hpp>
#endif
#if defined(ZHLN_HAS_BENCHMARK)
// The logic benchmark extension, both halves: the headless LogicBenchmark that
// --benchmark runs, and the viewer that hosts the F3 panel. A build with
// ZHLN_BUILD_EXTRAS=OFF has neither, which is why every use below is guarded.
#include <Benchmark/Benchmark.hpp>
#include <Benchmark/Viewer/BenchmarkViewer.hpp>
#endif
#if defined(ZHLN_HAS_RAGDOLL_AUTHORING)
#include <RagdollAuthoring/RagdollAuthoring.hpp>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <thread>

namespace {

// The native host runs with the gameplay layers that moved out of core.
//
// This is the composition root, and the list below *is* the composition: each
// Install applies a Module -- the components it owns, the frame steps it
// declares, the graph systems it adds -- and the engine keeps what it was
// given rather than a list of callbacks to replay. Nothing here reaches into
// the engine's schedule: a module's steps are placed by name against core's,
// and a step whose anchor is gone is reported instead of appended somewhere
// else. A build with ZHLN_BUILD_EXTRAS=OFF installs nothing and the engine runs
// bare.
//
// Order matters for the graphs: the character controller declares the
// external-writes anchor for MovementComponent, and the interaction system
// reads it, so the anchor must be registered first for hazard analysis to point
// the right way. Terrain installs last so its update-graph node appends after
// Interaction's, reproducing the core wiring's Audio → Interaction → Particle →
// Terrain order. Order does *not* matter for the frame steps any more -- they
// anchor to core's step names -- so a host may install these in any order it
// likes and get the same timeline.
void InstallGameplayExtras(ZHLN::Engine& engine) {
#if defined(ZHLN_HAS_FONTS)
    // Fonts are first-class assets with an AssetID: load the baked font into
    // AssetManager's font cache. The asset cache outranks the embedded default;
    // device-loss rebuilds re-upload from the cached asset. No TTF parsing at
    // runtime.
    //
    // The source names the font this repo vendors as its default -- JetBrains
    // Mono NF, baked with fontbm into resources/fonts/ (see
    // Fonts::VendoredDefaultFontSource) -- resolved by path, so the host must be
    // started somewhere FindDataFile can see the checkout. It also has to beat
    // the pak's cooked default, which zcook fills with the Font8x8 placeholder;
    // hence the pair-first source. Without the vendored file the chain is that
    // placeholder, then core's embedded bake. Installing the loader here, before
    // InitializeDefaultScene, is what decides which bake the boot atlas uses.
    auto fontID = ZHLN::Fonts::LoadFontAsset(engine, ZHLN::Fonts::VendoredDefaultFontSource());
    if (!fontID) {
        ZHLN::LogWarning("Font asset failed to load ({}), using embedded default.", fontID.error());
    }
#endif
#if defined(ZHLN_HAS_CHARACTER_CONTROLLER)
    ZHLN::Character::Install(engine);
#endif
#if defined(ZHLN_HAS_CAMERA)
    // Third-person target camera rig: registers its component and re-inserts
    // its frame step before the core CameraSystems step, and re-seeds the
    // boot camera's rig component across scene resets.
    ZHLN::CameraRig::Install(engine);
#endif
#if defined(ZHLN_HAS_INTERACTION)
    ZHLN::Interaction::Install(engine);
#endif
#if defined(ZHLN_HAS_TERRAIN)
    ZHLN::Terrain::Install(engine);
#endif
#if defined(ZHLN_HAS_ANIMATION)
    // Installs AnimationContext + AnimationSystem and TwoBoneIKSystem.
    ZHLN::IK::Install(engine);
#endif
#if defined(ZHLN_HAS_FALLBACK_SCENE)
    // Re-inserts the boot-failure detection step at its original schedule
    // position and subscribes the preset's teardown hook.
    ZHLN::FallbackScene::Install(engine);
#endif
#if defined(ZHLN_HAS_FREECAM)
    ZHLN::FreeCam::Install(engine);
#endif
#if defined(ZHLN_HAS_RAGDOLL_AUTHORING)
    ZHLN::RagdollAuthoring::Install(engine);
#endif
}

struct HostLaunchConfig {
    std::filesystem::path scenePath;
    std::filesystem::path graphicsConfigPath = "config/graphics.lua";
};

HostLaunchConfig s_HostLaunchConfig;

struct HostStateService {
    ZHLN::FS::FileWatchHandle graphicsWatch = 0;
    bool                      bootApplied   = false;
};

[[nodiscard]] auto LoadSceneDocument(ZHLN::Engine& engine, const std::filesystem::path& path) -> bool {
    const std::string ext = path.extension().string();
#if defined(ZHLN_HAS_LUA)
    if (ext == ".lua") {
        auto parsed = ZHLN::ReflectLua::EvaluateFile<ZHLN::Scene::Scene>(path);
        if (!parsed) {
            ZHLN::LogError("[Scene] failed to evaluate Lua scene '{}': {}", path.string(), parsed.error());
            return false;
        }
        engine.ClearScene();
        auto inst = ZHLN::Scene::Instantiate(engine, *parsed);
        if (!inst) {
            ZHLN::LogError("[Scene] failed to instantiate Lua scene '{}': {}", path.string(), inst.error());
            return false;
        }
        ZHLN::Log("[Scene] instantiated '{}' ({} entities, {} lights)", path.string(), inst->entities.size(), inst->lights.size());
        return true;
    }
#endif
#if defined(ZHLN_HAS_SCENE_TOML)
    if (ext == ".toml") {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            ZHLN::LogError("[Scene] could not open TOML scene '{}'", path.string());
            return false;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto        parsed = ZHLN::ReflectTOML::Parse<ZHLN::Scene::Scene>(text);
        if (!parsed) {
            ZHLN::LogError("[Scene] failed to parse TOML scene '{}': {}", path.string(), parsed.error());
            return false;
        }
        engine.ClearScene();
        auto inst = ZHLN::Scene::Instantiate(engine, *parsed);
        if (!inst) {
            ZHLN::LogError("[Scene] failed to instantiate TOML scene '{}': {}", path.string(), inst.error());
            return false;
        }
        ZHLN::Log("[Scene] instantiated '{}' ({} entities, {} lights)", path.string(), inst->entities.size(), inst->lights.size());
        return true;
    }
#endif
    ZHLN::LogError("[Scene] unsupported or disabled scene format for '{}'", path.string());
    return false;
}

#if defined(ZHLN_HAS_LUA)
void ApplyGraphicsProfileFromLua(ZHLN::Engine& engine, const std::filesystem::path& path) {
    auto source = ZHLN::ReflectLua::ReadLuaFileText(path);
    if (!source) {
        return;
    }
    const std::string      chunkName = "@" + path.generic_string();
    const ZHLN::RenderInfo hw        = engine.GetRenderContext().GetInfo();

    const auto applyOverlay = [&](ZHLN::GraphicsSettings& dst) -> std::expected<void, ZHLN::ErrorCode> {
        auto callRes = ZHLN::ReflectLua::ApplyCallPatchString(dst, *source, chunkName, ZHLN::LuaBudget {}, hw);
        if (!callRes && callRes.error().Is(ZHLN::LuaError::NotAFunction)) {
            return ZHLN::ReflectLua::ApplyPatchString(dst, *source, chunkName);
        }
        return callRes;
    };

    const ZHLN::GraphicsSettings base    = ZHLN::CollectGraphicsSettings(engine);
    ZHLN::GraphicsSettings       staging = base;
    if (auto res = applyOverlay(staging); !res) {
        ZHLN::LogWarning("[Graphics] '{}' rejected ({}); keeping live settings.", path.string(), res.error());
        return;
    }

    if (staging.qualityPreset != ZHLN::QualityLevel::Custom && staging.qualityPreset != base.qualityPreset) {
        ZHLN::GraphicsSettings presetLayered = base;
        presetLayered.ApplyPreset(staging.qualityPreset);
        if (applyOverlay(presetLayered)) {
            staging = presetLayered;
        }
    }

    ZHLN::ApplyGraphicsSettings(engine, staging);
    ZHLN::Log("[Graphics] applied declarative profile '{}' (preset={}).", path.string(), staging.qualityPreset);
}
#endif

void BootHostStateStep(ZHLN::Engine& engine, float /*dt*/, ZHLN::FrameContext& /*ctx*/) {
    auto svc = engine.Service<HostStateService>();
    if (!svc || svc->bootApplied) {
        return;
    }
    svc->bootApplied = true;

    if (!s_HostLaunchConfig.scenePath.empty()) {
        if (!LoadSceneDocument(engine, s_HostLaunchConfig.scenePath)) {
            ZHLN::LogWarning("[Scene] falling back to default scene after '{}' failed.", s_HostLaunchConfig.scenePath.string());
        }
    }

#if defined(ZHLN_HAS_LUA)
    if (!s_HostLaunchConfig.graphicsConfigPath.empty() && std::filesystem::exists(s_HostLaunchConfig.graphicsConfigPath)) {
        const std::filesystem::path cfgPath = s_HostLaunchConfig.graphicsConfigPath;
        ApplyGraphicsProfileFromLua(engine, cfgPath);
        svc->graphicsWatch = engine.GetFileSystemWatcher().WatchFile(cfgPath, [&engine, cfgPath](const ZHLN::FS::FileWatchEvent& ev) {
            if (ev.action != ZHLN::FS::FileWatchAction::Deleted) {
                ApplyGraphicsProfileFromLua(engine, cfgPath);
            }
        });
    }
#endif
}

void TeardownHostState(ZHLN::Engine& engine) {
    if (auto svc = engine.Service<HostStateService>(); svc && svc->graphicsWatch != 0) {
        if (!engine.GetFileSystemWatcher().Unwatch(svc->graphicsWatch)) {
            ZHLN::LogWarning("[Graphics] could not release file watch handle {}.", svc->graphicsWatch);
        }
        svc->graphicsWatch = 0;
    }
}

void InstallHostWithState(ZHLN::Engine& engine) {
    InstallGameplayExtras(engine);
    engine.EmplaceService<HostStateService>();

    static constexpr ZHLN::ModuleStep kSteps[] = {
        {
            .phase     = ZHLN::FramePhase::HotReload,
            .name      = "HostStateBoot",
            .run       = &BootHostStateStep,
            .placement = ZHLN::StepPlacement::After,
            .anchor    = "StateAndShaderReload",
        },
    };
    const ZHLN::Module mod {
        .name       = "HostState",
        .frameSteps = kSteps,
        .teardown   = &TeardownHostState,
    };
    if (auto res = engine.InstallModule(mod); !res) {
        ZHLN::LogError("[HostState] module install failed: {}", ZHLN::ToString(res.error()));
    }
}

#if defined(ZHLN_HAS_BENCHMARK)

// `zahlen --benchmark`: the headless half of extensions/Benchmark, run once
// and reported.
//
// It builds no window, no device and no scene -- the benchmark owns a
// standalone World -- which is what makes the flag usable on a machine with no
// display and the number comparable across machines. Workload sizes come from
// the environment so a CI job can pin them without a rebuild:
//
//   ZHLN_BENCH_ENTITIES=4000 ZHLN_BENCH_TICKS=20000 ./build/current/bin/zahlen --benchmark
//
// The sizes below are the defaults: big enough that a tick is dominated by the
// engine rather than by the timer's resolution, small enough that a workstation
// finishes in a few seconds.
inline constexpr size_t   kBenchmarkEntities = 2000;
inline constexpr uint64_t kBenchmarkTicks    = 5000;

[[nodiscard]] auto BenchmarkEnvU64(const char* name, uint64_t fallback) -> uint64_t {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char*                    end    = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return (end != value) ? static_cast<uint64_t>(parsed) : fallback;
}

// Runs the workload and prints one report. False only when the benchmark's
// world could not be created; the reason is logged by the extension itself.
[[nodiscard]] auto RunLogicBenchmark() -> bool {
    const auto entities = static_cast<size_t>(BenchmarkEnvU64("ZHLN_BENCH_ENTITIES", kBenchmarkEntities));
    const auto ticks    = BenchmarkEnvU64("ZHLN_BENCH_TICKS", kBenchmarkTicks);

    ZHLN::Bench::LogicBenchmark benchmark;
    if (!benchmark.Initialize(entities)) {
        return false;
    }

    // The fixed step a game loop would use; the reported cost is per call, so
    // the value only decides how far the world moves between measurements.
    constexpr float kFixedDt = 1.0f / 60.0f;
    for (uint64_t tick = 0; tick < ticks; ++tick) {
        benchmark.Step(kFixedDt);
    }

    const auto& metrics = benchmark.GetMetrics();
    // Engine-only rate: measured tick time inverted. The wall rate below also
    // carries whatever the host spent between Step() calls, and here that is
    // the loop above and nothing else.
    const double engineRate = metrics.avgTickTimeMs > 0.0f ? 1000.0 / static_cast<double>(metrics.avgTickTimeMs) : 0.0;
    const double wallRate   = metrics.totalTimeSec > 0.0 ? static_cast<double>(metrics.totalTicks) / metrics.totalTimeSec : 0.0;

    ZHLN::Log<ZHLN::LogChannel::StdOut>("[Bench] logic benchmark: {} entities, {} ticks", metrics.entityCount, metrics.totalTicks);
    ZHLN::Log<ZHLN::LogChannel::StdOut>(
        "[Bench] tick ms: avg {:.3f}  min {:.3f}  max {:.3f}  last {:.3f}", metrics.avgTickTimeMs, metrics.minTickTimeMs, metrics.maxTickTimeMs,
        metrics.lastTickTimeMs
    );
    ZHLN::Log<ZHLN::LogChannel::StdOut>(
        "[Bench] throughput: {:.0f} ticks/s engine-only, {:.0f} ticks/s wall ({:.2f} s total)", engineRate, wallRate, metrics.totalTimeSec
    );
    return true;
}

// F3 hosts the benchmark panel in the app's own window: the viewer half of the
// same extension, with this app's loop as the host loop.
//
// The panel owns the UI frame on the frames it draws (UpdateAndRenderUI does
// the BeginFrame/EndFrame round trip), so this callback is the whole UI story
// while the panel is up. An app with a HUD of its own would draw that HUD in
// this same callback and call the viewer's DrawPanel between its own BeginFrame
// and EndFrame instead -- one Clay frame, two authors.
[[nodiscard]] auto MakeBenchmarkOverlay() -> ZHLN::Engine::UICallback {
    // Shared rather than captured by value: the UICallback is copied around by
    // the engine, and the viewer is a single long-lived object (it owns the
    // benchmark's world) that must not be duplicated.
    struct OverlayState {
        ZHLN::Bench::BenchmarkViewer viewer;
        ZHLN::Clock                  clock;
        bool                         panelKeyWasDown     = false;
        bool                         telemetryKeyWasDown = false;
    };
    auto state = std::make_shared<OverlayState>();

    return [state](ZHLN::Engine& engine) -> void {
        // Raw level plus an edge, like the samples' toggles: no text field or
        // console in this app captures the keyboard.
        const auto input = engine.GetRegistry().GetSingleton<ZHLN::Components::InputStateComponent>();

        const bool panelKeyDown = input && input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::F3));
        if (panelKeyDown && !state->panelKeyWasDown) {
            state->viewer.ToggleVisible();
            ZHLN::Log("[Bench] logic benchmark panel {}.", state->viewer.IsVisible() ? "on" : "off");
        }
        state->panelKeyWasDown = panelKeyDown;

        // F4: the frame telemetry panel -- average FPS, the 1% / 0.1% frame-time
        // lows, and the GPU stage timings the renderer records every frame. Its
        // capture button writes the same numbers to a CSV under build/cache.
        const bool telemetryKeyDown = input && input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::F4));
        if (telemetryKeyDown && !state->telemetryKeyWasDown) {
            state->viewer.ToggleTelemetryVisible();
            ZHLN::Log("[Bench] frame telemetry panel {}.", state->viewer.IsTelemetryVisible() ? "on" : "off");
        }
        state->telemetryKeyWasDown = telemetryKeyDown;

        // One frame of bookkeeping whether or not anything is drawn: the frame
        // window and the profiler rows stay warm, so pressing F3 or F4 shows a
        // populated panel instead of one that starts on that frame. It is
        // idempotent per engine frame, so UpdateAndRenderUI ticking again below
        // is not a second sample, and it steps the benchmark only while that
        // panel is visible -- a hidden panel is not a background load.
        state->viewer.Tick(engine);

        if (!state->viewer.IsVisible() && !state->viewer.IsTelemetryVisible()) {
            return;
        }
        state->viewer.UpdateAndRenderUI(engine, state->clock.Tick());
    };
}

#endif // ZHLN_HAS_BENCHMARK

#if defined(ZHLN_HAS_EDITOR)
// WORLD EDITOR

struct EditorState {
    bool         simulationRunning = false;
    ZHLN::Entity selectedEntity    = ZHLN::Entity::Null();
    bool         freeCamActive     = true;
    float        freeCamSpeed      = 25.0f;
};

EditorState s_EditorState;

// --- Native (self-hosted) editor state
ZHLN::Editor::EditorState s_NativeEditorState;

// Where Ctrl+S writes. The editor has no notion of "the current scene" yet --
// nothing opens a file, so nothing knows its name -- and one predictable path
// next to the working directory beats inventing a session concept here.
constexpr std::string_view kSceneSavePath = "scene.toml";

// Extracts the world and writes it back out as a scene document.
//
// This is the round trip closing: Scene::Instantiate built the world from a
// description, Scene::Extract reads a description back out of the world, and
// the reflection-driven TOML layer turns that into text. Nothing here lists
// fields -- the description struct is the schema in both directions.
void SaveScene(ZHLN::Engine& engine) {
    auto scene = ZHLN::Scene::Extract(engine);

    // A running world carries no scene name; the file it is being written to is
    // the only name on offer.
    scene.name = std::filesystem::path(kSceneSavePath).stem().string();

#if defined(ZHLN_HAS_SCENE_TOML)
    const std::string text = ZHLN::ReflectTOML::SerializeTOML(scene);

    std::ofstream out {std::string {kSceneSavePath}, std::ios::binary | std::ios::trunc};
    if (!out) {
        ZHLN::Log("[WorldEditor] Ctrl+S: could not open '{}' for writing", kSceneSavePath);
        return;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (out.fail()) {
        ZHLN::Log("[WorldEditor] Ctrl+S: writing '{}' failed", kSceneSavePath);
        return;
    }

    ZHLN::Log("[WorldEditor] Ctrl+S: '{}' written ({} entities, {} lights)", kSceneSavePath, scene.entities.size(), scene.lights.size());
#else
    ZHLN::Log(
        "[WorldEditor] Ctrl+S: built without the TOML layer, so there is nothing to write the description through "
        "({} entities, {} lights extracted)",
        scene.entities.size(), scene.lights.size()
    );
#endif
}

constexpr float kLeftPanelWidth  = 260.0f;
constexpr float kRightPanelWidth = 320.0f;

ZHLN::Physics::RaycastResult CastPickingRay(ZHLN::Engine& engine, const ZHLN::Camera& cam, const ZHLN::RenderContext::ViewportRect& vp) {
    auto& reg    = engine.GetRegistry();
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    if (auto st = reg.GetSingleton<ZHLN::Components::InputStateComponent>()) {
        mouseX = st->mouseX;
        mouseY = st->mouseY;
    }

    if (vp.width == 0 || vp.height == 0) {
        return {};
    }

    // The projection maps to Vulkan Y-down clip space (see CreatePerspective),
    // and the scene rectangle's row 0 is the top: the pointer is made
    // rectangle-relative before the NDC map, because the scene is rasterized
    // by a fixed-function viewport into exactly this rectangle. (The old
    // 1 - 2y/h mirrored the ray vertically, so picking -- and the transform
    // modes that copied this math -- aimed at the point mirrored across the
    // horizontal centre line.)
    float ndcX   = (2.0f * (mouseX - static_cast<float>(vp.x))) / static_cast<float>(vp.width) - 1.0f;
    float ndcY   = (2.0f * (mouseY - static_cast<float>(vp.y))) / static_cast<float>(vp.height) - 1.0f;
    float aspect = static_cast<float>(vp.width) / static_cast<float>(vp.height);

    JPH::Mat44 invVP = (cam.GetProjectionMatrix(aspect) * cam.GetViewMatrix()).Inversed();

    JPH::Vec4 nearWorld = invVP * JPH::Vec4(ndcX, ndcY, 0.0f, 1.0f);
    JPH::Vec4 farWorld  = invVP * JPH::Vec4(ndcX, ndcY, 1.0f, 1.0f);

    JPH::Vec3 pNear = JPH::Vec3(nearWorld.GetX() / nearWorld.GetW(), nearWorld.GetY() / nearWorld.GetW(), nearWorld.GetZ() / nearWorld.GetW());
    JPH::Vec3 pFar  = JPH::Vec3(farWorld.GetX() / farWorld.GetW(), farWorld.GetY() / farWorld.GetW(), farWorld.GetZ() / farWorld.GetW());
    JPH::Vec3 dir   = (pFar - pNear).Normalized();

    return engine.GetPhysicsContext().Raycast(JPH::RVec3(pNear), dir, 1000.0f);
}

// Draws one frame of the self-hosted editor using the Clay layout engine:
// [Hierarchy | Viewport Toolbar | Inspector]
void RunNativeEditorFrame(ZHLN::GUI::Context& gui, ZHLN::Engine& engine, float dt) {
    auto& reg = engine.GetRegistry();
    gui.SetFontAtlas(engine.GetFontAtlas());
    ZHLN::GUI::Frame ui = gui.BeginFrame(engine.ConsumeUIInput(dt));

    ui.Box(
        "EditorWorkspace",
        ZHLN::GUI::BoxConfig {
            .width     = {.grow = 1.0f},
            .height    = {.grow = 1.0f},
            .color     = {0.0f, 0.0f, 0.0f, 0.0f}, // Transparent workspace over 3D scene
            .direction = ZHLN::GUI::Direction::Row
        },
        [&]() -> void {
            // Left: Hierarchy panel
            ui.Box(
                "HierarchyPanel",
                ZHLN::GUI::BoxConfig {
                    .width        = {.fixed = kLeftPanelWidth},
                    .height       = {.grow = 1.0f},
                    .color        = {0.07f, 0.09f, 0.13f, 0.95f},
                    .cornerRadius = {0.0f, 0.0f, 0.0f, 0.0f},
                    .padding      = 10.0f,
                    .gap          = 6.0f,
                    .direction    = ZHLN::GUI::Direction::Column
                },
                [&]() -> void { ZHLN::Editor::DrawHierarchyPanel(ui, engine, s_NativeEditorState, "Hierarchy"); }
            );

            // Center: Viewport overlay toolbar
            ui.Box(
                "CenterViewportOverlay",
                ZHLN::GUI::BoxConfig {
                    .width     = {.grow = 1.0f},
                    .height    = {.grow = 1.0f},
                    .color     = {0.0f, 0.0f, 0.0f, 0.0f}, // Transparent overlay
                    .padding   = 10.0f,
                    .gap       = 8.0f,
                    .direction = ZHLN::GUI::Direction::Column
                },
                [&]() -> void {
                    // Top toolbar in center viewport
                    ui.Box(
                        "ViewportToolbar",
                        ZHLN::GUI::BoxConfig {
                            .width        = {.grow = 0.0f, .fit = true},
                            .height       = {.fixed = 36.0f},
                            .color        = {0.08f, 0.10f, 0.14f, 0.90f},
                            .cornerRadius = {6.0f, 6.0f, 6.0f, 6.0f},
                            .padding      = 8.0f,
                            .gap          = 12.0f,
                            .direction    = ZHLN::GUI::Direction::Row,
                            .alignMain    = ZHLN::GUI::Alignment::Center,
                            .alignCross   = ZHLN::GUI::Alignment::Center
                        },
                        [&]() -> void { ui.Checkbox("Simulate", s_EditorState.simulationRunning); }
                    );
                }
            );

            // Right: Inspector panel
            ui.Box(
                "InspectorPanel",
                ZHLN::GUI::BoxConfig {
                    .width        = {.fixed = kRightPanelWidth},
                    .height       = {.grow = 1.0f},
                    .color        = {0.07f, 0.09f, 0.13f, 0.95f},
                    .cornerRadius = {0.0f, 0.0f, 0.0f, 0.0f},
                    .padding      = 10.0f,
                    .gap          = 6.0f,
                    .direction    = ZHLN::GUI::Direction::Column
                },
                [&]() -> void { ZHLN::Editor::DrawInspectorPanel(ui, reg, s_NativeEditorState, "Inspector"); }
            );
        }
    );

    engine.SubmitUI(std::move(ui).End());
}

int RunWorldEditor(ZHLN::Engine& engine, const ZHLN::CommandLineOptions& options, ZHLN::CrashState& crashState) {
    ZHLN::Clock clock;
    // The editor poses the world's camera: a world with no main camera entity has
    // none to pose.
    auto cameraComp = engine.GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
    if (!cameraComp) {
        return EXIT_FAILURE;
    }
    ZHLN::Camera& cam = cameraComp->camera;

    cam.position = {0.0f, 20.0f, 40.0f};
    cam.yaw      = -90.0f;
    cam.pitch    = -20.0f;

    const double targetFrameTime = options.fpsLimit > 0 ? 1.0 / static_cast<double>(options.fpsLimit) : 0.0;
    auto         frameStart      = std::chrono::high_resolution_clock::now();

    ZHLN::Log("[WorldEditor] Editor session launched.");

    bool saveChordWasDown = false;
    bool escWasDown       = false;

    ZHLN::GUI::Context gui(engine.GetFontAtlas());

    while (engine.IsRunning()) {
        float frameTime = clock.Tick();
        ZHLN::CheckForCrashes(crashState, &engine);
        engine.ProcessEvents();

        auto& reg   = engine.GetRegistry();
        auto  state = reg.GetSingleton<ZHLN::Components::InputStateComponent>();

        auto winSize = engine.GetPlatformHost().GetSize();

        // Dynamic scene viewport: the 3D composition targets the centre
        // column between the two panels. Read the panels' REAL boxes from last
        // frame's GUI layout; the constants are only the first-frame fallback
        // before any layout exists. The rectangle goes to the RenderContext as
        // a fixed-function viewport + scissor on the scene passes, so nothing
        // is rasterized outside it; the camera aspect, GPU culling screen
        // space, picking and transform unprojection all read the same
        // rectangle back through GetViewport.
        float vpX0 = kLeftPanelWidth;
        float vpX1 = static_cast<float>(winSize.width) - kRightPanelWidth;
        if (const auto left = gui.GetLastFrameRect("HierarchyPanel")) {
            vpX0 = left->x + left->width;
        }
        if (const auto right = gui.GetLastFrameRect("InspectorPanel")) {
            vpX1 = right->x;
        }
        auto& rc = engine.GetRenderContext();
        rc.SetViewport(
            ZHLN::RenderContext::ViewportRect {
                .x      = static_cast<uint32_t>(std::clamp(vpX0, 0.0f, static_cast<float>(winSize.width))),
                .y      = 0,
                .width  = static_cast<uint32_t>(std::max(0.0f, vpX1 - vpX0)),
                .height = winSize.height,
            }
        );
        const auto sceneViewport = rc.GetViewport();

        // TEMP-DIAG (camera jump investigation): log every viewport change so
        // a rectangle jump can be correlated with the camera log.
        {
            static ZHLN::RenderContext::ViewportRect lastVp {};
            static bool                              hadVp = false;
            if (!hadVp || lastVp.x != sceneViewport.x || lastVp.y != sceneViewport.y || lastVp.width != sceneViewport.width ||
                lastVp.height != sceneViewport.height) {
                ZHLN::Log(
                    "[DIAG-vp] win={}x{} panelEdges l={} r={} -> vp=({},{},{}x{})", winSize.width, winSize.height, vpX0, vpX1, sceneViewport.x, sceneViewport.y,
                    sceneViewport.width, sceneViewport.height
                );
                lastVp = sceneViewport;
                hadVp  = true;
            }
        }

        // Viewport bounds: center area between the left hierarchy and right inspector
        const bool pointerInViewport = state && state->mouseX >= vpX0 && state->mouseX <= vpX1;

        const bool uiCapturesMouse = state && (!pointerInViewport || state->wantCaptureMouse);
        // A focused text field owns the keyboard without setting a capture flag
        // of its own, so it has to be asked directly -- otherwise typing "wasd"
        // into a name box flies the editor camera. This reads last frame's
        // focus, which is the right question to ask before BeginFrame has run.
        const bool uiCapturesKeyboard = (state && state->wantCaptureKeyboard) || gui.IsTextInputFocused();

        // Blender-style modal transform runs before Escape, picking and the
        // fly camera. Sample the mode BEFORE the update: UpdateTransformMode
        // consumes the Esc press edge to cancel, so afterwards the mode reads
        // as None and the close check below would quit on the very keypress
        // the user meant as "abort the manipulation".
        const bool transformActive = s_NativeEditorState.transformMode != ZHLN::Editor::EditorState::TransformMode::None;
        ZHLN::Editor::UpdateTransformMode(
            reg, s_NativeEditorState, cam, ZHLN::Editor::SceneViewport {sceneViewport.x, sceneViewport.y, sceneViewport.width, sceneViewport.height},
            uiCapturesKeyboard
        );

        // Escape never quits the session -- quitting belongs to the window's
        // close button / the OS quit path. Blender-style, the press walks a
        // ladder instead: a live transform modal consumes it above to cancel;
        // a focused text field owns it (GUI unfocus); otherwise it clears the
        // current selection. Edge-detected so a held key clears once.
        const bool escDown = state && state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Escape));
        if (escDown && !escWasDown && !uiCapturesKeyboard && !transformActive) {
            s_NativeEditorState.selectedEntity = ZHLN::Entity::Null();
        }
        escWasDown = escDown;

        // Ctrl+S saves the world as a scene document. Edge-detected by hand:
        // InputStateComponent carries key *levels*, not presses, so a held
        // chord would write the file once per frame. Gated on the same keyboard
        // capture as Escape and the camera, or Ctrl+S typed into a text field
        // would save too.
        const bool controlDown =
            state && (state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LControl)) || state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RControl)));
        const bool saveChordDown = controlDown && state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::S));
        if (saveChordDown && !saveChordWasDown && !uiCapturesKeyboard) {
            SaveScene(engine);
        }
        saveChordWasDown = saveChordDown;

        if (state && pointerInViewport && !transformActive && !state->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton)) && !uiCapturesMouse) {
            // Window maps GLFW mouse buttons onto the same key stream (see
            // Window.cpp's mouse-button callback), so the raw level needs no
            // platform polling here -- the composition root stays GLFW-free.
            static bool wasMouseDown = false;
            const bool  isMouseDown  = state->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton));

            if (isMouseDown && !wasMouseDown) {
                const auto hit = CastPickingRay(engine, cam, sceneViewport);
                // Physics returns a body handle, not an ECS entity. Resolve it
                // through the components only when the editor needs a selection;
                // unowned bodies and removed components are not selectable.
                s_NativeEditorState.selectedEntity = ZHLN::Entity::Null();
                if (hit.hasHit) {
                    for (ZHLN::Entity entity: reg.GetEntitiesWith<ZHLN::Components::PhysicsComponent>()) {
                        const auto physics = reg.Get<ZHLN::Components::PhysicsComponent>(entity);
                        if (physics && physics->physicsHandle == hit.handle) {
                            s_NativeEditorState.selectedEntity = entity;
                            break;
                        }
                    }
                }
                // TEMP-DIAG (camera jump investigation): what the click saw.
                ZHLN::Log(
                    "[DIAG-pick] mouse=({},{}) vp=({},{},{}x{}) hit={} selNull={}", state->mouseX, state->mouseY, sceneViewport.x, sceneViewport.y,
                    sceneViewport.width, sceneViewport.height, hit.hasHit, s_NativeEditorState.selectedEntity == ZHLN::Entity::Null()
                );
            }
            wasMouseDown = isMouseDown;
        }

        // Native self-hosted editor frame using Clay
        RunNativeEditorFrame(gui, engine, frameTime);

        // The hierarchy's Add Shape dropdown only records a request; spawning
        // needs the Engine (GPU mesh + material), which lives here.
        if (s_NativeEditorState.requestedSpawn >= 0) {
            const int kind                     = s_NativeEditorState.requestedSpawn;
            s_NativeEditorState.requestedSpawn = -1;

            const float     yawRad   = JPH::DegreesToRadians(cam.yaw);
            const float     pitchRad = JPH::DegreesToRadians(cam.pitch);
            const JPH::Vec3 forward  = JPH::Vec3(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad)).Normalized();

            ZHLN::PrefabFactory::SpawnParams sp;
            sp.position = JPH::RVec3(cam.position + forward * 8.0f);

            ZHLN::Entity spawned = ZHLN::Entity::Null();
            switch (kind) {
                case 0:
                    spawned = ZHLN::PrefabFactory::CreateBox(engine, JPH::Vec3::sReplicate(0.5f), sp);
                    break;
                case 1:
                    spawned = ZHLN::PrefabFactory::CreatePlane(engine, 2.0f, JPH::Vec4(0.6f, 0.6f, 0.6f, 1.0f), sp);
                    break;
                case 2:
                    spawned = ZHLN::PrefabFactory::CreateSphere(engine, 0.5f, sp);
                    break;
                case 3:
                    spawned = ZHLN::PrefabFactory::CreateCylinder(engine, 0.5f, 1.0f, sp);
                    break;
                case 4:
                    spawned = ZHLN::PrefabFactory::CreateCone(engine, 0.5f, 1.0f, sp);
                    break;
                default:
                    break;
            }
            if (spawned != ZHLN::Entity::Null()) {
                s_NativeEditorState.selectedEntity = spawned;
            }
        }

        if (state && state->needsResize) {
            engine.GetRenderContext().SetResolution(state->newSize);
            state->needsResize = false;
            continue;
        }

        #if defined(ZHLN_HAS_FREECAM)
        {
            const ZHLN::Entity camEnt = engine.GetRegistry().SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
            if (camEnt != ZHLN::Entity::Null()) {
                engine.GetRegistry().Patch<ZHLN::FreeCam::FreeCamComponent>(camEnt, [&](auto& fc) -> auto { fc.enabled = !transformActive; });
            }
        }
#endif
        if (s_EditorState.simulationRunning) {
            ZHLN::GameplayStatus status = engine.Tick(frameTime, options.driver);
            if (status == ZHLN::GameplayStatus::RequestQuit) {
                engine.GetPlatformHost().Close();
                break;
            }
        } else {
#if defined(ZHLN_HAS_FREECAM)
            if (!transformActive) {
                ZHLN::FreeCam::Apply(engine, frameTime);
            }
#endif
            ZHLN::GameplayStatus status = engine.Tick(0.0f, options.driver);
            if (status == ZHLN::GameplayStatus::RequestQuit) {
                engine.GetPlatformHost().Close();
                break;
            }
        }

        if (options.fpsLimit > 0) {
            auto   frameEnd = std::chrono::high_resolution_clock::now();
            double elapsed  = std::chrono::duration<double>(frameEnd - frameStart).count();
            if (elapsed < targetFrameTime) {
                double sleepTime = targetFrameTime - elapsed;
                if (sleepTime > 0.002) {
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
                }
                while (std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - frameStart).count() < targetFrameTime) {
                    ZHLN::CPURelax();
                }
            }
        }
        frameStart = std::chrono::high_resolution_clock::now();
    }

    ZHLN::Log("[WorldEditor] Editor session closed.");
    return EXIT_SUCCESS;
}

#endif // ZHLN_HAS_EDITOR

} // namespace

auto main(int argc, char* argv[]) -> int {
    // Crash diagnostics keep their state in a caller-owned struct; static
    // storage duration is required because its address is copied into the
    // signal handler slots. See <Signal/CrashHandler.hpp>.
    static ZHLN::CrashState crashState;
    s_HostLaunchConfig = {};

    const std::array appHandlers = {
        ZHLN::CommandHandler {
            .key         = "--scene",
            .placeholder = "<file.toml|file.lua>",
            .description = "Instantiate a TOML or Lua scene recipe on boot",
            .action =
                [](ZHLN::CommandLineOptions&, std::string_view v) -> std::expected<void, ZHLN::ErrorCode> {
                    if (v.empty()) {
                        return std::unexpected(ZHLN::CommandLineError::MissingValue);
                    }
                    s_HostLaunchConfig.scenePath = std::filesystem::path(v);
                    return {};
                },
        },
        ZHLN::CommandHandler {
            .key         = "--graphics-config",
            .placeholder = "<file.lua>",
            .description = "Path to declarative Lua graphics profile (default: config/graphics.lua)",
            .action =
                [](ZHLN::CommandLineOptions&, std::string_view v) -> std::expected<void, ZHLN::ErrorCode> {
                    if (v.empty()) {
                        return std::unexpected(ZHLN::CommandLineError::MissingValue);
                    }
                    s_HostLaunchConfig.graphicsConfigPath = std::filesystem::path(v);
                    return {};
                },
        },
    };

    return ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)), appHandlers)
        .and_then([](const ZHLN::CommandLineOptions& options) -> std::expected<void, ZHLN::ErrorCode> {
            // Early exits (e.g. --help, --version, --print-graph) are successful runs
            if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
                return {};
            }

            ZHLN::SetLogLevel(options.logLevel);

            // A run mode, not a modifier: --benchmark takes the process, times
            // the engine and exits, so it is answered before anything that
            // would open a window or a scene.
            if (options.benchmark) {
#if defined(ZHLN_HAS_BENCHMARK)
                return RunLogicBenchmark() ? std::expected<void, ZHLN::ErrorCode> {}
                                           : std::unexpected(ZHLN::CommandLineError::InvalidValue);
#else
                ZHLN::Log("[Bench] --benchmark requested, but this build has no zahlen_benchmark extension (ZHLN_BUILD_EXTRAS=OFF).");
                return std::unexpected(ZHLN::CommandLineError::InvalidValue);
#endif
            }

            if (options.launchEditor) {
#if !defined(ZHLN_HAS_EDITOR)
                ZHLN::Log("[WorldEditor] --editor requested, but this build has no native editor extra.");
                return std::unexpected(ZHLN::CommandLineError::InvalidValue);
#else
            ZHLN::Platform::Init();
            ZHLN::SetupSignalHandler(crashState);
            ZHLN::TaskSystem::Scope taskScope;
            uint32_t                w = options.fullscreen ? 0 : 1280;
            uint32_t                h = options.fullscreen ? 0 : 720;

            ZHLN::EngineConfig config {
                .physics = {.maxBodies = 5000, .maxBodyPairs = 10000, .maxContactConstraints = 10000, .tempAllocatorSize = 64 * 1024 * 1024},
                .render =
                    {
                        .appName        = options.launchEditor ? "Zahlen World Editor" : "Zahlen Engine",
                        .width          = w,
                        .height         = h,
                        .vsync          = options.vsync,
                        .fullscreen     = options.fullscreen,
                        .validationMode = options.validationMode,
                        .headless       = options.headless,
                    },
                .enableFallbackScene = false,
            };

            auto engine_res = ZHLN::Engine::Create(config);
            if (!engine_res) {
                return std::unexpected(engine_res.error());
            }

            auto engine = std::move(engine_res.value());

            InstallHostWithState(*engine);

            engine->GetPlatformHost().Focus();
            engine->InitializeDefaultScene();

            RunWorldEditor(*engine, options, crashState);
            return {};
#endif
            }

            ZHLN::SetupSignalHandler(crashState);
            const auto pollCrashes = [](ZHLN::Engine& eng) { ZHLN::CheckForCrashes(crashState, &eng); };
            const ZHLN::Engine::RunConfig runConfig {
                .appName        = "Zahlen Engine",
                .vsync          = options.vsync,
                .fullscreen     = options.fullscreen,
                .validationMode = options.validationMode,
                .headless       = options.headless,
                .fpsLimit       = options.fpsLimit,
                .driver         = options.driver,
                .onFrameEvents  = pollCrashes,
            };

            // Runs the engine game loop and propagates any initialization/runtime Error.
            // The UI callback is the benchmark panel: F3 toggles it, and an app
            // that never presses F3 draws exactly what it drew before (the
            // callback returns without touching the UI frame while hidden).
#if defined(ZHLN_HAS_BENCHMARK)
            return ZHLN::Engine::Run(runConfig, MakeBenchmarkOverlay(), &InstallHostWithState);
#else
            return ZHLN::Engine::Run(runConfig, nullptr, &InstallHostWithState);
#endif
        })
        .transform([]() -> int {
            // Success path: mapped to EXIT_SUCCESS (0)
            return EXIT_SUCCESS;
        })
        .or_else([](const ZHLN::Error& err) -> std::expected<int, ZHLN::ErrorCode> {
            // Failure path: logs the rich error and maps to EXIT_FAILURE (1)
            ZHLN::Log("Fatal Engine Error: {}", err.Message());
            return EXIT_FAILURE;
        })
        .value();
}
