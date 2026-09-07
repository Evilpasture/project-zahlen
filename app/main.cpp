// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: app/main.cpp
//
// The composition root. It lives outside src/ because wiring an engine together
// means naming the optional layers it runs with, and the core library may not
// know extras exist. See include/ARCHITECTURE.md 1.2.
#include "engine/Platform.hpp"
#if defined(ZHLN_HAS_SCRIPTING)
// Core has no scripting of its own; the composition root is what names the
// optional layers the engine runs with.
#include <Scripting/Lua/LuaScriptRuntime.hpp>
#endif
#include "engine/system/GraphicsSettingsSync.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include "physics/PhysicsWorld.hpp"
#include <Jolt/Physics/Collision/CastResult.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Console.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/GUIEditor.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <Zahlen/physics/Physics.hpp>
#if defined(ZHLN_HAS_SCENE_TOML)
// The document layer is an optional extra, and the composition root is the one
// place allowed to name it: core may not reach into extras
// (tools/check_core_extras_boundary.py). SceneTOML.hpp is what turns a core
// ZHLN::Scene::Scene into a document, via its Jolt vector bindings.
#include <toml/SceneTOML.hpp>
#include <toml/TOML.hpp>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

namespace {

// ============================================================================
// WORLD EDITOR
// ============================================================================

struct EditorState {
    bool         simulationRunning = false;
    ZHLN::Entity selectedEntity    = ZHLN::Entity::Null();
    bool         freeCamActive     = true;
    float        freeCamSpeed      = 25.0f;
};

EditorState s_EditorState;

// --- Native (self-hosted) editor state --------------------------------------
ZHLN::Editor::EditorState s_NativeEditorState;

/// Where Ctrl+S writes. The editor has no notion of "the current scene" yet --
/// nothing opens a file, so nothing knows its name -- and one predictable path
/// next to the working directory beats inventing a session concept here.
constexpr std::string_view kSceneSavePath = "scene.toml";

/// Extracts the world and writes it back out as a scene document.
///
/// This is the round trip closing: Scene::Instantiate built the world from a
/// description, Scene::Extract reads a description back out of the world, and
/// the reflection-driven TOML layer turns that into text. Nothing here lists
/// fields -- the description struct is the schema in both directions.
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

    ZHLN::Log(
        "[WorldEditor] Ctrl+S: '{}' written ({} entities, {} lights)", kSceneSavePath, scene.entities.size(), scene.lights.size()
    );
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

void UpdateEditorCamera(ZHLN::Camera& cam, const ZHLN::Components::InputStateComponent& state, float dt, bool transformActive) {
    // A modal transform owns the pointer and the axis keys; the fly camera
    // would otherwise fight the manipulation for the same input. The flag is
    // the pre-update sample: on the frame an LMB/Esc ends the mode the camera
    // must not also act on whatever movement keys happen to be down.
    if (transformActive) {
        return;
    }
    const float sensitivity = 0.15f;

    const bool uiCapturesMouse    = state.wantCaptureMouse;
    const bool uiCapturesKeyboard = state.wantCaptureKeyboard;

    if (state.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton)) && !uiCapturesMouse) {
        cam.yaw += state.mouseDeltaX * sensitivity;
        cam.pitch = std::clamp(cam.pitch - (state.mouseDeltaY * sensitivity), -89.0f, 89.0f);
    }

    if (uiCapturesKeyboard) {
        return;
    }

    float yawRad   = JPH::DegreesToRadians(cam.yaw);
    float pitchRad = JPH::DegreesToRadians(cam.pitch);

    JPH::Vec3 forward(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
    forward         = forward.Normalized();
    JPH::Vec3 right = forward.Cross(JPH::Vec3::sAxisY()).Normalized();

    float moveSpeed = state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LShift)) ? (s_EditorState.freeCamSpeed * 2.5f) : s_EditorState.freeCamSpeed;

    JPH::Vec3 moveDirection = JPH::Vec3::sZero();
    if (state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::W))) {
        moveDirection += forward;
    }
    if (state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::S))) {
        moveDirection -= forward;
    }
    if (state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::A))) {
        moveDirection -= right;
    }
    if (state.IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::D))) {
        moveDirection += right;
    }

    if (moveDirection.LengthSq() > 0.0f) {
        cam.position += moveDirection.Normalized() * moveSpeed * dt;
    }
}

ZHLN::Physics::RaycastResult CastPickingRay(ZHLN::Engine& engine, const ZHLN::Camera& cam, const ZHLN::RenderContext::ViewportRect& vp) {
    auto& reg    = engine.GetRegistry();
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    if (auto* st = reg.GetSingleton<ZHLN::Components::InputStateComponent>()) {
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
    gui.BeginFrame(dt);

    gui.Box(
        "EditorWorkspace",
        ZHLN::GUI::BoxConfig {
            .width     = {.grow = 1.0f},
            .height    = {.grow = 1.0f},
            .color     = {0.0f, 0.0f, 0.0f, 0.0f}, // Transparent workspace over 3D scene
            .direction = ZHLN::GUI::Direction::Row
        },
        [&]() -> void {
            // Left: Hierarchy panel
            gui.Box(
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
                [&]() -> void { ZHLN::Editor::DrawHierarchyPanel(gui, reg, s_NativeEditorState, "Hierarchy"); }
            );

            // Center: Viewport overlay toolbar
            gui.Box(
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
                    gui.Box(
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
                        [&]() -> void { gui.Checkbox("Simulate", s_EditorState.simulationRunning); }
                    );
                }
            );

            // Right: Inspector panel
            gui.Box(
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
                [&]() -> void { ZHLN::Editor::DrawInspectorPanel(gui, reg, s_NativeEditorState, "Inspector"); }
            );
        }
    );

    gui.EndFrameAndRender(engine.GetRenderContext());
}

int RunWorldEditor(ZHLN::Engine& engine, const ZHLN::CommandLineOptions& options) {
    ZHLN::Clock clock;
    auto&       cam = engine.GetCamera();

    cam.position = {0.0f, 20.0f, 40.0f};
    cam.yaw      = -90.0f;
    cam.pitch    = -20.0f;

    const double targetFrameTime = options.fpsLimit > 0 ? 1.0 / static_cast<double>(options.fpsLimit) : 0.0;
    auto         frameStart      = std::chrono::high_resolution_clock::now();

    ZHLN::Log("[WorldEditor] Editor session launched.");

    bool saveChordWasDown = false;
    bool escWasDown       = false;

    while (engine.IsRunning()) {
        float frameTime = clock.GetDeltaTime();
        engine.ProcessEvents();

        auto& reg   = engine.GetRegistry();
        auto* state = reg.GetSingleton<ZHLN::Components::InputStateComponent>();

        auto winSize = engine.GetWindow().GetSize();

        // A cheap handle over the Impl the registry owns (GUIStateComponent),
        // so building it here costs a pointer and lets the viewport bounds and
        // the gating below ask about last frame's layout and text focus. The
        // same handle is handed to the frame builder.
        ZHLN::GUI::Context gui(engine);

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
        rc.SetViewport(ZHLN::RenderContext::ViewportRect {
            .x      = static_cast<uint32_t>(std::clamp(vpX0, 0.0f, static_cast<float>(winSize.width))),
            .y      = 0,
            .width  = static_cast<uint32_t>(std::max(0.0f, vpX1 - vpX0)),
            .height = winSize.height,
        });
        const auto sceneViewport = rc.GetViewport();

        // Ctrl+C/X/V in a focused field go to the OS clipboard through the
        // window. Re-set every frame because the handle is rebuilt; the sink is
        // stateless, so this is two stores.
        gui.SetClipboard(ZHLN::GUI::TextEdit::ClipboardSink {
            .userdata = &engine,
            .set      = [](void* ud, std::string_view text) -> void { static_cast<ZHLN::Engine*>(ud)->GetWindow().SetClipboardText(text); },
            .get      = [](void* ud) -> std::string { return static_cast<ZHLN::Engine*>(ud)->GetWindow().GetClipboardText(); },
        });

        // Viewport bounds: center area between the left hierarchy and right inspector
        const bool pointerInViewport = state != nullptr && state->mouseX >= vpX0 && state->mouseX <= vpX1;

        const bool uiCapturesMouse = state != nullptr && (!pointerInViewport || state->wantCaptureMouse);
        // A focused text field owns the keyboard without setting a capture flag
        // of its own, so it has to be asked directly -- otherwise typing "wasd"
        // into a name box flies the editor camera. This reads last frame's
        // focus, which is the right question to ask before BeginFrame has run.
        const bool uiCapturesKeyboard = (state != nullptr && state->wantCaptureKeyboard) || gui.IsTextInputFocused();

        // Blender-style modal transform runs before Escape, picking and the
        // fly camera. Sample the mode BEFORE the update: UpdateTransformMode
        // consumes the Esc press edge to cancel, so afterwards the mode reads
        // as None and the close check below would quit on the very keypress
        // the user meant as "abort the manipulation".
        const bool transformActive = s_NativeEditorState.transformMode != ZHLN::Editor::EditorState::TransformMode::None;
        ZHLN::Editor::UpdateTransformMode(
            reg, s_NativeEditorState, cam,
            ZHLN::Editor::SceneViewport {sceneViewport.x, sceneViewport.y, sceneViewport.width, sceneViewport.height}, uiCapturesKeyboard
        );

        // Escape never quits the session -- quitting belongs to the window's
        // close button / the OS quit path. Blender-style, the press walks a
        // ladder instead: a live transform modal consumes it above to cancel;
        // a focused text field owns it (GUI unfocus); otherwise it clears the
        // current selection. Edge-detected so a held key clears once.
        const bool escDown = state != nullptr && state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Escape));
        if (escDown && !escWasDown && !uiCapturesKeyboard && !transformActive) {
            s_NativeEditorState.selectedEntity = ZHLN::Entity::Null();
        }
        escWasDown = escDown;

        // Ctrl+S saves the world as a scene document. Edge-detected by hand:
        // InputStateComponent carries key *levels*, not presses, so a held
        // chord would write the file once per frame. Gated on the same keyboard
        // capture as Escape and the camera, or Ctrl+S typed into a text field
        // would save too.
        const bool controlDown   = state != nullptr && (state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LControl)) ||
                                                      state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RControl)));
        const bool saveChordDown = controlDown && state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::S));
        if (saveChordDown && !saveChordWasDown && !uiCapturesKeyboard) {
            SaveScene(engine);
        }
        saveChordWasDown = saveChordDown;

        if (state != nullptr && pointerInViewport && !transformActive &&
            !state->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton)) && !uiCapturesMouse) {
            // Window maps GLFW mouse buttons onto the same key stream (see
            // Window.cpp's mouse-button callback), so the raw level needs no
            // platform polling here -- the composition root stays GLFW-free.
            static bool wasMouseDown = false;
            const bool  isMouseDown  = state->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton));

            if (isMouseDown && !wasMouseDown) {
                auto hit                           = CastPickingRay(engine, cam, sceneViewport);
                s_NativeEditorState.selectedEntity = hit.hasHit ? hit.handle : ZHLN::Entity::Null();
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

            const float   yawRad   = JPH::DegreesToRadians(cam.yaw);
            const float   pitchRad = JPH::DegreesToRadians(cam.pitch);
            const JPH::Vec3 forward =
                JPH::Vec3(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad)).Normalized();

            ZHLN::CreativeWorksFactory::SpawnParams sp;
            sp.position = JPH::RVec3(cam.position + forward * 8.0f);

            ZHLN::Entity spawned = ZHLN::Entity::Null();
            switch (kind) {
                case 0: spawned = ZHLN::CreativeWorksFactory::CreateBox(engine, JPH::Vec3::sReplicate(0.5f), sp); break;
                case 1: spawned = ZHLN::CreativeWorksFactory::CreatePlane(engine, 2.0f, JPH::Vec4(0.6f, 0.6f, 0.6f, 1.0f), sp); break;
                case 2: spawned = ZHLN::CreativeWorksFactory::CreateSphere(engine, 0.5f, sp); break;
                case 3: spawned = ZHLN::CreativeWorksFactory::CreateCylinder(engine, 0.5f, 1.0f, sp); break;
                case 4: spawned = ZHLN::CreativeWorksFactory::CreateCone(engine, 0.5f, 1.0f, sp); break;
                default: break;
            }
            if (spawned != ZHLN::Entity::Null()) {
                s_NativeEditorState.selectedEntity = spawned;
            }
        }

        if (state != nullptr && state->needsResize) {
            engine.GetRenderContext().SetResolution(state->newSize);
            state->needsResize = false;
            continue;
        }

        if (s_EditorState.simulationRunning) {
            ZHLN::GameplayStatus status = engine.Tick(frameTime, options.driver);
            if (status == ZHLN::GameplayStatus::RequestQuit) {
                engine.GetWindow().Close();
                break;
            }
        } else {
            if (state != nullptr) {
                UpdateEditorCamera(cam, *state, frameTime, transformActive);
            }

            ZHLN::GameplayStatus status = engine.Tick(0.0f, options.driver);
            if (status == ZHLN::GameplayStatus::RequestQuit) {
                engine.GetWindow().Close();
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

} // namespace

auto main(int argc, char* argv[]) -> int {
    return ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)))
        .and_then([](const ZHLN::CommandLineOptions& options) -> std::expected<void, ZHLN::Error> {
            // Early exits (e.g. --help, --version, --print-graph) are successful runs
            if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
                return {};
            }

            ZHLN::SetLogLevel(options.logLevel);

            if (options.launchEditor) {
                ZHLN::Platform::Init();
                ZHLN::SetupSignalHandler();
                ZHLN::TaskSystem::Init();
                ZHLN::DefaultPreset::SetDisabled(true);

                uint32_t w = options.fullscreen ? 0 : 1280;
                uint32_t h = options.fullscreen ? 0 : 720;

                ZHLN::EngineConfig config {
                    .physics = {.maxBodies = 5000, .maxBodyPairs = 10000, .maxContactConstraints = 10000, .tempAllocatorSize = 64 * 1024 * 1024},
                    .render  = {
                        .appName        = options.launchEditor ? "Zahlen World Editor" : "Zahlen Engine",
                        .width          = w,
                        .height         = h,
                        .vsync          = options.vsync,
                        .fullscreen     = options.fullscreen,
                        .validationMode = options.validationMode,
                        .headless       = options.headless,
                    },
                };

                auto engine_res = ZHLN::Engine::Create(config);
                if (!engine_res) {
                    ZHLN::TaskSystem::Shutdown();
                    return std::unexpected(engine_res.error());
                }

                auto engine = std::move(engine_res.value());

#if defined(ZHLN_HAS_SCRIPTING)
                // Nothing in core installs a runtime, so a build without the
                // scripting extra simply has none: ScriptRunner forwards to
                // nothing and the engine runs C++-only.
                engine->GetScriptRunner().SetRuntime(std::make_unique<ZHLN::LuaScriptRuntime>());
#endif

                engine->GetWindow().Focus();
                engine->InitializeDefaultScene();

                RunWorldEditor(*engine, options);

                ZHLN::TaskSystem::Shutdown();
                return {};
            }

            // Runs the engine game loop and propagates any initialization/runtime Error
            return ZHLN::Engine::Run(options, nullptr);
        })
        .transform([]() -> int {
            // Success path: mapped to EXIT_SUCCESS (0)
            return EXIT_SUCCESS;
        })
        .or_else([](const ZHLN::Error& err) -> std::expected<int, ZHLN::Error> {
            // Failure path: logs the rich error and maps to EXIT_FAILURE (1)
            ZHLN::Log("Fatal Engine Error: {}", err.Message());
            return EXIT_FAILURE;
        })
        .value();
}
