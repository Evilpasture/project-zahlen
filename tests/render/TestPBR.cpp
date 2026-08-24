// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// Test Error Types
// ============================================================================

enum class PBRTestError : uint8_t {
    Success = 0,
    EngineInitFailed[[= ZHLN::Reflect::Description("Failed to initialize headless Engine context for PBR test.")]],
    RenderOutputBlank[[= ZHLN::Reflect::Description("Rendered frame is blank or failed to capture.")]],
    SpecularHighlightNotDetected[[= ZHLN::Reflect::Description("PBR specular reflection highlight was not observed on target surface.")]],
    MaterialCreationFailed[[= ZHLN::Reflect::Description("CreativeWorksFactory::CreateMaterial failed to construct GPU pipeline.")]],
};

// ============================================================================
// Test Suite Class
// ============================================================================

struct PBRTestSuite {
    PBRTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~PBRTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);

        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless PBR Test",
                .width          = width,
                .height         = height,
                .vsync          = false,
                .fullscreen     = false,
                .validationMode = ZHLN::ValidationMode::On,
                .headless       = true
            }
        };

        auto engineRes = ZHLN::Engine::Create(cfg);
        if (!engineRes) {
            return nullptr;
        }

        auto engine = std::move(engineRes.value());
        engine->InitializeDefaultScene();
        return engine;
    }

    struct Tests {
        // ====================================================================
        // Dielectric vs. Metallic Direct Lighting & Color Tinting
        // ====================================================================
        std::expected<void, ZHLN::Error> pbr_dielectric_vs_metallic_surface_response() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return checkEngine;
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // Set Fullbright = 0 to evaluate full PBR lighting and tonemapping
            auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            if (!settingsEnts.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                    pp.fullBright      = 0;
                    pp.ambientExposure = 12.0f;
                });
            }

            // 1. Sun Directional Light aiming directly toward front surfaces (+Z direction from camera view)
            const ZHLN::Entity sunEnt = reg.Create();
            reg.Add(
                sunEnt,
                ZHLN::Components::TransformComponent {
                    .position = JPH::Vec3(0.0f, 10.0f, 10.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({20.0f, 0.0f, 0.0f})
                },
                ZHLN::Components::LightComponent {
                    .type      = ZHLN::LightType::Sun,
                    .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                    .intensity = 220.0f,
                    .direction = JPH::Vec3(0.0f, 0.35f, 0.93f).Normalized() // Direction TO the sun
                }
            );

            // 2. Camera looking forward along -Z toward (0, 1.0, 0)
            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 1.0f, 4.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;

            // 3. Construct PBR Gold Metallic Material (metallic = 1.0, roughness = 0.25)
            auto goldMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.25f, .baseColor = {1.0f, 0.84f, 0.0f, 1.0f}}
            );
            if (!goldMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            const ZHLN::Entity goldCube = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(-1.2, 1.0, 0.0), .createPhysics = false, .materialOverride = *goldMatRes}
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(goldCube));

            // 4. Construct PBR Plastic Red Dielectric Material (metallic = 0.0, roughness = 0.25)
            auto redMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.25f, .baseColor = {1.0f, 0.05f, 0.05f, 1.0f}}
            );
            if (!redMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            const ZHLN::Entity redCube = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(1.2, 1.0, 0.0), .createPhysics = false, .materialOverride = *redMatRes}
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(redCube));

            // 5. Render 15 frames of PBR lighting evaluation
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 15; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            // 6. Capture rendered frame and validate pixel histogram
            const std::string ppmPath    = "headless_pbr_output.ppm";
            const auto        captureRes = rc.CaptureScreenshotPPM(ppmPath);
            if (!captureRes) {
                return std::unexpected(PBRTestError::RenderOutputBlank);
            }

            std::ifstream ppm(ppmPath, std::ios::binary);
            if (!ppm.is_open()) {
                return std::unexpected(PBRTestError::RenderOutputBlank);
            }

            std::string header;
            int         width = 0, height = 0, maxColor = 0;
            ppm >> header >> width >> height >> maxColor;
            ppm.get();

            std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 3));
            ppm.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

            uint32_t goldColoredPixels = 0;
            uint32_t redColoredPixels  = 0;

            for (size_t i = 0; i < pixels.size(); i += 3) {
                const uint8_t r = pixels[i + 0];
                const uint8_t g = pixels[i + 1];
                const uint8_t b = pixels[i + 2];

                // Detect Gold metallic reflections (High Red & Green, Low Blue)
                if (r > 60 && g > 40 && b < 40 && r >= g) {
                    goldColoredPixels++;
                }

                // Detect Red dielectric diffuse (High Red, Low Green & Blue)
                if (r > 60 && g < 40 && b < 40) {
                    redColoredPixels++;
                }
            }

            ZHLN::Test::ExpectTrue(goldColoredPixels > 100u);
            ZHLN::Test::ExpectTrue(redColoredPixels > 100u);

            if (goldColoredPixels < 100u || redColoredPixels < 100u) {
                return std::unexpected(PBRTestError::SpecularHighlightNotDetected);
            }

            ZHLN::Println("    [PASS] PBR validated: {} gold metallic pixels, {} red dielectric pixels.", goldColoredPixels, redColoredPixels);
            return {};
        }

        // ====================================================================
        // 3. Roughness Microfacet Specular Broadening (pixel compactness)
        // ====================================================================
        std::expected<void, ZHLN::Error> pbr_roughness_distribution_broadening() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return checkEngine;
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            for (ZHLN::Entity camEnt: reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>()) {
                reg.Remove<ZHLN::Components::FreeCamTagComponent>(camEnt);
                reg.Patch<ZHLN::Components::AASettingsComponent>(camEnt, [](auto& aa) { aa.state.mode = ZHLN::AAMode::None; });
            }

            auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            if (!settingsEnts.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                    pp.fullBright        = 0;
                    pp.ambientExposure   = 12.0f;
                    pp.vignetteIntensity = 0.0f;
                    pp.enableSSR         = 0;
                    pp.enableRTR         = 0;
                });
            }

            const ZHLN::Entity sunEnt = reg.Create();
            reg.Add(
                sunEnt,
                ZHLN::Components::TransformComponent {
                    .position = JPH::Vec3(0.0f, 10.0f, 10.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({20.0f, 0.0f, 0.0f})
                },
                ZHLN::Components::LightComponent {
                    .type      = ZHLN::LightType::Sun,
                    .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                    .intensity = 220.0f,
                    // Behind the camera so N≈V≈L on the +Z cube faces; a
                    // 20° elevation misses the GGX peak of a 0.05 chrome.
                    .direction = JPH::Vec3(0.0f, 0.08f, 0.997f).Normalized()
                }
            );

            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 1.0f, 4.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;

            // Metals keep a visible highlight after the screenshot's ACES×0.015
            // mapping; a gray dielectric lands below L=80 and the warm-pixel
            // gate never fires. Same albedo, only roughness differs.
            auto smoothMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.05f, .baseColor = {0.92f, 0.92f, 0.94f, 1.0f}}
            );
            if (!smoothMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            auto roughMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.85f, .baseColor = {0.92f, 0.92f, 0.94f, 1.0f}}
            );
            if (!roughMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            const ZHLN::Entity smoothBox = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(-1.15, 1.0, 0.0), .createPhysics = false, .materialOverride = *smoothMatRes}
            );

            const ZHLN::Entity roughBox = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(1.15, 1.0, 0.0), .createPhysics = false, .materialOverride = *roughMatRes}
            );

            ZHLN::Test::ExpectTrue(reg.IsAlive(smoothBox));
            ZHLN::Test::ExpectTrue(reg.IsAlive(roughBox));

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 12; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            const std::string ppmPath    = "headless_pbr_roughness.ppm";
            const auto        captureRes = engine->GetRenderContext().CaptureScreenshotPPM(ppmPath);
            if (!captureRes) {
                return std::unexpected(PBRTestError::RenderOutputBlank);
            }

            std::ifstream ppm(ppmPath, std::ios::binary);
            if (!ppm.is_open()) {
                return std::unexpected(PBRTestError::RenderOutputBlank);
            }

            std::string header;
            int         width = 0, height = 0, maxColor = 0;
            ppm >> header >> width >> height >> maxColor;
            ppm.get();

            std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 3));
            ppm.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

            auto luminance = [](uint8_t r, uint8_t g, uint8_t b) -> float {
                return 0.2126f * static_cast<float>(r) + 0.7152f * static_cast<float>(g) + 0.0722f * static_cast<float>(b);
            };

            struct HalfStats {
                float    maxL     = 0.0f;
                uint32_t warm     = 0; // L > 25  — lit surface after ACES×0.015
                uint32_t hot      = 0; // L > 140 — specular peak
                double   sumX     = 0.0;
                double   sumY     = 0.0;
                double   sumX2    = 0.0;
                double   sumY2    = 0.0;
                uint32_t highlight = 0;
            };

            // Ignore the sky strip; the cubes sit in the middle of the frame.
            const int y0 = height / 6;
            const int y1 = (height * 5) / 6;

            auto analyzeHalf = [&](int x0, int x1) -> HalfStats {
                HalfStats s;
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const size_t  i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3u;
                        const float   L = luminance(pixels[i], pixels[i + 1], pixels[i + 2]);
                        s.maxL          = std::max(s.maxL, L);
                        if (L > 25.0f) {
                            s.warm++;
                        }
                        if (L > 140.0f) {
                            s.hot++;
                        }
                    }
                }
                const float hi = std::max(40.0f, s.maxL * 0.70f);
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3u;
                        const float  L = luminance(pixels[i], pixels[i + 1], pixels[i + 2]);
                        if (L < hi) {
                            continue;
                        }
                        const double dx = static_cast<double>(x);
                        const double dy = static_cast<double>(y);
                        s.sumX += dx;
                        s.sumY += dy;
                        s.sumX2 += dx * dx;
                        s.sumY2 += dy * dy;
                        s.highlight++;
                    }
                }
                return s;
            };

            const int       mid    = width / 2;
            const HalfStats smooth = analyzeHalf(0, mid);
            const HalfStats rough  = analyzeHalf(mid, width);

            auto compactness = [](const HalfStats& s) -> double {
                if (s.highlight < 4u) {
                    return 1.0e9;
                }
                const double n  = static_cast<double>(s.highlight);
                const double mx = s.sumX / n;
                const double my = s.sumY / n;
                return (s.sumX2 / n - mx * mx) + (s.sumY2 / n - my * my);
            };

            const double smoothSpread = compactness(smooth);
            const double roughSpread  = compactness(rough);
            const double smoothPeak   = (smooth.warm > 0) ? static_cast<double>(smooth.hot) / static_cast<double>(smooth.warm) : 0.0;
            const double roughPeak    = (rough.warm > 0) ? static_cast<double>(rough.hot) / static_cast<double>(rough.warm) : 0.0;

            ZHLN::Println(
                "    [INFO] PBR roughness: smooth warm={} hot={} maxL={:.1f} spread={:.1f} peak={:.3f}; "
                "rough warm={} hot={} maxL={:.1f} spread={:.1f} peak={:.3f}",
                smooth.warm, smooth.hot, smooth.maxL, smoothSpread, smoothPeak, rough.warm, rough.hot, rough.maxL, roughSpread, roughPeak
            );

            ZHLN::Test::ExpectTrue(smooth.warm > 50u && rough.warm > 50u);
            // Low roughness concentrates energy (tighter / hotter highlight).
            const bool tighter  = smoothSpread + 8.0 < roughSpread;
            const bool hotter   = smoothPeak > roughPeak + 0.02 && smooth.maxL + 4.0f >= rough.maxL;
            ZHLN::Test::ExpectTrue(tighter || hotter);

            if (smooth.warm <= 50u || rough.warm <= 50u || !(tighter || hotter)) {
                return std::unexpected(PBRTestError::SpecularHighlightNotDetected);
            }

            ZHLN::Println(
                "    [PASS] PBR roughness: smooth spread={:.1f} peak={:.3f} maxL={:.1f}; rough spread={:.1f} peak={:.3f} maxL={:.1f}.", smoothSpread,
                smoothPeak, smooth.maxL, roughSpread, roughPeak, rough.maxL
            );
            return {};
        }

        // ====================================================================
        // 4. Energy Conservation & Fullbright Override
        // ====================================================================
        std::expected<void, ZHLN::Error> pbr_fullbright_mode_override() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return checkEngine;
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // Set Fullbright ON: Disables direct lighting/shadows and outputs raw G-buffer albedo directly
            auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            if (!settingsEnts.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) { pp.fullBright = 1; });
            }

            // Create explicit bright green material
            auto greenMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.0f, 1.0f, 0.0f, 1.0f}}
            );
            if (!greenMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            // Green flat box placed in front of camera
            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 1.0f, 3.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;

            ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(2.0f, 2.0f, 0.1f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.0, 0.0), .createPhysics = false, .materialOverride = *greenMatRes}
            );

            // Tick 5 frames
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 5; ++frame) {
                engine->ProcessEvents();
                engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
            }

            const std::string ppmPath    = "headless_pbr_fullbright.ppm";
            const auto        captureRes = rc.CaptureScreenshotPPM(ppmPath);
            if (!captureRes) {
                return std::unexpected(PBRTestError::RenderOutputBlank);
            }

            std::ifstream ppm(ppmPath, std::ios::binary);
            if (!ppm.is_open()) {
                return std::unexpected(PBRTestError::RenderOutputBlank);
            }

            std::string header;
            int         width = 0, height = 0, maxColor = 0;
            ppm >> header >> width >> height >> maxColor;
            ppm.get();

            std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 3));
            ppm.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

            uint32_t pureGreenPixels = 0;
            for (size_t i = 0; i < pixels.size(); i += 3) {
                const uint8_t r = pixels[i + 0];
                const uint8_t g = pixels[i + 1];
                const uint8_t b = pixels[i + 2];

                // Fullbright mode preserves raw green albedo without shadow/shading attenuation
                if (g > 200 && r < 50 && b < 50) {
                    pureGreenPixels++;
                }
            }

            ZHLN::Test::ExpectTrue(pureGreenPixels > 500u);
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<PBRTestSuite>();
}
