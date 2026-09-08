// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/gui/GUI.hpp>
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
            auto engine = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(UITestError::EngineInitFailed);
            }

            auto setInput = [&](float mx, float my, bool lbutton) {
                auto& in = engine->GetRegistry().GetOrEmplaceSingleton<ZHLN::Components::InputStateComponent>();
                in.mouseX = mx;
                in.mouseY = my;
                in.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::LButton), lbutton);
            };

            uint32_t clickCountA = 0;
            uint32_t clickCountB = 0;
            uint32_t hoverCountA = 0;
            bool     isHoveredA  = false;
            bool     isHoveredB  = false;
            bool     isActiveA   = false;
            bool     isActiveB   = false;
            bool     wasClickedA = false;
            bool     wasClickedB = false;

            engine->SetUICallback([&](ZHLN::Engine& eng) {
                ZHLN::GUI::Context ui(eng);
                ui.BeginFrame(0.016f);

                ui.Box(
                    "Container",
                    ZHLN::GUI::BoxConfig {
                        .width     = {.fixed = 300.0f},
                        .height    = {.fixed = 200.0f},
                        .padding   = 20.0f,
                        .gap       = 10.0f,
                        .direction = ZHLN::GUI::Direction::Column
                    },
                    [&]() {
                        wasClickedA = ui.Button("ButtonA", [&]() { clickCountA++; }, [&]() { hoverCountA++; });
                        isHoveredA  = ui.IsItemHovered();
                        isActiveA   = ui.IsItemActive();

                        wasClickedB = ui.Button("ButtonB", [&]() { clickCountB++; });
                        isHoveredB  = ui.IsItemHovered();
                        isActiveB   = ui.IsItemActive();
                    }
                );

                ui.EndFrameAndRender(eng.GetRenderContext());
            });

            constexpr float dt = 1.0f / 60.0f;

            // Frame 1: Initial layout establishment (mouse at 0, 0, unpressed)
            setInput(0.0f, 0.0f, false);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectFalse(wasClickedA);
            ZHLN::Test::ExpectFalse(wasClickedB);
            ZHLN::Test::ExpectEq(clickCountA, 0u);
            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 2: Mouse hovers over ButtonA (approx x: 50, y: 35), unpressed
            setInput(50.0f, 35.0f, false);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectFalse(wasClickedA);
            ZHLN::Test::ExpectTrue(isHoveredA);
            ZHLN::Test::ExpectFalse(isActiveA);
            ZHLN::Test::ExpectFalse(wasClickedB);
            ZHLN::Test::ExpectFalse(isHoveredB);
            ZHLN::Test::ExpectFalse(isActiveB);
            ZHLN::Test::ExpectEq(clickCountA, 0u);
            ZHLN::Test::ExpectTrue(hoverCountA > 0u);

            // Frame 3: Mouse pressed down this frame over ButtonA
            setInput(50.0f, 35.0f, true);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectTrue(wasClickedA);
            ZHLN::Test::ExpectTrue(isHoveredA);
            ZHLN::Test::ExpectTrue(isActiveA);
            ZHLN::Test::ExpectFalse(wasClickedB);
            ZHLN::Test::ExpectEq(clickCountA, 1u);
            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 4: Button is held down across next frame (single-fire guarantee: should NOT click again)
            setInput(50.0f, 35.0f, true);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectFalse(wasClickedA);
            ZHLN::Test::ExpectTrue(isHoveredA);
            ZHLN::Test::ExpectTrue(isActiveA);
            ZHLN::Test::ExpectFalse(wasClickedB);
            ZHLN::Test::ExpectEq(clickCountA, 1u);

            // Frame 5: Mouse released over ButtonA
            setInput(50.0f, 35.0f, false);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectFalse(wasClickedA);
            ZHLN::Test::ExpectTrue(isHoveredA);
            ZHLN::Test::ExpectFalse(isActiveA);
            ZHLN::Test::ExpectEq(clickCountA, 1u);

            // Frame 6: Mouse pressed outside (at 500, 400) and dragged onto ButtonB (at 50, 85)
            // Drag-into-click prevention test
            setInput(500.0f, 400.0f, true);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            // Move mouse over ButtonB while still held down from previous frame
            setInput(50.0f, 85.0f, true);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectFalse(wasClickedB); // Drag-into-hover must NOT trigger click
            ZHLN::Test::ExpectEq(clickCountB, 0u);

            // Frame 7: Release mouse over ButtonB, then press again to verify ButtonB clicks cleanly
            setInput(50.0f, 85.0f, false);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            setInput(50.0f, 85.0f, true);
            engine->ProcessEvents();
            engine->Tick(dt, ZHLN::GameplayDriver::Cpp);

            ZHLN::Test::ExpectTrue(wasClickedB);
            ZHLN::Test::ExpectEq(clickCountA, 1u);
            ZHLN::Test::ExpectEq(clickCountB, 1u);

            return {};
        }
    };
};

auto RunUISuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<UITestSuite>();
}
