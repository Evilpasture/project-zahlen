// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/GUIToolingPrimitivesSample.cpp
//
// Demonstrates the six native-ECS GUI tooling primitives that ship with
// <Zahlen/GUI.hpp>:
//
//     ui.Checkbox(label, bool& value)
//     ui.DragFloat / Slider(label, float& value, min, max, step)
//     ui.TextInput(label, FixedString/string& value)
//     ui.Dropdown(label, int& selectedIdx, span<string_view> options)
//     ui.CollapsingHeader(label, defaultOpen, fn)
//     ui.Columns / Splitter(direction, ratio, leftFn, rightFn)
//
// With these primitives any tool/inspector window can be expressed in
// 20-30 lines of C++: this sample is the living proof.
//
// Controls:
//   * Toggle every bool/drag/dropdown/text/splitter on the panel.
//   * Press Escape to close the sample.

#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>

#include <array>
#include <cstdlib>
#include <string_view>

namespace {

// Tool-window state. The widget primitives read/write these fields directly
// through their bool& / float& / int& / StringT& references, so the struct is
// your single source of truth — no widget IDs, no getter/setter plumbing.
struct RenderSettings {
    bool              enableVsync       = true;
    bool              showWireframe     = false;
    bool              showGrid          = true;
    float             fov               = 75.0f;
    float             exposure          = 1.0f;
    float             bloomIntensity    = 0.15f;
    ZHLN::String256   profileName       = ZHLN::String256("Default");
    int               qualityPreset     = 1; // 0=Low 1=Medium 2=High 3=Ultra
};

constexpr std::array<std::string_view, 4> kQualityPresets = {"Low", "Medium", "High", "Ultra"};

// Draws the "Render Settings" inspector — this is the ~30-line tool window.
void DrawRenderSettingsWindow(ZHLN::GUI::Context& ui, RenderSettings& s) {
    ui.Panel("RenderSettings", ZHLN::GUI::PanelConfig {
        .width    = 400.0f,
        .height   = 0.0f,
        .x        = -200.0f,   // anchored center, offset left half-width
        .y        = -240.0f,
        .gap      = 6.0f,
        .padding  = 14.0f,
    }, [&]() -> void {
        ui.Label("Render Settings", ZHLN::GUI::LabelConfig {
            .scale         = 1.05f,
            .color         = {0.40f, 0.72f, 1.00f, 1.0f},
            .align         = ZHLN::TextAlignment::Center,
            .verticalAlign = ZHLN::TextVerticalAlignment::Center,
            .height        = 28.0f,
        });

        auto scopeDisplay = ui.CollapsingHeader("Display", true, [&]() -> void {
            ui.Checkbox("VSync",    "Enable VSync",    s.enableVsync);
            ui.Checkbox("Wireframe","Wireframe Overlay",s.showWireframe);
            ui.Checkbox("Grid",     "Show Grid",       s.showGrid);
            ui.DragFloat("FOV",      s.fov,            30.0f, 120.0f, 0.5f);
            ui.Slider("Exposure",    s.exposure,        0.1f,   5.0f, 0.01f);
            ui.Slider("Bloom",       s.bloomIntensity,  0.0f,   1.0f, 0.01f);
        });

        auto scopeQuality = ui.CollapsingHeader("Quality", false, [&]() -> void {
            int idx = s.qualityPreset;
            ui.Dropdown("Preset", "Quality Preset", idx,
                        std::span<const std::string_view>(kQualityPresets));
            s.qualityPreset = idx;
        });

        auto scopeProfile = ui.CollapsingHeader("Profile", true, [&]() -> void {
            ui.TextInput("ProfileName", "Profile Name", s.profileName);
        });

        // Splitter/Columns demo: left half "Preview" placeholder, right half "Stats".
        float split = 0.55f;
        auto scopeSplit = ui.Columns("PreviewSplit", ZHLN::GUI::SplitDirection::Horizontal, split,
            [&]() -> void {
                ui.Box(ZHLN::GUI::BoxConfig {
                    .height    = 100.0f,
                    .color     = {0.06f, 0.09f, 0.14f, 0.85f},
                    .direction = ZHLN::FlexDirection::Column,
                    .justify   = ZHLN::FlexJustify::Center,
                    .alignItems= ZHLN::FlexAlign::Center,
                    .padding   = 8.0f,
                }, [&]() -> void {
                    ui.Label("Preview", ZHLN::GUI::LabelConfig {
                        .scale         = 0.90f,
                        .color         = {0.55f, 0.70f, 0.90f, 0.60f},
                        .align         = ZHLN::TextAlignment::Center,
                        .verticalAlign = ZHLN::TextVerticalAlignment::Center,
                    });
                });
            },
            [&]() -> void {
                ui.Label("Stats", ZHLN::GUI::LabelConfig {
                    .scale         = 0.80f,
                    .color         = {0.70f, 0.80f, 0.95f, 1.0f},
                    .height        = 20.0f,
                });
                ui.Label("FPS: 142",  ZHLN::GUI::LabelConfig {.scale = 0.75f, .height = 18.0f});
                ui.Label("Draws: 312",ZHLN::GUI::LabelConfig {.scale = 0.75f, .height = 18.0f});
                ui.Label("VSync: on", ZHLN::GUI::LabelConfig {.scale = 0.75f, .height = 18.0f});
            }
        );
        (void)scopeDisplay; (void)scopeQuality; (void)scopeProfile; (void)scopeSplit;
    });
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    auto optionsRes = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& options = optionsRes.value();
    if (options.helpRequested || options.versionRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(options.logLevel);
    ZHLN::SetupSignalHandler();
    ZHLN::TaskSystem::Init();

    auto engineRes = ZHLN::Engine::Create(ZHLN::EngineConfig {
        .physics = {.maxBodies = 2048, .maxBodyPairs = 4096, .maxContactConstraints = 4096},
        .render  = {
            .appName    = ZHLN::String64("Zahlen :: GUI Tooling Primitives"),
            .width      = 1280,
            .height     = 720,
            .vsync      = options.vsync,
            .fullscreen = options.fullscreen,
        },
    });
    if (!engineRes) {
        ZHLN::Log("FATAL: Failed to initialize Engine: {}", engineRes.error().Message());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    engine->GetWindow().Focus();
    engine->InitializeDefaultScene();

    RenderSettings settings;

    ZHLN::Clock clock;
    while (engine->IsRunning()) {
        const float dt = std::min(clock.GetDeltaTime(), 0.05f);
        engine->ProcessEvents();

        // Allow Escape to quit.
        auto& reg = engine->GetRegistry();
        if (auto* input = reg.GetSingleton<ZHLN::Components::InputStateComponent>()) {
            if (input->IsKeyDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::Escape))) {
                engine->GetWindow().Close();
            }
        }

        const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetWindow().Close();
            break;
        }

        // Evaluate the immediate-mode UI against the registry.
        ZHLN::GUI::Context ui(reg, engine->GetCurrentFrame());
        DrawRenderSettingsWindow(ui, settings);
    }

    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}
