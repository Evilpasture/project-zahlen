// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Dedicated world editor. Its paused/simulating loop is intentionally custom;
// an ordinary game does not link this UI, document layer, or editor logic.
#include <SceneDocument.hpp>
#include "engine/Platform.hpp"
// clang-format off
#include <Zahlen/Physics/Jolt.hpp>
// clang-format on
#include <Zahlen/ConfigCli.hpp>
#include <FreeCam/FreeCam.hpp>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Signal/CrashHandler.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Engine/Scene.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window/Window.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/GUI/GUI.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <editor/GUIEditor.hpp>
#include <toml/SceneTOML.hpp>
#include <toml/TOML.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

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

// Where Ctrl+S writes for now. Document import is supported, but the
// editor does not yet track the source name for Save As.
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

auto RunWorldEditor(ZHLN::Engine& engine, const ZHLN::EngineConfig& options, ZHLN::CrashState& crashState)
    -> std::expected<void, ZHLN::ErrorCode> {
    ZHLN::Clock clock;
    // The editor poses the world's camera: a world with no main camera entity has
    // none to pose.
    auto cameraComp = engine.GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
    if (!cameraComp) {
        return std::unexpected(ZHLN::AppError::SceneInitFailed);
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
        if (!engine.IsRunning()) break;

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

        {
            const ZHLN::Entity camEnt = engine.GetRegistry().SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
            if (camEnt != ZHLN::Entity::Null()) {
                engine.GetRegistry().Patch<ZHLN::FreeCam::FreeCamComponent>(camEnt, [&](auto& fc) -> auto { fc.enabled = !transformActive; });
            }
        }
        if (s_EditorState.simulationRunning) {
            if (engine.Tick(frameTime) == ZHLN::FrameStatus::Error) {
                ZHLN::LogError("[WorldEditor] engine frame failed");
                return std::unexpected(ZHLN::AppError::FrameTickFailed);
            }
        } else {
            if (!transformActive) {
                ZHLN::FreeCam::Apply(engine, frameTime);
            }
            if (engine.Tick(0.0f) == ZHLN::FrameStatus::Error) {
                ZHLN::LogError("[WorldEditor] engine frame failed");
                return std::unexpected(ZHLN::AppError::FrameTickFailed);
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
    return {};
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    static ZHLN::CrashState crashState;
    ZHLN::EngineConfig config {.appName = "Zahlen World Editor", .enableFallbackScene = false};
    std::filesystem::path scenePath;
    const std::array appHandlers = {
        ZHLN::CliFlag {
            .key         = "--scene",
            .placeholder = "<file.toml|file.lua>",
            .description = "Open a TOML or Lua scene document in the world editor",
            .action      = [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                if (v.empty()) return std::unexpected(ZHLN::CommandLineError::MissingValue);
                scenePath = std::filesystem::path(v);
                return {};
            },
        },
    };
    const auto parsed = ZHLN::ParseEngineConfig(std::span(argv, static_cast<size_t>(argc)), config, appHandlers);
    if (!parsed) {
        ZHLN::LogError("[WorldEditor] command line: {}", parsed.error());
        return EXIT_FAILURE;
    }
    if (parsed->helpRequested || parsed->versionRequested || parsed->graphRequested) return EXIT_SUCCESS;
    if (config.headless) {
        ZHLN::LogError("[WorldEditor] this editor requires a window");
        return EXIT_FAILURE;
    }
    ZHLN::SetLogLevel(config.logLevel);

    // Unlike App's ordinary-game loop, the editor pauses simulation and still
    // renders/picks/edits. Keep this bespoke loop in its own executable.
    ZHLN::Platform::Init();
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Scope tasks;
    auto created = ZHLN::Engine::Create(config);
    if (!created) {
        ZHLN::LogError("[WorldEditor] engine creation failed: {}", created.error());
        return EXIT_FAILURE;
    }
    auto engine = std::move(*created);
    ZHLN::InstallApplicationDefaults(*engine);
    engine->GetPlatformHost().Focus();
    if (!engine->InitializeDefaultScene()) {
        ZHLN::LogError("[WorldEditor] initial scene failed");
        return EXIT_FAILURE;
    }
    if (!scenePath.empty() && !ZHLN::Tools::LoadSceneDocument(*engine, scenePath)) {
        return EXIT_FAILURE;
    }
    const auto ran = RunWorldEditor(*engine, config, crashState);
    if (!ran) ZHLN::LogError("[WorldEditor] run failed: {}", ran.error());
    return ran ? EXIT_SUCCESS : EXIT_FAILURE;
}
