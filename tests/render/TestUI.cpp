// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <expected>
#include <fstream>
#include <string>
#include <vector>

enum class UITestError : uint8_t {
    EngineInitFailed             ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize headless Engine context for UI test."> {}) = 1,
    RenderOutputBlank            ZHLN_ANNOTATION(ZHLN::Description<"Rendered frame is blank or failed to capture."> {}),
    UINotRendered                ZHLN_ANNOTATION(ZHLN::Description<"Automated pixel analysis detected zero UI pixels on screen."> {}),
    ButtonClickInteractionFailed ZHLN_ANNOTATION(ZHLN::Description<"Button click interaction, state transitions, or callback dispatch failed."> {}),
};

struct UITestSuite {
    UITestSuite() {
        ZHLN::Test::Headless::BeginSession();
    }

    ~UITestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> ZHLN::Test::Headless::EngineHandle {
        return ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {.appName = "Headless UI Test", .width = width, .height = height});
    }

    struct Tests {
        std::expected<void, ZHLN::Error> immediate_mode_ui_rendering() {
            auto engine = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(UITestError::EngineInitFailed);
            }

            auto& rc = engine->GetRenderContext();

            // Set up a simple UI scene
            engine->SetUICallback([&](ZHLN::Engine& eng) {
                ZHLN::GUI::Context ui(eng);
                ui.BeginFrame(0.016f);

                ui.Box(
                    "TestBox",
                    ZHLN::GUI::BoxConfig {
                        .width   = {.fixed = 200.0f},
                        .height  = {.fixed = 200.0f},
                        .color   = {0.0f, 1.0f, 0.0f, 1.0f}, // Pure Green Box
                        .padding = 20.0f
                    },
                    [&]() {
                        ui.Text("TEST", 20.0f, {0.0f, 0.0f, 1.0f, 1.0f}); // Pure Blue Text
                    }
                );

                ui.EndFrameAndRender(eng.GetRenderContext());
            });

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 5; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            const std::string ppmPath = "headless_ui_output.ppm";
            if (!rc.CaptureScreenshotPPM(ppmPath)) {
                return std::unexpected(UITestError::RenderOutputBlank);
            }

            std::ifstream ppm(ppmPath, std::ios::binary);
            if (!ppm.is_open()) {
                return std::unexpected(UITestError::RenderOutputBlank);
            }

            std::string header;
            int         width = 0, height = 0, maxColor = 0;
            ppm >> header >> width >> height >> maxColor;
            ppm.get();

            std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 3));
            ppm.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

            uint32_t greenBoxPixels = 0;
            uint32_t blueTextPixels = 0;

            for (size_t i = 0; i < pixels.size(); i += 3) {
                const uint8_t r = pixels[i + 0];
                const uint8_t g = pixels[i + 1];
                const uint8_t b = pixels[i + 2];

                if (g > 200 && r < 50 && b < 50) {
                    greenBoxPixels++;
                }
                // Blue text might have anti-aliasing/blending, check dominantly blue
                else if (b > 150 && r < 100 && g < 100) {
                    blueTextPixels++;
                }
            }

            ZHLN::Test::ExpectTrue(greenBoxPixels > 1000u);
            ZHLN::Test::ExpectTrue(blueTextPixels > 10u);

            if (greenBoxPixels < 1000u || blueTextPixels < 10u) {
                return std::unexpected(UITestError::UINotRendered);
            }

            ZHLN::Println("    [PASS] Immediate-mode UI rendered {} green box pixels and {} blue text pixels.", greenBoxPixels, blueTextPixels);
            return {};
        }

        std::expected<void, ZHLN::Error> button_click_interaction_and_states() {
            ZHLN::ECS::Registry registry;
            auto& input = registry.GetOrEmplaceSingleton<ZHLN::Components::InputStateComponent>();

            uint32_t clickCountA = 0;
            uint32_t clickCountB = 0;
            uint32_t hoverCountA = 0;

            // Frame 1: Initial layout establishment (mouse far away at 0, 0, unpressed)
            input.mouseX = 0.0f;
            input.mouseY = 0.0f;
            input.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::LButton), false);
            {
                ZHLN::GUI::Context gui(registry, {800, 600});
                gui.BeginFrame(0.016f);

                gui.Box(
                    "Container",
                    ZHLN::GUI::BoxConfig {
                        .width     = {.fixed = 400.0f},
                        .height    = {.fixed = 300.0f},
                        .padding   = 20.0f,
                        .gap       = 10.0f,
                        .direction = ZHLN::GUI::Direction::Column
                    },
                    [&]() {
                        bool clickedA = gui.Button("ButtonA", [&]() { clickCountA++; }, [&]() { hoverCountA++; });
                        bool clickedB = gui.Button("ButtonB", [&]() { clickCountB++; });

                        ZHLN::Test::ExpectFalse(clickedA);
                        ZHLN::Test::ExpectFalse(clickedB);
                        ZHLN::Test::ExpectFalse(gui.IsItemHovered());
                        ZHLN::Test::ExpectFalse(gui.IsItemActive());
                    }
                );

                gui.EndFrame();
            }

            ZHLN::Test::ExpectEq(clickCountA, 0u);
            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 2: Mouse hovers over ButtonA (approx x: 50, y: 40), still unpressed
            input.mouseX = 50.0f;
            input.mouseY = 40.0f;
            input.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::LButton), false);
            {
                ZHLN::GUI::Context gui(registry, {800, 600});
                gui.BeginFrame(0.016f);

                gui.Box(
                    "Container",
                    ZHLN::GUI::BoxConfig {
                        .width     = {.fixed = 400.0f},
                        .height    = {.fixed = 300.0f},
                        .padding   = 20.0f,
                        .gap       = 10.0f,
                        .direction = ZHLN::GUI::Direction::Column
                    },
                    [&]() {
                        bool clickedA = gui.Button("ButtonA", [&]() { clickCountA++; }, [&]() { hoverCountA++; });
                        ZHLN::Test::ExpectFalse(clickedA);
                        ZHLN::Test::ExpectTrue(gui.IsItemHovered());
                        ZHLN::Test::ExpectFalse(gui.IsItemActive());

                        bool clickedB = gui.Button("ButtonB", [&]() { clickCountB++; });
                        ZHLN::Test::ExpectFalse(clickedB);
                        ZHLN::Test::ExpectFalse(gui.IsItemHovered());
                        ZHLN::Test::ExpectFalse(gui.IsItemActive());
                    }
                );

                gui.EndFrame();
            }

            ZHLN::Test::ExpectEq(clickCountA, 0u);
            ZHLN::Test::ExpectTrue(hoverCountA > 0u);

            // Frame 3: Mouse pressed down this frame over ButtonA
            input.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::LButton), true);
            {
                ZHLN::GUI::Context gui(registry, {800, 600});
                gui.BeginFrame(0.016f);

                gui.Box(
                    "Container",
                    ZHLN::GUI::BoxConfig {
                        .width     = {.fixed = 400.0f},
                        .height    = {.fixed = 300.0f},
                        .padding   = 20.0f,
                        .gap       = 10.0f,
                        .direction = ZHLN::GUI::Direction::Column
                    },
                    [&]() {
                        bool clickedA = gui.Button("ButtonA", [&]() { clickCountA++; });
                        ZHLN::Test::ExpectTrue(clickedA);
                        ZHLN::Test::ExpectTrue(gui.IsItemHovered());
                        ZHLN::Test::ExpectTrue(gui.IsItemActive());

                        bool clickedB = gui.Button("ButtonB", [&]() { clickCountB++; });
                        ZHLN::Test::ExpectFalse(clickedB);
                        ZHLN::Test::ExpectFalse(gui.IsItemHovered());
                        ZHLN::Test::ExpectFalse(gui.IsItemActive());
                    }
                );

                gui.EndFrame();
            }

            ZHLN::Test::ExpectEq(clickCountA, 1u);
            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 4: Button is held down across next frame (single-fire guarantee: should NOT click again)
            {
                ZHLN::GUI::Context gui(registry, {800, 600});
                gui.BeginFrame(0.016f);

                gui.Box(
                    "Container",
                    ZHLN::GUI::BoxConfig {
                        .width     = {.fixed = 400.0f},
                        .height    = {.fixed = 300.0f},
                        .padding   = 20.0f,
                        .gap       = 10.0f,
                        .direction = ZHLN::GUI::Direction::Column
                    },
                    [&]() {
                        bool clickedA = gui.Button("ButtonA", [&]() { clickCountA++; });
                        ZHLN::Test::ExpectFalse(clickedA); // Hold should NOT re-trigger click
                        ZHLN::Test::ExpectTrue(gui.IsItemHovered());
                        ZHLN::Test::ExpectTrue(gui.IsItemActive());

                        bool clickedB = gui.Button("ButtonB", [&]() { clickCountB++; });
                        ZHLN::Test::ExpectFalse(clickedB);
                    }
                );

                gui.EndFrame();
            }

            ZHLN::Test::ExpectEq(clickCountA, 1u);

            // Frame 5: Mouse released over ButtonA
            input.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::LButton), false);
            {
                ZHLN::GUI::Context gui(registry, {800, 600});
                gui.BeginFrame(0.016f);

                gui.Box(
                    "Container",
                    ZHLN::GUI::BoxConfig {
                        .width     = {.fixed = 400.0f},
                        .height    = {.fixed = 300.0f},
                        .padding   = 20.0f,
                        .gap       = 10.0f,
                        .direction = ZHLN::GUI::Direction::Column
                    },
                    [&]() {
                        bool clickedA = gui.Button("ButtonA", [&]() { clickCountA++; });
                        ZHLN::Test::ExpectFalse(clickedA);
                        ZHLN::Test::ExpectTrue(gui.IsItemHovered());
                        ZHLN::Test::ExpectFalse(gui.IsItemActive());
                    }
                );

                gui.EndFrame();
            }

            ZHLN::Test::ExpectEq(clickCountA, 1u);

            // Frame 6: Mouse pressed outside (at 700, 500) and dragged onto ButtonB (at 50, 100)
            // Drag-into-click prevention test
            input.mouseX = 700.0f;
            input.mouseY = 500.0f;
            input.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::LButton), true);
            {
                ZHLN::GUI::Context gui(registry, {800, 600});
                gui.BeginFrame(0.016f);

                gui.Box(
                    "Container",
                    ZHLN::GUI::BoxConfig {
                        .width     = {.fixed = 400.0f},
                        .height    = {.fixed = 300.0f},
                        .padding   = 20.0f,
                        .gap       = 10.0f,
                        .direction = ZHLN::GUI::Direction::Column
                    },
                    [&]() {
                        gui.Button("ButtonA");
                        gui.Button("ButtonB", [&]() { clickCountB++; });
                    }
                );

                gui.EndFrame();
            }

            // Now move mouse over ButtonB while still held down from previous frame
            input.mouseX = 50.0f;
            input.mouseY = 100.0f;
            {
                ZHLN::GUI::Context gui(registry, {800, 600});
                gui.BeginFrame(0.016f);

                gui.Box(
                    "Container",
                    ZHLN::GUI::BoxConfig {
                        .width     = {.fixed = 400.0f},
                        .height    = {.fixed = 300.0f},
                        .padding   = 20.0f,
                        .gap       = 10.0f,
                        .direction = ZHLN::GUI::Direction::Column
                    },
                    [&]() {
                        gui.Button("ButtonA");
                        bool clickedB = gui.Button("ButtonB", [&]() { clickCountB++; });
                        ZHLN::Test::ExpectFalse(clickedB); // Drag-into-hover must NOT trigger click
                    }
                );

                gui.EndFrame();
            }

            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 7: Release mouse over ButtonB, then press again to verify ButtonB clicks cleanly
            input.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::LButton), false);
            {
                ZHLN::GUI::Context gui(registry, {800, 600});
                gui.BeginFrame(0.016f);
                gui.Box(
                    "Container",
                    ZHLN::GUI::BoxConfig {
                        .width     = {.fixed = 400.0f},
                        .height    = {.fixed = 300.0f},
                        .padding   = 20.0f,
                        .gap       = 10.0f,
                        .direction = ZHLN::GUI::Direction::Column
                    },
                    [&]() {
                        gui.Button("ButtonA");
                        gui.Button("ButtonB");
                    }
                );
                gui.EndFrame();
            }

            input.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::LButton), true);
            {
                ZHLN::GUI::Context gui(registry, {800, 600});
                gui.BeginFrame(0.016f);
                gui.Box(
                    "Container",
                    ZHLN::GUI::BoxConfig {
                        .width     = {.fixed = 400.0f},
                        .height    = {.fixed = 300.0f},
                        .padding   = 20.0f,
                        .gap       = 10.0f,
                        .direction = ZHLN::GUI::Direction::Column
                    },
                    [&]() {
                        gui.Button("ButtonA");
                        bool clickedB = gui.Button("ButtonB", [&]() { clickCountB++; });
                        ZHLN::Test::ExpectTrue(clickedB);
                    }
                );
                gui.EndFrame();
            }

            ZHLN::Test::ExpectEq(clickCountA, 1u);
            ZHLN::Test::ExpectEq(clickCountB, 1u);

            return {};
        }
    };
};

auto RunUISuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<UITestSuite>();
}
