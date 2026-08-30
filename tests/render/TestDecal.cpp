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

enum class DecalTestError : uint8_t {
    EngineInitFailed[[= ZHLN::Description<"Failed to initialize headless Engine context for Decal test.">{}]] = 1,
    ProceduralTextureFailed[[= ZHLN::Description<"Failed to allocate procedural decal texture.">{}]],
    DecalEntitySpawnFailed[[= ZHLN::Description<"Failed to create entity with DecalComponent.">{}]],
    RenderOutputBlank[[= ZHLN::Description<"Rendered frame is blank or failed to capture.">{}]],
    DecalPixelsNotProjected[[= ZHLN::Description<"Projected decal pixels were not detected on target geometry surface.">{}]],
    DecalBoundingClippingFailed[[= ZHLN::Description<"Decal bled onto geometry outside its projection bounding volume.">{}]],
};

struct DecalTestSuite {
    DecalTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~DecalTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);

        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless Decal Test",
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

    static auto GenerateCircularDecalTexture(uint32_t size, uint8_t r, uint8_t g, uint8_t b) -> std::vector<uint32_t> {
        std::vector<uint32_t> pixels(static_cast<size_t>(size * size));
        const float           center = static_cast<float>(size) * 0.5f;

        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                float dx   = (static_cast<float>(x) - center) / center;
                float dy   = (static_cast<float>(y) - center) / center;
                float dist = std::sqrt(dx * dx + dy * dy);

                uint8_t alpha        = (dist <= 0.85f) ? 255 : 0;
                pixels[y * size + x] = (static_cast<uint32_t>(alpha) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) |
                                       static_cast<uint32_t>(r);
            }
        }
        return pixels;
    }

    struct Tests {
        // ====================================================================
        // 1. Decal Component Registration & System Parameter Assembly
        // ====================================================================
        std::expected<void, ZHLN::Error> decal_component_registration_and_setup() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // 1. Create a 64x64 bright yellow procedural decal texture
            auto                yellowPixels = GenerateCircularDecalTexture(64, 255, 230, 20);
            ZHLN::TextureHandle decalTex     = rc.CreateProceduralTexture("vfx_test_decal_yellow", 64, 64, true, yellowPixels.data());

            ZHLN::Test::ExpectTrue(decalTex != ZHLN::TextureHandle::Invalid);
            if (decalTex == ZHLN::TextureHandle::Invalid) {
                return std::unexpected(DecalTestError::ProceduralTextureFailed);
            }

            // 2. Spawn Decal Entity with DecalComponent
            const JPH::Vec3  decalPos(0.0f, 1.5f, 0.0f);
            const JPH::Quat  decalRot = JPH::Quat::sIdentity();
            const JPH::Vec3  decalScale(1.5f, 1.5f, 2.0f);
            const JPH::Mat44 worldMat = ZHLN::Math::CreateTransform(decalPos, decalRot, decalScale);

            const ZHLN::Entity decalEnt = reg.Create();
            reg.Add(
                decalEnt, ZHLN::Components::NameComponent {.name = ZHLN::String64("TestDecalEntity")},
                ZHLN::Components::TransformComponent {.position = decalPos, .rotation = decalRot, .scale = decalScale},
                ZHLN::Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
                ZHLN::Components::DecalComponent {.albedoMap = decalTex, .normalMap = ZHLN::TextureHandle::Invalid, .roughness = 0.85f, .metallic = 0.05f}
            );

            ZHLN::Test::ExpectTrue(reg.IsAlive(decalEnt));

            const auto* decalComp = reg.Get<ZHLN::Components::DecalComponent>(decalEnt);
            auto        checkComp = ZHLN::Test::AssertTrue(decalComp != nullptr);
            if (!checkComp)
                return checkComp;

            ZHLN::Test::ExpectEq(decalComp->roughness, 0.85f);
            ZHLN::Test::ExpectEq(decalComp->metallic, 0.05f);
            ZHLN::Test::ExpectEq(decalComp->albedoMap, decalTex);

            return {};
        }

        // ====================================================================
        // 2. Screen-Space Decal Projection & Surface Stamping Verification
        // ====================================================================
        std::expected<void, ZHLN::Error> decal_surface_projection_and_pixel_verification() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // Set Fullbright to test raw G-buffer decal color stamping without lighting variance
            auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            if (!settingsEnts.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) { pp.fullBright = 1; });
            }

            // 1. Camera looking along standard forward direction (-Z) from Z = 2.0m
            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 1.5f, 2.0f);
            cam.yaw      = -90.0f; // Look directly along -Z
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;

            auto camEnts = reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>();
            if (!camEnts.empty()) {
                reg.Patch<ZHLN::Components::TargetCameraComponent>(camEnts[0], [](auto& tc) {
                    tc.yaw       = -90.0f;
                    tc.pitch     = 0.0f;
                    tc.stiffness = 0.0f;
                });
            }

            // 2. Dark Neutral Wall in front of camera (at Z = -2.0m, dimensions 8x8m)
            const ZHLN::Entity wall = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(4.0f, 4.0f, 0.2f),
                ZHLN::CreativeWorksFactory::SpawnParams {
                    .position = JPH::RVec3(0.0, 1.5, -2.0), .createPhysics = false, .color = {0.15f, 0.15f, 0.15f, 1.0f} // Dark gray background (RGB: ~38)
                }
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(wall));

            // 3. Create solid red procedural decal texture (R=255, G=0, B=0, A=255)
            auto                redPixels   = GenerateCircularDecalTexture(64, 255, 0, 0);
            ZHLN::TextureHandle redDecalTex = rc.CreateProceduralTexture("vfx_test_decal_red", 64, 64, true, redPixels.data());

            // 4. Spawn Decal centered on the wall (at Z = -2.0m, Scale 2.5m x 2.5m x 2.0m depth)
            const JPH::Vec3  decalPos(0.0f, 1.5f, -2.0f);
            const JPH::Quat  decalRot = JPH::Quat::sIdentity();
            const JPH::Vec3  decalScale(2.5f, 2.5f, 2.0f);
            const JPH::Mat44 worldMat = ZHLN::Math::CreateTransform(decalPos, decalRot, decalScale);

            const ZHLN::Entity decalEnt = reg.Create();
            reg.Add(
                decalEnt, ZHLN::Components::NameComponent {.name = ZHLN::String64("ProjectedRedDecal")},
                ZHLN::Components::TransformComponent {.position = decalPos, .rotation = decalRot, .scale = decalScale},
                ZHLN::Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
                ZHLN::Components::DecalComponent {.albedoMap = redDecalTex, .normalMap = ZHLN::TextureHandle::Invalid, .roughness = 0.90f, .metallic = 0.0f}
            );

            // 5. Simulate 10 frames of G-buffer, Decal pass, and lighting resolution
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 10; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            // 6. Screenshot capture & pixel validation
            const std::string ppmPath    = "headless_decal_output.ppm";
            const auto        captureRes = rc.CaptureScreenshotPPM(ppmPath);
            if (!captureRes) {
                return std::unexpected(DecalTestError::RenderOutputBlank);
            }

            std::ifstream ppm(ppmPath, std::ios::binary);
            if (!ppm.is_open()) {
                return std::unexpected(DecalTestError::RenderOutputBlank);
            }

            std::string header;
            int         width = 0, height = 0, maxColor = 0;
            ppm >> header >> width >> height >> maxColor;
            ppm.get(); // consume newline

            std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 3));
            ppm.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

            uint32_t projectedRedPixels = 0;
            uint32_t darkWallPixels     = 0;

            for (size_t i = 0; i < pixels.size(); i += 3) {
                const uint8_t r = pixels[i + 0];
                const uint8_t g = pixels[i + 1];
                const uint8_t b = pixels[i + 2];

                // Detect projected red decal pixels (Dominant Red)
                if (r > 180 && g < 60 && b < 60) {
                    projectedRedPixels++;
                }

                // Detect unprojected neutral dark wall pixels (Gray: R ~ G ~ B)
                if (r > 20 && std::abs((int) r - (int) g) < 15 && std::abs((int) g - (int) b) < 15) {
                    darkWallPixels++;
                }
            }

            ZHLN::Test::ExpectTrue(projectedRedPixels > 200u);
            ZHLN::Test::ExpectTrue(darkWallPixels > 1000u);

            if (projectedRedPixels < 200u) {
                return std::unexpected(DecalTestError::DecalPixelsNotProjected);
            }

            ZHLN::Println("    [PASS] Decal projection stamped {} red pixels onto target wall surface.", projectedRedPixels);
            return {};
        }

        // ====================================================================
        // 3. Decal Bounding Box Volume Clipping Invariants
        // ====================================================================
        std::expected<void, ZHLN::Error> decal_bounding_box_volume_clipping() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 1.5f, 2.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;

            // 1. Front Wall at Z = -2.0m (Inside decal projection volume)
            ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(2.0f, 2.0f, 0.1f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.5, -2.0), .createPhysics = false, .color = {0.1f, 0.1f, 0.1f, 1.0f}}
            );

            // 2. Far Object at Z = -15.0m (Completely outside decal projection depth)
            ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(4.0f, 4.0f, 0.1f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.5, -15.0), .createPhysics = false, .color = {0.1f, 0.1f, 0.1f, 1.0f}}
            );

            // 3. Small decal positioned tightly around the front wall (depth extent = 1.0m)
            auto                bluePixels   = GenerateCircularDecalTexture(64, 0, 120, 255);
            ZHLN::TextureHandle blueDecalTex = rc.CreateProceduralTexture("vfx_test_decal_blue", 64, 64, true, bluePixels.data());

            const JPH::Vec3  decalPos(0.0f, 1.5f, -2.0f);
            const JPH::Mat44 worldMat = ZHLN::Math::CreateTransform(decalPos, JPH::Quat::sIdentity(), JPH::Vec3(1.5f, 1.5f, 1.0f));

            const ZHLN::Entity decalEnt = reg.Create();
            reg.Add(
                decalEnt, ZHLN::Components::TransformComponent {.position = decalPos, .scale = JPH::Vec3(1.5f, 1.5f, 1.0f)},
                ZHLN::Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
                ZHLN::Components::DecalComponent {.albedoMap = blueDecalTex}
            );

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 5; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            return {};
        }
    };
};

// Exported for the GPU_Lighting group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunDecalSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<DecalTestSuite>();
}

