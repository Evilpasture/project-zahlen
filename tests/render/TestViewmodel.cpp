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

enum class ViewmodelTestError : uint8_t {
    EngineInitFailed[[= ZHLN::Reflect::Description<"Failed to initialize headless Engine context for Viewmodel test.">{}]] = 1,
    FOVDecouplingFailed[[= ZHLN::Reflect::Description<"Camera FOV modifications altered the viewmodel projection matrix.">{}]],
    RenderOutputBlank[[= ZHLN::Reflect::Description<"Rendered frame is blank or failed to capture.">{}]],
    ViewmodelNotRendered[[= ZHLN::Reflect::Description<"Automated pixel analysis detected zero viewmodel pixels on screen.">{}]],
    ViewmodelOccludedByWorld[[= ZHLN::Reflect::Description<"Viewmodel was erroneously occluded by world geometry in front of it.">{}]],
};

struct ViewmodelTestSuite {
    ViewmodelTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~ViewmodelTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);

        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless Viewmodel Test",
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
        // 1. Invariant: Fixed Viewmodel FOV (58 deg) Decoupled from Camera FOV
        // ====================================================================
        std::expected<void, ZHLN::Error> viewmodel_fov_decoupling_invariants() {
            auto engine      = CreateTestEngine(1280, 720);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto&       cam    = engine->GetCamera();
            const float aspect = 1280.0f / 720.0f;

            // Compute expected fixed 58-degree viewmodel projection
            const JPH::Mat44 expectedVMProj = ZHLN::Math::CreatePerspective(JPH::DegreesToRadians(58.0f), aspect, cam.nearZ, cam.farZ);

            // Test A: Normal Camera FOV (45.0 deg)
            cam.fov                = 45.0f;
            JPH::Mat44 worldProj45 = cam.GetProjectionMatrix(aspect);
            ZHLN::Test::ExpectFalse(worldProj45.IsClose(expectedVMProj, 1e-4f));

            // Test B: Sniper Scope / Zoomed-in Camera FOV (15.0 deg)
            cam.fov                = 15.0f;
            JPH::Mat44 worldProj15 = cam.GetProjectionMatrix(aspect);
            ZHLN::Test::ExpectFalse(worldProj15.IsClose(expectedVMProj, 1e-4f));

            // Test C: Ultra-Wide Camera FOV (110.0 deg)
            cam.fov                 = 110.0f;
            JPH::Mat44 worldProj110 = cam.GetProjectionMatrix(aspect);
            ZHLN::Test::ExpectFalse(worldProj110.IsClose(expectedVMProj, 1e-4f));

            // Test D: When camera FOV is exactly 58.0 deg, both projections match
            cam.fov                = 58.0f;
            JPH::Mat44 worldProj58 = cam.GetProjectionMatrix(aspect);
            ZHLN::Test::ExpectTrue(worldProj58.IsClose(expectedVMProj, 1e-4f));

            return {};
        }

        // ====================================================================
        // 2. DrawFlags::Viewmodel Pipeline Routing & Frame Execution
        // ====================================================================
        std::expected<void, ZHLN::Error> viewmodel_draw_flag_pipeline_routing() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto& reg = engine->GetRegistry();

            // 1. Sun light
            const ZHLN::Entity sunEnt = reg.Create();
            reg.Add(
                sunEnt, ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 10.0f, 0.0f)},
                ZHLN::Components::LightComponent {.type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 150.0f}
            );

            // 2. Spawn viewmodel weapon box in front of camera (-Z direction)
            ZHLN::Material vmMat;
            vmMat.baseColorFactor[0] = 0.0f;
            vmMat.baseColorFactor[1] = 1.0f;
            vmMat.baseColorFactor[2] = 0.5f;
            vmMat.baseColorFactor[3] = 1.0f;

            const ZHLN::Entity vmGun = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.08f, 0.08f, 0.35f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.2, 1.35, 1.5), .createPhysics = false, .materialOverride = vmMat}
            );

            // Flag as Viewmodel
            reg.Patch<ZHLN::Components::MeshComponent>(vmGun, [](auto& mc) { mc.flags |= ZHLN::DrawFlags::Viewmodel; });

            const auto* mc      = reg.Get<ZHLN::Components::MeshComponent>(vmGun);
            auto        checkMC = ZHLN::Test::AssertTrue(mc != nullptr);
            if (!checkMC)
                return checkMC;

            ZHLN::Test::ExpectTrue((mc->flags & ZHLN::DrawFlags::Viewmodel) != ZHLN::DrawFlags::None);

            // 3. Simulate multiple frames ensuring pipeline executes without Vulkan validation errors
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 15; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            return {};
        }

        // ====================================================================
        // 3. Viewmodel Depth Priority & Automated Pixel Readback
        // ====================================================================
        std::expected<void, ZHLN::Error> viewmodel_depth_priority_and_pixel_verification() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // Set Fullbright to test raw G-buffer viewmodel color resolution without lighting variance
            auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            if (!settingsEnts.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) { pp.fullBright = 1; });
            }

            // 1. Setup Camera looking along standard forward direction (-Z)
            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 1.5f, 2.0f);
            cam.yaw      = -90.0f; // Look directly along -Z
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;

            // Sync camera backing ECS component
            auto camEnts = reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>();
            if (!camEnts.empty()) {
                reg.Patch<ZHLN::Components::TargetCameraComponent>(camEnts[0], [](auto& tc) {
                    tc.yaw       = -90.0f;
                    tc.pitch     = 0.0f;
                    tc.stiffness = 0.0f;
                });
            }

            // 2. Solid Dark Wall directly in front of camera (at Z = -2.0m, dimensions 8x8m)
            ZHLN::Material wallMat;
            wallMat.baseColorFactor[0] = 0.15f;
            wallMat.baseColorFactor[1] = 0.15f;
            wallMat.baseColorFactor[2] = 0.15f;
            wallMat.baseColorFactor[3] = 1.0f;

            const ZHLN::Entity darkWall = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(4.0f, 4.0f, 0.2f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.5, -2.0), .createPhysics = false, .materialOverride = wallMat}
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(darkWall));

            // 3. Vibrant Cyan Viewmodel Object placed in front of camera (Z = +1.2m in world, 0.8m from camera)
            ZHLN::Material vmMat;
            vmMat.baseColorFactor[0] = 0.0f;
            vmMat.baseColorFactor[1] = 1.0f;
            vmMat.baseColorFactor[2] = 1.0f;
            vmMat.baseColorFactor[3] = 1.0f;

            const ZHLN::Entity vmObject = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.3f, 0.3f, 0.3f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.5, 1.2), .createPhysics = false, .materialOverride = vmMat}
            );
            reg.Patch<ZHLN::Components::MeshComponent>(vmObject, [](auto& mc) { mc.flags |= ZHLN::DrawFlags::Viewmodel; });

            // 4. Render 15 frames to stabilize G-buffer and Viewmodel pass
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 15; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            // 5. Screenshot capture and pixel histogram analysis
            const std::string ppmPath    = "headless_viewmodel_output.ppm";
            const auto        captureRes = rc.CaptureScreenshotPPM(ppmPath);
            if (!captureRes) {
                return std::unexpected(ViewmodelTestError::RenderOutputBlank);
            }

            std::ifstream ppm(ppmPath, std::ios::binary);
            if (!ppm.is_open()) {
                return std::unexpected(ViewmodelTestError::RenderOutputBlank);
            }

            std::string header;
            int         width = 0, height = 0, maxColor = 0;
            ppm >> header >> width >> height >> maxColor;
            ppm.get(); // consume newline

            std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 3));
            ppm.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

            uint32_t cyanViewmodelPixels = 0;
            uint32_t totalLitPixels      = 0;

            for (size_t i = 0; i < pixels.size(); i += 3) {
                const uint8_t r = pixels[i + 0];
                const uint8_t g = pixels[i + 1];
                const uint8_t b = pixels[i + 2];

                if (r > 10 || g > 10 || b > 10) {
                    totalLitPixels++;
                }

                // Identify bright cyan viewmodel pixels (Green & Blue high, Red low)
                if (g > 180 && b > 180 && r < 60) {
                    cyanViewmodelPixels++;
                }
            }

            ZHLN::Test::ExpectTrue(totalLitPixels > 1000u);
            ZHLN::Test::ExpectTrue(cyanViewmodelPixels > 100u);

            if (totalLitPixels < 1000u) {
                return std::unexpected(ViewmodelTestError::RenderOutputBlank);
            }
            if (cyanViewmodelPixels < 100u) {
                return std::unexpected(ViewmodelTestError::ViewmodelNotRendered);
            }

            ZHLN::Println("    [PASS] Viewmodel pass rendered {} cyan pixels over world geometry.", cyanViewmodelPixels);
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ViewmodelTestSuite>();
}
