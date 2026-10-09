// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <AssetCooking/EnvironmentPreparation.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render/EnvironmentImage.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ============================================================================
// Test Error Types
// ============================================================================

enum class PBRTestError : uint8_t {
    EngineInitFailed             ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize headless Engine context for PBR test."> {}) = 1,
    RenderOutputBlank            ZHLN_ANNOTATION(ZHLN::Description<"Rendered frame is blank or failed to capture."> {}),
    SpecularHighlightNotDetected ZHLN_ANNOTATION(ZHLN::Description<"PBR specular reflection highlight was not observed on target surface."> {}),
    MaterialCreationFailed       ZHLN_ANNOTATION(ZHLN::Description<"RenderContext::CreateMaterial failed to construct GPU pipeline."> {}),
    EnvironmentBakeFailed       ZHLN_ANNOTATION(ZHLN::Description<"The prepared HDR environment could not be registered or baked."> {}),
    ShadowSideDiffuseTinted     ZHLN_ANNOTATION(ZHLN::Description<"A compact HDR emitter behind an opaque surface changed its diffuse color."> {}),
    CookedSunPolicyFailed       ZHLN_ANNOTATION(ZHLN::Description<"Cooked HDR sun was duplicated, not released, or did not cast a shadow."> {}),
};

// ============================================================================
// Test Suite Class
// ============================================================================

struct PBRTestSuite {
    PBRTestSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~PBRTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    // Pooled: one engine per resolution for the whole binary, with the
    // scene reset between tests. Creating a Vulkan instance per test is
    // what eventually exhausts the loader's static TLS and turns the tail
    // of the group into "vkCreateInstance: Found no drivers!".
    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> ZHLN::Test::Headless::EngineHandle {
        return ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {
            .appName = "Headless PBR Test", .width = width, .height = height
        });
    }

    struct Tests {
        // ====================================================================
        // Dielectric vs. Metallic Direct Lighting & Color Tinting
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> pbr_dielectric_vs_metallic_surface_response() {
            auto engine      = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(PBRTestError::EngineInitFailed);
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // Set Fullbright = 0 to evaluate full PBR lighting and tonemapping.
            // Scene levels are load-bearing: sun 220 + exposure 12 overexpose
            // the red dielectric ~10x, so even its 5%-albedo channels clip and
            // Khronos Neutral desaturates the result to white (PNG-proven: the
            // "red" box rendered pure white). Levels match the culling-sweep
            // scene's proven-sane combo; the INFO means below confirm red
            // reads red -- if a future grade pushes faces back toward clip,
            // re-level here, never in the gates.
            auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            if (!settingsEnts.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                    pp.fullBright      = 0;
                    pp.ambientExposure = 2.0f;
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
                    .intensity = 40.0f,
                    .direction = JPH::Vec3(0.0f, 0.35f, 0.93f).Normalized() // Direction TO the sun
                }
            );

            // 2. Camera looking forward along -Z toward (0, 1.0, 0)
            const auto camComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
            if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }
            ZHLN::Camera& cam = camComp->camera;
            cam.position = JPH::Vec3(0.0f, 1.0f, 4.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;

            // 3. Construct PBR Gold Metallic Material (metallic = 1.0, roughness = 0.25)
            auto goldMatRes = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 1.0f, .roughness = 0.25f, .baseColor = {1.0f, 0.84f, 0.0f, 1.0f}});
            if (!goldMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            const ZHLN::Entity goldCube = ZHLN::PrefabFactory::CreateBox(
                *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(-1.2, 1.0, 0.0), .createPhysics = false, .materialOverride = *goldMatRes}
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(goldCube));

            // 4. Construct PBR Plastic Red Dielectric Material (metallic = 0.0, roughness = 0.25)
            auto redMatRes = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.25f, .baseColor = {1.0f, 0.05f, 0.05f, 1.0f}});
            if (!redMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            const ZHLN::Entity redCube = ZHLN::PrefabFactory::CreateBox(
                *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(1.2, 1.0, 0.0), .createPhysics = false, .materialOverride = *redMatRes}
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(redCube));

            // 5. Render 15 frames of PBR lighting evaluation
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 15; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            // 6. Capture and compare the two halves relatively. The old gate
            // counted absolute pixels above fixed 8-bit floors (> 100 gold,
            // > 100 red), so any exposure or tonemapping change failed it.
            // The replacement asks two exposure-proof questions per half:
            // does the gold window read warm (R and G both well above B)
            // and does the red window read red-dominant -- each measured
            // against its own brightest pixel, and each required to beat
            // the OTHER half at its own hue, so shared background cancels.
            const ZHLN::Test::Image::RgbImage frame = ZHLN::Test::Headless::Capture(*engine, "headless_pbr_output.ppm");
            if (!frame.Valid()) {
                return std::unexpected(PBRTestError::RenderOutputBlank);
            }

            // Boxes at x = -1.2/+1.2, z = 0 under a 60deg camera at z = 4:
            // the left box spans roughly x in [0.20, 0.36], the right in
            // [0.64, 0.80], both vertically centered. The windows frame
            // each box with margin; the sky they also contain is shared,
            // so the cross-half comparison cancels it.
            const ZHLN::Test::Image::NormalizedRect goldWindow {.x0 = 0.10, .y0 = 0.30, .x1 = 0.45, .y1 = 0.70};
            const ZHLN::Test::Image::NormalizedRect redWindow {.x0 = 0.55, .y0 = 0.30, .x1 = 0.90, .y1 = 0.70};

            const double goldWarm = ZHLN::Test::Image::YellowShare(frame, goldWindow);
            const double redWarm  = ZHLN::Test::Image::YellowShare(frame, redWindow);

            const auto goldStats = ZHLN::Test::Image::MeasureSubRegion(frame, goldWindow);
            const auto redStats  = ZHLN::Test::Image::MeasureSubRegion(frame, redWindow);
            // Red leads green by 43 luma here yet strict dominant-red share
            // reads 0.000: at this brightness no pixel clears the 1.6x
            // classifier even on a correctly red dielectric. So the red side
            // compares R-minus-G EXCESS cross-half instead of shares. Green
            // is the shared channel where red albedo's lead shows: gold's F0
            // crushes blue (gold B = 106 vs red B = 191), so any B-based
            // metric favours gold and R-G is the only honest axis. Linear
            // physics gives red ~4x the excess (albedo keeps ~95% R-over-G,
            // gold F0 ~23%); Neutral compresses the measured gap to ~2x
            // (42.9 vs 20.9), and the bounds sit between.
            const double goldExcess = goldStats.meanR - goldStats.meanG;
            const double redExcess  = redStats.meanR - redStats.meanG;
            ZHLN::Println(
                "    [INFO] PBR dielectric vs metallic: gold window warm={:.3f} R-G excess={:.1f} mean=({:.1f},{:.1f},{:.1f}); red window warm={:.3f} R-G excess={:.1f} mean=({:.1f},{:.1f},{:.1f}).",
                goldWarm, goldExcess, goldStats.meanR, goldStats.meanG, goldStats.meanB, redWarm, redExcess, redStats.meanR, redStats.meanG,
                redStats.meanB
            );

            const bool goldPresent = ZHLN::Test::ExpectGt(goldWarm, 0.01);
            const bool redPresent  = ZHLN::Test::ExpectGt(redExcess, 5.0);
            const bool goldWarmer  = ZHLN::Test::ExpectGt(goldWarm, redWarm * 1.3);
            const bool redRedder   = ZHLN::Test::ExpectGt(redExcess, goldExcess * 1.5) && ZHLN::Test::ExpectGt(redExcess, goldExcess + 10.0);

            if (!goldPresent || !redPresent || !goldWarmer || !redRedder) {
                return std::unexpected(PBRTestError::SpecularHighlightNotDetected);
            }

            ZHLN::Println(
                "    [PASS] PBR validated: gold warm share {:.3f} vs {:.3f}, red R-G excess {:.1f} vs {:.1f}.",
                goldWarm, redWarm, redExcess, goldExcess
            );
            return {};
        }

        // ====================================================================
        // 3. Roughness Microfacet Specular Broadening (pixel compactness)
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> pbr_roughness_distribution_broadening() {
            auto engine      = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(PBRTestError::EngineInitFailed);
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            for (ZHLN::Entity camEnt: reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>()) {
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
                reg.Patch<ZHLN::Components::ShadowSettingsComponent>(settingsEnts[0], [](auto& sh) {
                    sh.shadowWidth = 400.0f;
                    sh.sunSize     = 0.001f;
                });
            }

            const ZHLN::Entity sunEnt = reg.Create();
            reg.Add(
                sunEnt,
                ZHLN::Components::TransformComponent {
                    .position = JPH::Vec3(0.0f, 10.0f, 10.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({0.0f, 0.0f, 0.0f})
                },
                ZHLN::Components::LightComponent {
                    .type      = ZHLN::LightType::Sun,
                    .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                    .intensity = 220.0f,
                    // Exactly behind the camera: N=V=L on the +Z face so the
                    // GGX peak is on-screen. A few degrees of elevation puts
                    // r=0.05's lobe between texels (maxL≈15, warm=0).
                    .direction = JPH::Vec3(0.0f, 0.0f, 1.0f)
                }
            );

            const auto camComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
            if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }
            ZHLN::Camera& cam = camComp->camera;
            cam.position = JPH::Vec3(0.0f, 1.0f, 4.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;

            // Metals keep a visible highlight after the screenshot's ACES×0.015
            // mapping; a gray dielectric lands below L=80 and the warm-pixel
            // gate never fires. Same albedo, only roughness differs.
            auto smoothMatRes = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 1.0f, .roughness = 0.18f, .baseColor = {0.92f, 0.92f, 0.94f, 1.0f}});
            if (!smoothMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            auto roughMatRes = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 1.0f, .roughness = 0.85f, .baseColor = {0.92f, 0.92f, 0.94f, 1.0f}});
            if (!roughMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            const ZHLN::Entity smoothBox = ZHLN::PrefabFactory::CreateBox(
                *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(-1.15, 1.0, 0.0), .createPhysics = false, .materialOverride = *smoothMatRes}
            );

            const ZHLN::Entity roughBox = ZHLN::PrefabFactory::CreateBox(
                *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(1.15, 1.0, 0.0), .createPhysics = false, .materialOverride = *roughMatRes}
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
                float    maxL      = 0.0f;
                uint32_t warm      = 0; // L > 25  — lit surface after ACES×0.015
                uint32_t hot       = 0; // L > 140 — specular peak
                double   sumX      = 0.0;
                double   sumY      = 0.0;
                double   sumX2     = 0.0;
                double   sumY2     = 0.0;
                uint32_t highlight = 0;
            };

            // Ignore the sky strip; the cubes sit in the middle of the frame.
            const int y0 = height / 6;
            const int y1 = (height * 5) / 6;

            auto analyzeHalf = [&](int x0, int x1) -> HalfStats {
                HalfStats s;
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3u;
                        const float  L = luminance(pixels[i], pixels[i + 1], pixels[i + 2]);
                        s.maxL         = std::max(s.maxL, L);
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

            const bool bothLit = ZHLN::Test::ExpectGt(smooth.maxL, 20.0f) && ZHLN::Test::ExpectGt(rough.maxL, 20.0f) && ZHLN::Test::ExpectGt(rough.warm, 50u);
            // Low roughness concentrates energy (tighter / hotter highlight).
            const bool tighter = smoothSpread + 8.0 < roughSpread;
            const bool hotter  = (smooth.maxL + 4.0f >= rough.maxL) && (smoothPeak + 0.01 > roughPeak || smooth.hot + 4u >= rough.hot);
            const bool peaked  = smooth.maxL > 80.0f;
            ZHLN::Test::ExpectTrue(tighter || hotter || peaked);

            if (!bothLit || !(tighter || hotter || peaked)) {
                return std::unexpected(PBRTestError::SpecularHighlightNotDetected);
            }

            ZHLN::Println(
                "    [PASS] PBR roughness: smooth spread={:.1f} peak={:.3f} maxL={:.1f}; rough spread={:.1f} peak={:.3f} maxL={:.1f}.", smoothSpread,
                smoothPeak, smooth.maxL, roughSpread, roughPeak, rough.maxL
            );
            return {};
        }

        // ====================================================================
        // HDR IBL: a compact sun must not ring into the shadowed diffuse lobe
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> pbr_hdr_sun_does_not_tint_shadowed_dielectric() {
            auto engine = CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(PBRTestError::EngineInitFailed);
            }
            auto& reg = engine->GetRegistry();
            auto& rc = engine->GetRenderContext();
            ZHLN::Test::Headless::DisableTAA(*engine);
            const ZHLN::Entity settings = reg.SingletonEntity<ZHLN::Components::GlobalSettingsTagComponent>();
            if (settings == ZHLN::Entity::Null()) return std::unexpected(PBRTestError::EngineInitFailed);
            for (const ZHLN::Entity camera: reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>()) {
            }
            reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings, [](auto& pp) {
                pp.giMode = 0;
                pp.fullBright = 0;
                pp.ambientExposure = 1.0f;
                pp.exposure = 1.0f;
                pp.tonemapper = 3;
                pp.bloomStrength = 0.0f;
                pp.glowIntensity = 0.0f;
                pp.vignetteIntensity = 0.0f;
                pp.enableSSR = 0;
                pp.enableRTR = 0;
            });
            auto material = rc.CreateMaterial(ZHLN::MaterialDesc {
                .metallic = 0.0f, .roughness = 1.0f, .baseColor = {0.75f, 0.75f, 0.75f, 1.0f}
            });
            if (!material) return std::unexpected(PBRTestError::MaterialCreationFailed);
            const auto camComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
            if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }
            ZHLN::Camera& cam = camComp->camera;
            cam.position = JPH::Vec3(0.0f, 1.0f, 4.0f);
            cam.yaw = -90.0f;
            cam.pitch = 0.0f;
            ZHLN::PrefabFactory::CreateBox(
                *engine, JPH::Vec3(1.5f, 1.5f, 0.2f),
                ZHLN::PrefabFactory::SpawnParams {
                    .position = JPH::RVec3(0.0, 1.0, 0.0), .createPhysics = false, .materialOverride = *material
                }
            );

            // Flat HDR vs. the same sky with a single-pixel point-like
            // hotspot at N dot L ~= -0.53 on the +Z face. The 9-coefficient SH
            // projection alone makes all three channels *negative* there, so
            // the old renderer clamped this otherwise lit surface to black.
            // Exact cosine irradiance from the hotspot is zero at this face.
            constexpr uint32_t w = 128, h = 64;
            ZHLN::EnvironmentImage flat {.width = w, .height = h};
            flat.rgba.resize(static_cast<size_t>(w) * h * 4u);
            for (size_t i = 0; i < flat.rgba.size(); i += 4u) {
                flat.rgba[i + 0] = 0.20f;
                flat.rgba[i + 1] = 0.25f;
                flat.rgba[i + 2] = 0.30f;
                flat.rgba[i + 3] = 1.0f;
            }
            ZHLN::EnvironmentImage hotspot = flat;
            const size_t pixel = (static_cast<size_t>(h / 2) * w + 52u) * 4u;
            hotspot.rgba[pixel + 0] = 50000.0f;
            hotspot.rgba[pixel + 1] = 38000.0f;
            hotspot.rgba[pixel + 2] = 12000.0f;
            ZHLN::AssetCooking::PrepareEnvironmentImage(hotspot);
            if (!hotspot.sun || hotspot.lightingRgba.empty()) return std::unexpected(PBRTestError::EnvironmentBakeFailed);
            const auto expectedSun = *hotspot.sun;
            auto& assets = engine->GetAssetManager();
            if (!assets.CacheEnvironmentImage("pbr_flat_hdr", std::move(flat)) ||
                !assets.CacheEnvironmentImage("pbr_compact_hdr", std::move(hotspot))) {
                return std::unexpected(PBRTestError::EnvironmentBakeFailed);
            }
            ZHLN::Components::EnvironmentMapComponent environment;
            environment.source.assign("pbr_flat_hdr");
            reg.Add(settings, std::move(environment));
            ZHLN::Test::Headless::TickFrames(*engine, 5, 1.0f / 60.0f);
            const auto baseline = ZHLN::Test::Headless::Capture(*engine, "headless_pbr_hdr_flat.ppm");
            reg.Patch<ZHLN::Components::EnvironmentMapComponent>(settings, [](auto& env) { env.source.assign("pbr_compact_hdr"); });
            ZHLN::Test::Headless::TickFrames(*engine, 5, 1.0f / 60.0f);
            const auto candidate = ZHLN::Test::Headless::Capture(*engine, "headless_pbr_hdr_compact.ppm");
            if (!baseline.Valid() || !candidate.Valid() || baseline.width != candidate.width || baseline.height != candidate.height) {
                return std::unexpected(PBRTestError::RenderOutputBlank);
            }

            // Compare the interior of the front face, away from the box edges,
            // AO and antialiasing. The hotspot is behind this face, so neither
            // its diffuse nor specular lobe can physically receive that light.
            // The flat environment and the material are otherwise identical.
            const auto middle = [](const auto& image) {
                std::array<double, 3> average {};
                for (int y = image.height / 2 - 4; y <= image.height / 2 + 4; ++y) {
                    for (int x = image.width / 2 - 4; x <= image.width / 2 + 4; ++x) {
                        const size_t i = (static_cast<size_t>(y) * image.width + x) * 3u;
                        for (int c = 0; c < 3; ++c) average[c] += image.rgb[i + c] / 81.0;
                    }
                }
                return average;
            };
            const auto before = middle(baseline), after = middle(candidate);
            if (before[0] < 20.0 || before[1] < 20.0 || before[2] < 20.0 ||
                std::abs(before[0] - after[0]) > 10.0 ||
                std::abs(before[1] - after[1]) > 10.0 ||
                std::abs(before[2] - after[2]) > 10.0) {
                return std::unexpected(PBRTestError::ShadowSideDiffuseTinted);
            }
            // The sun must be scene-owned now, not packed into ambient SH.
            const auto generated = reg.GetEntitiesWith<ZHLN::Components::EnvironmentSunTagComponent>();
            const auto light     = generated.size() == 1 ? reg.Get<ZHLN::Components::LightComponent>(generated[0]) : ZHLN::Optional<ZHLN::Components::LightComponent&> {};
            if (!light || light->type != ZHLN::LightType::Sun ||
                std::abs(light->direction.GetX() - expectedSun.direction[0]) > 0.01f ||
                std::abs(light->color.GetX() - 3.14159265f * expectedSun.irradiance[0]) > 0.02f) {
                return std::unexpected(PBRTestError::CookedSunPolicyFailed);
            }
            // A hidden cooked sky may be made visible without changing HDR
            // content. The original panorama must upload on this transition;
            // a sunless specular cube is NOT a substitute for the skybox.
            reg.Patch<ZHLN::Components::EnvironmentMapComponent>(settings, [](auto& env) { env.renderSkybox = 1; });
            ZHLN::Test::Headless::TickFrames(*engine, 3, 1.0f / 60.0f);
            const auto sky = ZHLN::Test::Headless::Capture(*engine, "headless_pbr_hdr_visible_sky.ppm");
            if (!sky.Valid() || sky.rgb[0] < 20u || sky.rgb[1] < 20u || sky.rgb[2] < 20u)
                return std::unexpected(PBRTestError::CookedSunPolicyFailed);
            reg.Patch<ZHLN::Components::EnvironmentMapComponent>(settings, [](auto& env) { env.renderSkybox = 0; });
            ZHLN::Test::Headless::TickFrames(*engine, 2, 1.0f / 60.0f);
            // Authored suns take priority even when created after the cooked
            // light. Keeping the conditioned IBL removes the HDR disk from
            // both specular and diffuse; it is NOT added as a second sun.
            const ZHLN::Entity authored = reg.Create(ZHLN::Components::LightComponent {
                .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f),
                .intensity = 0.0f, .direction = JPH::Vec3(0.0f, 0.0f, 1.0f)
            });
            ZHLN::Test::Headless::TickFrames(*engine, 3, 1.0f / 60.0f);
            if (!reg.GetEntitiesWith<ZHLN::Components::EnvironmentSunTagComponent>().empty())
                return std::unexpected(PBRTestError::CookedSunPolicyFailed);
            const auto overridden = ZHLN::Test::Headless::Capture(*engine, "headless_pbr_hdr_authored_override.ppm");
            if (!overridden.Valid() || overridden.width != baseline.width || overridden.height != baseline.height)
                return std::unexpected(PBRTestError::RenderOutputBlank);
            const auto authoredColor = middle(overridden);
            for (int c = 0; c < 3; ++c) {
                if (std::abs(authoredColor[c] - before[c]) > 10.0)
                    return std::unexpected(PBRTestError::CookedSunPolicyFailed);
            }
            reg.Destroy(authored); // data-only light, no external handles
            ZHLN::Test::Headless::TickFrames(*engine, 2, 1.0f / 60.0f);
            if (reg.GetEntitiesWith<ZHLN::Components::EnvironmentSunTagComponent>().size() != 1u)
                return std::unexpected(PBRTestError::CookedSunPolicyFailed);
            reg.Patch<ZHLN::Components::EnvironmentMapComponent>(settings, [](auto& env) { env.source.assign("pbr_flat_hdr"); });
            ZHLN::Test::Headless::TickFrames(*engine, 2, 1.0f / 60.0f);
            if (!reg.GetEntitiesWith<ZHLN::Components::EnvironmentSunTagComponent>().empty())
                return std::unexpected(PBRTestError::CookedSunPolicyFailed);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> pbr_cooked_hdr_sun_is_occluded_by_cascade_shadow() {
            auto engine = CreateTestEngine(640, 480);
            if (!engine) return std::unexpected(PBRTestError::EngineInitFailed);
            auto& reg = engine->GetRegistry();
            auto& rc = engine->GetRenderContext();
            ZHLN::Test::Headless::DisableTAA(*engine);
            const ZHLN::Entity settings = reg.SingletonEntity<ZHLN::Components::GlobalSettingsTagComponent>();
            if (settings == ZHLN::Entity::Null()) return std::unexpected(PBRTestError::EngineInitFailed);
            for (const ZHLN::Entity camera: reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>())
            reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings, [](auto& pp) {
                pp.giMode = 0;
                pp.ambientExposure = 1.0f;
                pp.exposure = 0.1f;
                pp.tonemapper = 3;
                pp.bloomStrength = 0.0f;
                pp.glowIntensity = 0.0f;
                pp.vignetteIntensity = 0.0f;
                pp.enableSSR = 0;
                pp.enableRTR = 0; // Exercise the CSM path, not ambient/RTR.
            });
            reg.Patch<ZHLN::Components::ShadowSettingsComponent>(settings, [](auto& shadows) {
                shadows.shadowWidth = 16.0f;
                shadows.shadowResolution = 2048;
                shadows.sunSize = 0.001f;
            });
            const auto camComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
            if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }
            ZHLN::Camera& cam = camComp->camera;
            cam.position = JPH::Vec3(0.0f, 1.0f, 4.0f);
            cam.yaw = -90.0f;
            cam.pitch = 0.0f;

            auto mat = rc.CreateMaterial(ZHLN::MaterialDesc {
                .metallic = 0.0f, .roughness = 1.0f, .baseColor = {0.75f, 0.75f, 0.75f, 1.0f}
            });
            if (!mat) return std::unexpected(PBRTestError::MaterialCreationFailed);
            ZHLN::PrefabFactory::CreateBox(*engine, JPH::Vec3(3.0f, 3.0f, 0.07f),
                {.position = JPH::RVec3(0.0, 1.0, 0.0), .createPhysics = false, .materialOverride = *mat});

            constexpr uint32_t w = 128, h = 64;
            ZHLN::EnvironmentImage env;
            env.width = w;
            env.height = h;
            env.rgba.resize(static_cast<size_t>(w) * h * 4u);
            for (size_t i = 0; i < env.rgba.size(); i += 4u) {
                env.rgba[i] = 0.05f;
                env.rgba[i + 1] = 0.06f;
                env.rgba[i + 2] = 0.07f;
                env.rgba[i + 3] = 1.0f;
            }
            // Equirect direction has positive X/Y/Z; the occluder can block
            // the receiver's center without covering it from the camera.
            const size_t hot = (22u * w + 81u) * 4u;
            env.rgba[hot] = 50000.0f;
            env.rgba[hot + 1] = 38000.0f;
            env.rgba[hot + 2] = 12000.0f;
            ZHLN::AssetCooking::PrepareEnvironmentImage(env);
            if (!env.sun) return std::unexpected(PBRTestError::EnvironmentBakeFailed);
            const auto sunDirection = env.sun->direction;
            if (!engine->GetAssetManager().CacheEnvironmentImage("pbr_shadowed_hdr", std::move(env)))
                return std::unexpected(PBRTestError::EnvironmentBakeFailed);
            ZHLN::Components::EnvironmentMapComponent environment;
            environment.source.assign("pbr_shadowed_hdr");
            reg.Add(settings, std::move(environment));
            ZHLN::Test::Headless::TickFrames(*engine, 6, 1.0f / 60.0f);
            const auto lit = ZHLN::Test::Headless::Capture(*engine, "headless_pbr_hdr_unoccluded.ppm");

            const double distance = 1.5;
            ZHLN::PrefabFactory::CreateBox(*engine, JPH::Vec3(0.32f, 0.32f, 0.32f),
                {.position = JPH::RVec3(sunDirection[0] * distance, 1.0 + sunDirection[1] * distance,
                                         sunDirection[2] * distance), .createPhysics = false, .materialOverride = *mat});
            ZHLN::Test::Headless::TickFrames(*engine, 6, 1.0f / 60.0f);
            const auto shadowed = ZHLN::Test::Headless::Capture(*engine, "headless_pbr_hdr_occluded.ppm");
            if (!lit.Valid() || !shadowed.Valid() || lit.width != shadowed.width || lit.height != shadowed.height)
                return std::unexpected(PBRTestError::RenderOutputBlank);
            const auto centerLuma = [](const auto& image) {
                double total = 0.0;
                for (int y = image.height / 2 - 5; y <= image.height / 2 + 5; ++y) {
                    for (int x = image.width / 2 - 5; x <= image.width / 2 + 5; ++x) {
                        const size_t i = (static_cast<size_t>(y) * image.width + x) * 3u;
                        total += (image.rgb[i] + image.rgb[i + 1] + image.rgb[i + 2]) / (3.0 * 121.0);
                    }
                }
                return total;
            };
            if (centerLuma(lit) < centerLuma(shadowed) + 18.0)
                return std::unexpected(PBRTestError::CookedSunPolicyFailed);
            return {};
        }

        // ====================================================================
        // 4. Energy Conservation & Fullbright Override
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> pbr_fullbright_mode_override() {
            auto engine      = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(PBRTestError::EngineInitFailed);
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // Set Fullbright ON: Disables direct lighting/shadows and outputs raw G-buffer albedo directly
            auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            if (!settingsEnts.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) { pp.fullBright = 1; });
            }

            // Create explicit bright green material
            auto greenMatRes = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.0f, 1.0f, 0.0f, 1.0f}});
            if (!greenMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            // Green flat box placed in front of camera
            const auto camComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
            if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
                return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
            }
            ZHLN::Camera& cam = camComp->camera;
            cam.position = JPH::Vec3(0.0f, 1.0f, 3.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;

            ZHLN::PrefabFactory::CreateBox(
                *engine, JPH::Vec3(2.0f, 2.0f, 0.1f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.0, 0.0), .createPhysics = false, .materialOverride = *greenMatRes}
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

            ZHLN::Test::ExpectGt(pureGreenPixels, 500u);
            return {};
        }
    };
};

// Exported for the GPU_Lighting group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunPBRSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<PBRTestSuite>();
}

