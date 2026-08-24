// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cstdint>
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

enum class UILayoutRenderError : uint8_t {
    Success = 0,
    EngineInitFailed[[= ZHLN::Reflect::Description("Failed to initialize headless Engine context for UI layout test.")]],
    RenderOutputBlank[[= ZHLN::Reflect::Description("Rendered frame is blank or failed to capture.")]],
    LayoutMismatch[[= ZHLN::Reflect::Description("Colored UI bands are not in the expected flex-column positions.")]],
};

namespace {

struct PpmImage {
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> pixels;
};

[[nodiscard]] auto LoadPPM(const std::string& path) -> std::expected<PpmImage, ZHLN::Error> {
    std::ifstream ppm(path, std::ios::binary);
    if (!ppm.is_open()) {
        return std::unexpected(UILayoutRenderError::RenderOutputBlank);
    }

    PpmImage    image;
    std::string header;
    int         maxColor = 0;
    ppm >> header >> image.width >> image.height >> maxColor;
    ppm.get();
    if (header != "P6" || image.width <= 0 || image.height <= 0) {
        return std::unexpected(UILayoutRenderError::RenderOutputBlank);
    }

    image.pixels.resize(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3u);
    ppm.read(reinterpret_cast<char*>(image.pixels.data()), static_cast<std::streamsize>(image.pixels.size()));
    if (ppm.gcount() != static_cast<std::streamsize>(image.pixels.size())) {
        return std::unexpected(UILayoutRenderError::RenderOutputBlank);
    }
    return image;
}

void Sample(const PpmImage& image, int x, int y, uint8_t& r, uint8_t& g, uint8_t& b) {
    x = std::clamp(x, 0, image.width - 1);
    y = std::clamp(y, 0, image.height - 1);
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * 3u;
    r              = image.pixels[i + 0];
    g              = image.pixels[i + 1];
    b              = image.pixels[i + 2];
}

[[nodiscard]] bool IsRed(uint8_t r, uint8_t g, uint8_t b) {
    return r > 160 && g < 60 && b < 60;
}

[[nodiscard]] bool IsGreen(uint8_t r, uint8_t g, uint8_t b) {
    return g > 160 && r < 60 && b < 60;
}

[[nodiscard]] uint32_t CountInRect(const PpmImage& image, int x0, int y0, int x1, int y1, bool (*pred)(uint8_t, uint8_t, uint8_t)) {
    uint32_t count = 0;
    x0             = std::max(0, x0);
    y0             = std::max(0, y0);
    x1             = std::min(image.width, x1);
    y1             = std::min(image.height, y1);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            uint8_t r = 0, g = 0, b = 0;
            Sample(image, x, y, r, g, b);
            if (pred(r, g, b)) {
                count++;
            }
        }
    }
    return count;
}

} // namespace

struct UILayoutRenderTestSuite {
    UILayoutRenderTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~UILayoutRenderTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // Top-left column: padding 10, gap 10, two 50px bands (red then green).
        // Pixel bands must sit at y≈10..60 and y≈70..120, not at the default centered popup.
        std::expected<void, ZHLN::Error> flex_column_colored_bands_match_layout() {
            ZHLN::DefaultPreset::SetDisabled(true);

            const ZHLN::EngineConfig cfg {
                .physics = {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 4 * 1024 * 1024},
                .render  = {
                    .appName        = "Headless UI Layout",
                    .width          = 640,
                    .height         = 480,
                    .vsync          = false,
                    .fullscreen     = false,
                    .validationMode = ZHLN::ValidationMode::On,
                    .headless       = true
                }
            };

            auto engineRes = ZHLN::Engine::Create(cfg);
            if (!engineRes) {
                return std::unexpected(UILayoutRenderError::EngineInitFailed);
            }

            auto engine = std::move(engineRes.value());
            engine->InitializeDefaultScene();

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            for (ZHLN::Entity camEnt: reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>()) {
                reg.Patch<ZHLN::Components::AASettingsComponent>(camEnt, [](auto& aa) { aa.state.mode = ZHLN::AAMode::None; });
            }
            auto settings = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            if (!settings.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [](auto& pp) {
                    pp.fullBright        = 1;
                    pp.vignetteIntensity = 0.0f;
                    pp.enableSSR         = 0;
                    pp.enableRTR         = 0;
                });
            }

            engine->SetUICallback([](ZHLN::Engine& e) {
                ZHLN::GUI::Context ui(e.GetRegistry(), e.GetCurrentFrame());

                const ZHLN::GUI::PanelConfig rootCfg {
                    .width      = 400.0f,
                    .height     = 300.0f,
                    .x          = 0.0f,
                    .y          = 0.0f,
                    .anchorMinX = 0.0f,
                    .anchorMinY = 0.0f,
                    .anchorMaxX = 0.0f,
                    .anchorMaxY = 0.0f,
                    .color      = {0.04f, 0.04f, 0.08f, 1.0f},
                    .edgeWidth  = 0.0f,
                    .direction  = ZHLN::FlexDirection::Column,
                    .justify    = ZHLN::FlexJustify::FlexStart,
                    .gap        = 10.0f,
                    .padding    = 10.0f,
                };

                ui.BeginPanel("layout_root", rootCfg, [&] {
                    ui.BeginBox("band_red", ZHLN::GUI::BoxConfig {.height = 50.0f, .color = {1.0f, 0.0f, 0.0f, 1.0f}, .edgeWidth = 0.0f, .padding = 0.0f}, [] {});
                    ui.BeginBox(
                        "band_green", ZHLN::GUI::BoxConfig {.height = 50.0f, .color = {0.0f, 1.0f, 0.0f, 1.0f}, .edgeWidth = 0.0f, .padding = 0.0f}, [] {}
                    );
                });
            });

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 8; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            const std::string ppmPath    = "headless_ui_layout.ppm";
            const auto        captureRes = rc.CaptureScreenshotPPM(ppmPath);
            if (!captureRes) {
                return std::unexpected(UILayoutRenderError::RenderOutputBlank);
            }

            auto imageRes = LoadPPM(ppmPath);
            if (!imageRes) {
                return std::unexpected(imageRes.error());
            }
            const PpmImage& image = *imageRes;

            // Interior of each band (avoid the 1px edge / Yoga rounding).
            const uint32_t redInBand    = CountInRect(image, 20, 18, 380, 52, IsRed);
            const uint32_t greenInBand  = CountInRect(image, 20, 78, 380, 112, IsGreen);
            const uint32_t redInGreen   = CountInRect(image, 20, 78, 380, 112, IsRed);
            const uint32_t greenInRed   = CountInRect(image, 20, 18, 380, 52, IsGreen);
            const uint32_t redInGap     = CountInRect(image, 20, 62, 380, 68, IsRed);
            const uint32_t greenInGap   = CountInRect(image, 20, 62, 380, 68, IsGreen);
            const uint32_t redOffCanvas = CountInRect(image, 500, 200, 630, 460, IsRed);

            const uint32_t bandArea = static_cast<uint32_t>((380 - 20) * (52 - 18));
            ZHLN::Test::ExpectTrue(redInBand > bandArea / 2u);
            ZHLN::Test::ExpectTrue(greenInBand > bandArea / 2u);
            ZHLN::Test::ExpectTrue(redInGreen < 40u);
            ZHLN::Test::ExpectTrue(greenInRed < 40u);
            ZHLN::Test::ExpectTrue(redInGap + greenInGap < 80u);
            ZHLN::Test::ExpectTrue(redOffCanvas < 40u);

            if (redInBand <= bandArea / 2u || greenInBand <= bandArea / 2u || redInGreen >= 40u || greenInRed >= 40u ||
                redInGap + greenInGap >= 80u || redOffCanvas >= 40u) {
                return std::unexpected(UILayoutRenderError::LayoutMismatch);
            }

            ZHLN::Println("    [PASS] UI flex column: {} red / {} green pixels in expected bands.", redInBand, greenInBand);
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<UILayoutRenderTestSuite>();
}
