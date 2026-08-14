// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cmath>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <vector>

enum class AnimatedMeshTestError : uint32_t {
    Success = 0,
    AssetNotFound[[= ZHLN::Reflect::Description("Could not find 'Uzi.glb' on disk.")]],
    PrefabLoadFailed[[= ZHLN::Reflect::Description("CreativeWorksFactory failed to load or parse the GLB prefab.")]],
    NoSkeletalMeshSpawned[[= ZHLN::Reflect::Description("No entities with SkeletalMeshComponent were spawned.")]],
    NoAnimatorFound[[= ZHLN::Reflect::Description("Root entity does not contain an AnimatorComponent.")]],
    SimulationTickFailed[[= ZHLN::Reflect::Description("Engine::Tick failed during animated mesh playback.")]],
    MeshDeformationExplosion[[= ZHLN::Reflect::Description("Skinned mesh bounding radius exploded or contains NaN/Inf positions.")]],
    RenderOutputBlank[[= ZHLN::Reflect::Description("Rendered frame is completely black or failed to capture.")]],
    MissingEmissiveGlow[[= ZHLN::Reflect::Description("Automated pixel analysis detected zero emissive purple pixels.")]],
};

struct RenderAnimatedMeshTestSuite {
    RenderAnimatedMeshTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, 131072);
    }

    ~RenderAnimatedMeshTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        std::expected<void, ZHLN::Error> headless_automated_skinning_and_emission_verification() {
            // 1. Resolve asset path
            while (!std::filesystem::exists("resources") && std::filesystem::current_path().has_parent_path()) {
                auto parent = std::filesystem::current_path().parent_path();
                if (parent == std::filesystem::current_path()) {
                    break;
                }
                std::filesystem::current_path(parent);
            }

            std::string assetPath;
            if (std::filesystem::exists("resources/assets/Uzi.glb")) {
                assetPath = "Uzi.glb";
            } else if (std::filesystem::exists("resources/assets/murderdrones/Uzi.glb")) {
                assetPath = "murderdrones/Uzi.glb";
            } else {
                return std::unexpected(AnimatedMeshTestError::AssetNotFound);
            }

            ZHLN::DefaultPreset::SetDisabled(true);

            // 2. Headless Configuration (Runs cleanly in CI/CD without X11/Wayland windows)
            const ZHLN::EngineConfig cfg {
                .physics = {.maxBodies = 512, .maxBodyPairs = 1024, .maxContactConstraints = 1024, .tempAllocatorSize = 16 * 1024 * 1024},
                .render  = {
                    .appName        = "Headless Skinning Sanity Test",
                    .width          = 640,
                    .height         = 480,
                    .vsync          = false,
                    .fullscreen     = false,
                    .validationMode = ZHLN::ValidationMode::On,
                    .headless       = true
                }
            };

            auto engineRes   = ZHLN::Engine::Create(cfg);
            auto checkEngine = ZHLN::Test::AssertTrue(engineRes.has_value());
            if (!checkEngine) {
                return checkEngine;
            }

            auto engine = std::move(engineRes.value());
            engine->InitializeDefaultScene();

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // 3. Directional Lighting
            ZHLN::Entity sunEnt = reg.Create();
            reg.Add(
                sunEnt,
                ZHLN::Components::TransformComponent {
                    .position = JPH::Vec3(0.0f, 15.0f, 0.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({50.0f, -35.0f, 0.0f})
                },
                ZHLN::Components::LightComponent {.type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 0.98f, 0.94f), .intensity = 160.0f}
            );

            // 4. Instantiate Prefab
            std::vector<ZHLN::Entity> spawnedParts(512);
            uint32_t                  count = ZHLN::CreativeWorksFactory::InstantiatePrefab(
                *engine, assetPath,
                {
                    .position        = JPH::RVec3(0.0f, 0.0f, 0.0f),
                    .createPhysics   = false,
                    .isStaticPhysics = true,
                    .isAnimated      = true,
                },
                spawnedParts.data(), 512
            );
            spawnedParts.resize(count);

            if (count == 0 || spawnedParts.empty()) {
                return std::unexpected(AnimatedMeshTestError::PrefabLoadFailed);
            }

            ZHLN::Entity rootEntity = spawnedParts[0];
            auto*        animComp   = reg.Get<ZHLN::Components::AnimatorComponent>(rootEntity);
            if (animComp == nullptr) {
                return std::unexpected(AnimatedMeshTestError::NoAnimatorFound);
            }

            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 1.0f, 2.5f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;
            cam.fov      = 45.0f;

            // 5. Automated Ticking (60 frames)
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 60; ++frame) {
                engine->ProcessEvents();
                auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                if (status != ZHLN::GameplayStatus::OK) {
                    return std::unexpected(AnimatedMeshTestError::SimulationTickFailed);
                }
            }

            // 6. Automated Skinning Sanity Check: Verify Bounding Radii
            for (ZHLN::Entity e: spawnedParts) {
                if (auto* mesh = reg.Get<ZHLN::Components::MeshComponent>(e)) {
                    if (std::isnan(mesh->cullRadius) || std::isinf(mesh->cullRadius) || mesh->cullRadius > 20.0f) {
                        return std::unexpected(AnimatedMeshTestError::MeshDeformationExplosion);
                    }
                }
            }

            // 7. Automated Pixel Readback & Color Histogram Analysis
            const std::string ppmPath    = "headless_skinning_output.ppm";
            auto              captureRes = rc.CaptureScreenshotPPM(ppmPath);
            if (!captureRes) {
                return std::unexpected(AnimatedMeshTestError::RenderOutputBlank);
            }

            std::ifstream ppm(ppmPath, std::ios::binary);
            if (!ppm.is_open()) {
                return std::unexpected(AnimatedMeshTestError::RenderOutputBlank);
            }

            std::string header;
            int         width    = 0;
            int         height   = 0;
            int         maxColor = 0;
            ppm >> header >> width >> height >> maxColor;
            ppm.get(); // consume newline

            std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 3));
            ppm.read(reinterpret_cast<char*>(pixels.data()), pixels.size());

            uint32_t purpleEmissivePixels   = 0;
            uint32_t visibleCharacterPixels = 0;

            for (size_t i = 0; i < pixels.size(); i += 3) {
                uint8_t r = pixels[i + 0];
                uint8_t g = pixels[i + 1];
                uint8_t b = pixels[i + 2];

                // Check for non-black pixels (rendered character)
                if (r > 15 || g > 15 || b > 15) {
                    visibleCharacterPixels++;
                }

                // Detect purple/magenta emissive visor glow (high R & B, lower G)
                if (r > 120 && b > 150 && g < 100) {
                    purpleEmissivePixels++;
                }
            }

            ZHLN::Test::ExpectTrue(visibleCharacterPixels > 1000u);
            ZHLN::Test::ExpectTrue(purpleEmissivePixels > 50u);

            if (visibleCharacterPixels < 1000u) {
                return std::unexpected(AnimatedMeshTestError::RenderOutputBlank);
            }
            if (purpleEmissivePixels < 50u) {
                return std::unexpected(AnimatedMeshTestError::MissingEmissiveGlow);
            }

            ZHLN::Println("    [PASS] Headless test verified skinning geometry and {} purple emissive pixels.", purpleEmissivePixels);
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<RenderAnimatedMeshTestSuite>();
}
