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
    EngineInitFailed  ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize headless Engine context for UI test."> {}) = 1,
    RenderOutputBlank ZHLN_ANNOTATION(ZHLN::Description<"Rendered frame is blank or failed to capture."> {}),
    UINotRendered     ZHLN_ANNOTATION(ZHLN::Description<"Automated pixel analysis detected zero UI pixels on screen."> {}),
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
    };
};

auto RunUISuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<UITestSuite>();
}
