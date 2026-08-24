// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "imgui.h"
#include <Zahlen/Components.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <algorithm>
#include <cstdint>
#include <expected>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

enum class ImGuiRenderTestError : uint8_t {
    Success = 0,
    EngineInitFailed[[= ZHLN::Reflect::Description("Failed to initialize headless Engine context for ImGui render test.")]],
    RenderOutputBlank[[= ZHLN::Reflect::Description("Rendered ImGui screenshot is blank or failed to capture.")]],
    MissingExpectedPixels[[= ZHLN::Reflect::Description("PPM analysis did not find the expected ImGui colored regions.")]],
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
        return std::unexpected(ImGuiRenderTestError::RenderOutputBlank);
    }

    PpmImage    image;
    std::string header;
    int         maxColor = 0;
    ppm >> header >> image.width >> image.height >> maxColor;
    ppm.get();
    if (header != "P6" || image.width <= 0 || image.height <= 0 || maxColor != 255) {
        return std::unexpected(ImGuiRenderTestError::RenderOutputBlank);
    }

    image.pixels.resize(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3u);
    ppm.read(reinterpret_cast<char*>(image.pixels.data()), static_cast<std::streamsize>(image.pixels.size()));
    if (ppm.gcount() != static_cast<std::streamsize>(image.pixels.size())) {
        return std::unexpected(ImGuiRenderTestError::RenderOutputBlank);
    }
    return image;
}

[[nodiscard]] uint32_t CountPixels(
    const PpmImage& image,
    int             x0,
    int             y0,
    int             x1,
    int             y1,
    bool (*predicate)(uint8_t, uint8_t, uint8_t)
) {
    uint32_t count = 0;
    x0             = std::clamp(x0, 0, image.width);
    y0             = std::clamp(y0, 0, image.height);
    x1             = std::clamp(x1, 0, image.width);
    y1             = std::clamp(y1, 0, image.height);

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const size_t  i = (static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * 3u;
            const uint8_t r = image.pixels[i + 0];
            const uint8_t g = image.pixels[i + 1];
            const uint8_t b = image.pixels[i + 2];
            if (predicate(r, g, b)) {
                ++count;
            }
        }
    }
    return count;
}

[[nodiscard]] bool IsRed(uint8_t r, uint8_t g, uint8_t b) {
    return r > 200 && g < 70 && b < 70;
}

[[nodiscard]] bool IsGreen(uint8_t r, uint8_t g, uint8_t b) {
    return g > 200 && r < 70 && b < 70;
}

[[nodiscard]] bool IsBlue(uint8_t r, uint8_t g, uint8_t b) {
    return b > 200 && r < 70 && g < 90;
}

} // namespace

struct ImGuiRenderTestSuite {
    ImGuiRenderTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~ImGuiRenderTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        std::expected<void, ZHLN::Error> imgui_draw_data_is_rendered_into_headless_ppm() {
            ZHLN::DefaultPreset::SetDisabled(true);

            const ZHLN::EngineConfig cfg {
                .physics = {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 4 * 1024 * 1024},
                .render  = {
                    .appName        = "Headless ImGui Screenshot",
                    .width          = 320,
                    .height         = 240,
                    .vsync          = false,
                    .fullscreen     = false,
                    .validationMode = ZHLN::ValidationMode::On,
                    .headless       = true,
                },
            };

            auto engineRes = ZHLN::Engine::Create(cfg);
            if (!engineRes) {
                return std::unexpected(ImGuiRenderTestError::EngineInitFailed);
            }

            auto engine = std::move(engineRes.value());
            engine->InitializeDefaultScene();

            auto& reg = engine->GetRegistry();
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

            engine->SetUICallback([](ZHLN::Engine&) {
                ImGui::SetNextWindowPos(ImVec2(32.0f, 24.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(192.0f, 96.0f), ImGuiCond_Always);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
                ImGui::Begin(
                    "ImGuiPPMSentinel", nullptr,
                    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                );

                ImDrawList* drawList = ImGui::GetWindowDrawList();
                const ImVec2 origin  = ImGui::GetWindowPos();
                drawList->AddRectFilled(ImVec2(origin.x + 12.0f, origin.y + 12.0f), ImVec2(origin.x + 172.0f, origin.y + 32.0f), IM_COL32(255, 0, 0, 255));
                drawList->AddRectFilled(ImVec2(origin.x + 12.0f, origin.y + 38.0f), ImVec2(origin.x + 172.0f, origin.y + 58.0f), IM_COL32(0, 255, 0, 255));
                drawList->AddRectFilled(ImVec2(origin.x + 12.0f, origin.y + 64.0f), ImVec2(origin.x + 172.0f, origin.y + 84.0f), IM_COL32(0, 0, 255, 255));

                ImGui::End();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar(2);
            });

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 6; ++frame) {
                engine->GetRenderContext().BeginImGuiFrame();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            const std::string ppmPath = "headless_imgui_render.ppm";
            if (!engine->GetRenderContext().CaptureScreenshotPPM(ppmPath)) {
                return std::unexpected(ImGuiRenderTestError::RenderOutputBlank);
            }

            auto imageRes = LoadPPM(ppmPath);
            if (!imageRes) {
                return std::unexpected(imageRes.error());
            }
            const PpmImage& image = *imageRes;

            const uint32_t red   = CountPixels(image, 40, 32, 210, 62, IsRed);
            const uint32_t green = CountPixels(image, 40, 58, 210, 88, IsGreen);
            const uint32_t blue  = CountPixels(image, 40, 84, 210, 114, IsBlue);

            ZHLN::Test::ExpectTrue(red > 1800u);
            ZHLN::Test::ExpectTrue(green > 1800u);
            ZHLN::Test::ExpectTrue(blue > 1800u);

            if (red <= 1800u || green <= 1800u || blue <= 1800u) {
                return std::unexpected(ImGuiRenderTestError::MissingExpectedPixels);
            }
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ImGuiRenderTestSuite>();
}
