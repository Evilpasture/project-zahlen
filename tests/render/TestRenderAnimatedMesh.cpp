// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fstream>
#include <glTF/GLTFImporter.hpp>
#include <span>
#include <string>
#include <vector>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
namespace {

// Embed the binary GLB directly into the read-only data section of the test binary
// NOLINTBEGIN(bugprone-string-literal-with-embedded-nul, modernize-avoid-c-arrays, cppcoreguidelines-avoid-c-arrays)
constexpr uint8_t kUziGlbData[] = {
#embed "Uzi.glb"
};
// NOLINTEND(bugprone-string-literal-with-embedded-nul, modernize-avoid-c-arrays, cppcoreguidelines-avoid-c-arrays)
} // namespace
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
enum class AnimatedMeshTestError : uint8_t {
    PrefabLoadFailed ZHLN_ANNOTATION(ZHLN::Description<"CreativeWorksFactory failed to load or parse the in-memory GLB prefab.">{}) = 1,
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize headless Engine context for the animated mesh test.">{}),
    NoSkeletalMeshSpawned ZHLN_ANNOTATION(ZHLN::Description<"No entities with SkeletalMeshComponent were spawned.">{}),
    NoAnimatorFound ZHLN_ANNOTATION(ZHLN::Description<"Root entity does not contain an AnimatorComponent.">{}),
    SimulationTickFailed ZHLN_ANNOTATION(ZHLN::Description<"Engine::Tick failed during animated mesh playback.">{}),
    MeshDeformationExplosion ZHLN_ANNOTATION(ZHLN::Description<"Skinned mesh bounding radius exploded or contains NaN/Inf positions.">{}),
    RenderOutputBlank ZHLN_ANNOTATION(ZHLN::Description<"Rendered frame is completely black or failed to capture.">{}),
    MissingEmissiveGlow ZHLN_ANNOTATION(ZHLN::Description<"Automated pixel analysis detected zero emissive purple pixels.">{}),
};

struct RenderAnimatedMeshTestSuite {
    RenderAnimatedMeshTestSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~RenderAnimatedMeshTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    struct Tests {
        std::expected<void, ZHLN::Error> headless_automated_skinning_and_emission_verification() {
            // 1. Headless and pooled (runs hermetically without a window
            //    manager, and without its own Vulkan instance).
            const auto engine      = ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {
                     .appName               = "Headless Skinning Sanity Test",
                     .maxBodies             = 512,
                     .maxBodyPairs          = 1024,
                     .maxContactConstraints = 1024,
                     .tempAllocatorSize     = 16 * 1024 * 1024
            });
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(AnimatedMeshTestError::EngineInitFailed);
            }

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            // 2. Directional Sun Lighting
            const ZHLN::Entity sunEnt = reg.Create();
            reg.Add(
                sunEnt,
                ZHLN::Components::TransformComponent {
                    .position = JPH::Vec3(0.0f, 15.0f, 0.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({50.0f, -35.0f, 0.0f})
                },
                ZHLN::Components::LightComponent {.type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 0.98f, 0.94f), .intensity = 160.0f}
            );

            // 3. Instantiate Prefab Directly From Embedded In-Memory Byte Stream
            std::vector<ZHLN::Entity> spawnedParts(512);
            const uint32_t            count = ZHLN::GLTF::InstantiatePrefabFromMemory(
                *engine, kUziGlbData, "Uzi.glb",
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

            const ZHLN::Entity rootEntity = spawnedParts[0];
            const auto*        animComp   = reg.Get<ZHLN::Components::AnimatorComponent>(rootEntity);
            if (animComp == nullptr) {
                return std::unexpected(AnimatedMeshTestError::NoAnimatorFound);
            }

            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 1.0f, 2.5f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;
            cam.fov      = 45.0f;

            // 4. Automated Ticking (60 frames of animation playback & skinning compute)
            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 60; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                if (status != ZHLN::GameplayStatus::OK) {
                    return std::unexpected(AnimatedMeshTestError::SimulationTickFailed);
                }
            }

            // 5. Automated Skinning Sanity Check: Verify Bounding Radii
            for (const ZHLN::Entity e: spawnedParts) {
                if (const auto* mesh = reg.Get<ZHLN::Components::MeshComponent>(e)) {
                    if (std::isnan(mesh->cullRadius) || std::isinf(mesh->cullRadius) || mesh->cullRadius > 20.0f) {
                        return std::unexpected(AnimatedMeshTestError::MeshDeformationExplosion);
                    }
                }
            }

            // 6. Automated Pixel Readback & Color Histogram Analysis
            const std::string ppmPath    = "headless_skinning_output.ppm";
            const auto        captureRes = rc.CaptureScreenshotPPM(ppmPath);
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
            ppm.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

            uint32_t purpleEmissivePixels   = 0;
            uint32_t visibleCharacterPixels = 0;

            for (size_t i = 0; i < pixels.size(); i += 3) {
                const uint8_t r = pixels[i + 0];
                const uint8_t g = pixels[i + 1];
                const uint8_t b = pixels[i + 2];

                // Check for non-black pixels (rendered character geometry)
                if (r > 15 || g > 15 || b > 15) {
                    visibleCharacterPixels++;
                }

                // Detect purple/magenta emissive visor glow or purple hair (Blue & Red dominant over Green)
                if (b > 120 && r > 90 && b > g && r > (g - 20) && (b - g) > 20) {
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

            ZHLN::Println("    [PASS] Headless test verified embedded skinning geometry and {} purple emissive pixels.", purpleEmissivePixels);
            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunRenderAnimatedMeshSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<RenderAnimatedMeshTestSuite>();
}

