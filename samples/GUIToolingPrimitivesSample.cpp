// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/CommandLine.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdlib>
#include <string_view>

namespace {

struct RenderSettings {
    bool            enableVsync    = true;
    bool            showWireframe  = false;
    bool            showGrid       = true;
    float           fov            = 75.0f;
    float           exposure       = 1.0f;
    float           bloomIntensity = 0.15f;
    ZHLN::String256 profileName    = ZHLN::String256("Default");
    int             qualityPreset  = 1;
};

constexpr std::array<std::string_view, 4> kQualityPresets = {"Low", "Medium", "High", "Ultra"};

void DrawRenderSettingsWindow(ZHLN::GUI::Context& ui, RenderSettings& s) {
    ui.Panel(
        "RenderSettings",
        ZHLN::GUI::PanelConfig {
            .width   = 400.0f,
            .height  = 0.0f,
            .x       = 0.0f,
            .y       = 0.0f,
            .gap     = 6.0f,
            .padding = 14.0f,
        },
        [&]() -> void {
            ui.Label(
                "Render Settings", ZHLN::GUI::LabelConfig {
                                       .scale         = 1.05f,
                                       .color         = {0.40f, 0.72f, 1.00f, 1.0f},
                                       .align         = ZHLN::TextAlignment::Center,
                                       .verticalAlign = ZHLN::TextVerticalAlignment::Center,
                                       .height        = 28.0f,
                                   }
            );

            ui.CollapsingHeader("Display", true, [&]() -> void {
                ui.Checkbox("VSync", "Enable VSync", s.enableVsync);
                ui.Checkbox("Wireframe", "Wireframe Overlay", s.showWireframe);
                ui.Checkbox("Grid", "Show Grid", s.showGrid);
                ui.DragFloat("FOV", s.fov, 30.0f, 120.0f, 0.5f);
                ui.Slider("Exposure", s.exposure, 0.1f, 5.0f, 0.01f);
                ui.Slider("Bloom", s.bloomIntensity, 0.0f, 1.0f, 0.01f);
            });

            ui.CollapsingHeader("Quality", false, [&]() -> void {
                int idx = s.qualityPreset;
                ui.Dropdown("Preset", "Quality Preset", idx, std::span<const std::string_view>(kQualityPresets));
                s.qualityPreset = idx;
            });

            ui.CollapsingHeader("Profile", true, [&]() -> void { ui.TextInput("ProfileName", "Profile Name", s.profileName); });

            float split = 0.55f;
            ui.Columns(
                "PreviewSplit", ZHLN::GUI::SplitDirection::Horizontal, split,
                [&]() -> void {
                    ui.Box(
                        ZHLN::GUI::BoxConfig {
                            .height     = 100.0f,
                            .color      = {0.06f, 0.09f, 0.14f, 0.85f},
                            .direction  = ZHLN::FlexDirection::Column,
                            .justify    = ZHLN::FlexJustify::Center,
                            .alignItems = ZHLN::FlexAlign::Center,
                            .padding    = 8.0f,
                        },
                        [&]() -> void {
                            ui.Label(
                                "Preview", ZHLN::GUI::LabelConfig {
                                               .scale         = 0.90f,
                                               .color         = {0.55f, 0.70f, 0.90f, 0.60f},
                                               .align         = ZHLN::TextAlignment::Center,
                                               .verticalAlign = ZHLN::TextVerticalAlignment::Center,
                                           }
                            );
                        }
                    );
                },
                [&]() -> void {
                    ui.Label(
                        "Stats", ZHLN::GUI::LabelConfig {
                                     .scale  = 0.80f,
                                     .color  = {0.70f, 0.80f, 0.95f, 1.0f},
                                     .height = 20.0f,
                                 }
                    );
                    ui.Label("FPS: 142", ZHLN::GUI::LabelConfig {.scale = 0.75f, .height = 18.0f});
                    ui.Label("Draws: 312", ZHLN::GUI::LabelConfig {.scale = 0.75f, .height = 18.0f});
                    ui.Label("VSync: on", ZHLN::GUI::LabelConfig {.scale = 0.75f, .height = 18.0f});
                }
            );
        }
    );
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    return ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)))
        .and_then([](const ZHLN::CommandLineOptions& options) -> std::expected<void, ZHLN::Error> {
            if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
                return {};
            }

            ZHLN::SetLogLevel(options.logLevel);
            ZHLN::DefaultPreset::SetDisabled(true); // Suppress missing libgameplay.so fallback

            RenderSettings settings;

            // ZHLN::Engine::Run handles Platform::Init, resize events, frame pacing,
            // and executes your UI callback in Phase::UI before rendering.
            return ZHLN::Engine::Run(options, [&settings](ZHLN::Engine& engine) {
                ZHLN::GUI::Context ui(engine.GetRegistry(), engine.GetCurrentFrame());
                DrawRenderSettingsWindow(ui, settings);
            });
        })
        .transform([]() -> int { return EXIT_SUCCESS; })
        .or_else([](const ZHLN::Error& err) -> std::expected<int, ZHLN::Error> {
            ZHLN::Log("Fatal Error: {}", err.Message());
            return EXIT_FAILURE;
        })
        .value();
}
