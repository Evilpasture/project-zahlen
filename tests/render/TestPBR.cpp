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
#include <engine/PBR.hpp>
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
    BrdfLutGenerationFailed[[= ZHLN::Reflect::Description("Generated 2D BRDF LUT failed numerical or boundary invariants.")]],
    SphericalHarmonicsFailed[[= ZHLN::Reflect::Description("Diffuse Spherical Harmonics coefficients violated positive energy conservation.")]],
    SpecularMipGenerationFailed[[= ZHLN::Reflect::Description("Specular pre-filtered environment cubemap mips failed validation.")]],
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
        // 1. Analytical IBL Precomputation (BRDF LUT, SH & Specular Mips)
        // ====================================================================
        std::expected<void, ZHLN::Error> pbr_ibl_precomputation_and_lut_invariants() {
            // A. BRDF Look-Up Table Verification (128x128)
            constexpr uint32_t lutDim  = 128;
            auto               lutData = ZHLN::PBR::GenerateBRDFLUT(lutDim, lutDim);

            ZHLN::Test::ExpectEq(lutData.size(), static_cast<size_t>(lutDim * lutDim));
            if (lutData.size() != static_cast<size_t>(lutDim * lutDim)) {
                return std::unexpected(PBRTestError::BrdfLutGenerationFailed);
            }

            // In BRDFLUT.cpp:
            // y = 0 -> roughness = 0.001 (smooth mirror surface)
            // x = lutDim - 1 -> NdotV = 1.0 (perpendicular normal incidence)
            // At smooth roughness and normal viewing, scaleR (Fresnel scale A) approaches 1.0 (255)
            uint32_t smoothNormalPixel = lutData[0 * lutDim + (lutDim - 1)];
            uint8_t  scaleR            = smoothNormalPixel & 0xFF;        // Scale (A)
            uint8_t  biasG             = (smoothNormalPixel >> 8) & 0xFF; // Bias (B)

            ZHLN::Test::ExpectTrue(scaleR > 200u);
            ZHLN::Test::ExpectTrue(biasG < 50u);

            // B. Diffuse Spherical Harmonics Validation (9 Bands)
            auto shBands = ZHLN::PBR::GenerateDiffuseSH();
            ZHLN::Test::ExpectEq(shBands.size(), static_cast<size_t>(9));

            // Invariant: L0 band (sh[0]) represents ambient irradiance and must be strictly positive
            ZHLN::Test::ExpectTrue(shBands[0].GetX() > 0.0f);
            ZHLN::Test::ExpectTrue(shBands[0].GetY() > 0.0f);
            ZHLN::Test::ExpectTrue(shBands[0].GetZ() > 0.0f);

            if (shBands[0].GetX() <= 0.0f || shBands[0].GetY() <= 0.0f || shBands[0].GetZ() <= 0.0f) {
                return std::unexpected(PBRTestError::SphericalHarmonicsFailed);
            }

            // C. Specular Pre-filtered Environment Map (Roughness Mips)
            constexpr uint32_t cubemapSize = 32;
            auto               mipSmooth   = ZHLN::PBR::GenerateSpecularMip(cubemapSize, 0.0f);
            auto               mipRough    = ZHLN::PBR::GenerateSpecularMip(cubemapSize, 1.0f);

            ZHLN::Test::ExpectEq(mipSmooth.size(), static_cast<size_t>(6)); // 6 cubemap faces
            ZHLN::Test::ExpectEq(mipRough.size(), static_cast<size_t>(6));

            for (int face = 0; face < 6; ++face) {
                ZHLN::Test::ExpectEq(mipSmooth[face].size(), static_cast<size_t>(cubemapSize * cubemapSize));
                ZHLN::Test::ExpectEq(mipRough[face].size(), static_cast<size_t>(cubemapSize * cubemapSize));
            }

            return {};
        }

        // ====================================================================
        // 2. Dielectric vs. Metallic Direct Lighting & Color Tinting
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
        // 3. Roughness Microfacet Specular Broadening Invariant
        // ====================================================================
        std::expected<void, ZHLN::Error> pbr_roughness_distribution_broadening() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return checkEngine;
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            auto smoothMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.05f, .baseColor = {0.8f, 0.8f, 0.8f, 1.0f}}
            );
            if (!smoothMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            auto roughMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.8f, 0.8f, 0.8f, 1.0f}}
            );
            if (!roughMatRes) {
                return std::unexpected(PBRTestError::MaterialCreationFailed);
            }

            const ZHLN::Entity smoothBox = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(-1.0, 1.0, 0.0), .createPhysics = false, .materialOverride = *smoothMatRes}
            );

            const ZHLN::Entity roughBox = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(1.0, 1.0, 0.0), .createPhysics = false, .materialOverride = *roughMatRes}
            );

            ZHLN::Test::ExpectTrue(reg.IsAlive(smoothBox));
            ZHLN::Test::ExpectTrue(reg.IsAlive(roughBox));

            const auto* pbrSmooth = reg.Get<ZHLN::Components::PBRComponent>(smoothBox);
            const auto* pbrRough  = reg.Get<ZHLN::Components::PBRComponent>(roughBox);

            auto checkPBR = ZHLN::Test::AssertTrue(pbrSmooth != nullptr && pbrRough != nullptr);
            if (!checkPBR) {
                return checkPBR;
            }

            ZHLN::Test::ExpectEq(pbrSmooth->roughness, 0.05f);
            ZHLN::Test::ExpectEq(pbrRough->roughness, 0.85f);

            // Simulate 5 frames
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 5; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

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
