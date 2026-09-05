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
#include <GLFW/glfw3.h>
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
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/gui/UIComponents.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <print>
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

constexpr float kLeftPanelWidth  = 260.0f;
constexpr float kRightPanelWidth = 320.0f;

void UpdateEditorCamera(ZHLN::Camera& cam, const ZHLN::Components::InputStateComponent& state, float dt) {
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

ZHLN::Physics::RaycastResult CastPickingRay(ZHLN::Engine& engine, const ZHLN::Camera& cam) {
    auto& reg    = engine.GetRegistry();
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    if (auto* st = reg.GetSingleton<ZHLN::Components::InputStateComponent>()) {
        mouseX = st->mouseX;
        mouseY = st->mouseY;
    }
    auto winSize = engine.GetWindow().GetSize();

    if (winSize.width == 0 || winSize.height == 0) {
        return {};
    }

    float ndcX   = (2.0f * mouseX) / static_cast<float>(winSize.width) - 1.0f;
    float ndcY   = 1.0f - (2.0f * mouseY) / static_cast<float>(winSize.height);
    float aspect = static_cast<float>(winSize.width) / static_cast<float>(winSize.height);

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
void RunNativeEditorFrame(ZHLN::Engine& engine, float dt) {
    auto&              reg = engine.GetRegistry();
    ZHLN::GUI::Context gui(engine);
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

    while (engine.IsRunning()) {
        float frameTime = clock.GetDeltaTime();
        engine.ProcessEvents();

        auto& reg   = engine.GetRegistry();
        auto* state = reg.GetSingleton<ZHLN::Components::InputStateComponent>();

        auto winSize = engine.GetWindow().GetSize();

        // Viewport bounds: center area between the left hierarchy and right inspector
        const bool pointerInViewport = state != nullptr && state->mouseX >= kLeftPanelWidth &&
                                       state->mouseX <= (static_cast<float>(winSize.width) - kRightPanelWidth);

        const bool uiCapturesMouse    = state != nullptr && (!pointerInViewport || state->wantCaptureMouse);
        const bool uiCapturesKeyboard = state != nullptr && state->wantCaptureKeyboard;

        if (state != nullptr && state->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Escape)) && !uiCapturesKeyboard) {
            engine.GetWindow().Close();
            break;
        }

        if (state != nullptr && pointerInViewport && !state->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton)) && !uiCapturesMouse) {
            static bool wasMouseDown = false;
            bool        isMouseDown  = glfwGetMouseButton(static_cast<GLFWwindow*>(engine.GetWindow().GetNativeHandle()), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

            if (isMouseDown && !wasMouseDown) {
                auto hit                           = CastPickingRay(engine, cam);
                s_NativeEditorState.selectedEntity = hit.hasHit ? hit.handle : ZHLN::Entity::Null();
            }
            wasMouseDown = isMouseDown;
        }

        // Native self-hosted editor frame using Clay
        RunNativeEditorFrame(engine, frameTime);

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
                UpdateEditorCamera(cam, *state, frameTime);
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
