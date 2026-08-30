// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cmath>
#include <cstdint>
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

enum class CameraLookAtError : uint8_t {
    EngineInitFailed[[= ZHLN::Description<"Failed to initialize headless Engine context for camera look-at test.">{}]] = 1,
    RenderOutputBlank[[= ZHLN::Description<"Rendered frame is blank or failed to capture.">{}]],
    TargetNotCentered[[= ZHLN::Description<"Target-colored pixels are not centered; camera is not looking at the target.">{}]],
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
        return std::unexpected(CameraLookAtError::RenderOutputBlank);
    }

    PpmImage    image;
    std::string header;
    int         maxColor = 0;
    ppm >> header >> image.width >> image.height >> maxColor;
    ppm.get();
    if (header != "P6" || image.width <= 0 || image.height <= 0) {
        return std::unexpected(CameraLookAtError::RenderOutputBlank);
    }

    image.pixels.resize(static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 3u);
    ppm.read(reinterpret_cast<char*>(image.pixels.data()), static_cast<std::streamsize>(image.pixels.size()));
    if (ppm.gcount() != static_cast<std::streamsize>(image.pixels.size())) {
        return std::unexpected(CameraLookAtError::RenderOutputBlank);
    }
    return image;
}

void DisableJitterAndVignette(ZHLN::ECS::Registry& reg) {
    for (ZHLN::Entity camEnt: reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>()) {
        reg.Remove<ZHLN::Components::FreeCamTagComponent>(camEnt);
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
}

} // namespace

struct CameraLookAtTestSuite {
    CameraLookAtTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~CameraLookAtTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // Off-axis red target + green decoy at the default look-at. A working
        // TargetCamera must put the red centroid near the image center and keep
        // the decoy off-screen. The default free-cam (origin look-at) does the opposite.
        std::expected<void, ZHLN::Error> target_camera_centers_colored_subject() {
            ZHLN::DefaultPreset::SetDisabled(true);

            const ZHLN::EngineConfig cfg {
                .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
                .render  = {
                    .appName        = "Headless Camera LookAt",
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
                return std::unexpected(CameraLookAtError::EngineInitFailed);
            }

            auto engine = std::move(engineRes.value());
            engine->InitializeDefaultScene();

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();
            DisableJitterAndVignette(reg);

            auto redMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.0f, 0.0f, 1.0f}}
            );
            auto greenMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.0f, 1.0f, 0.0f, 1.0f}}
            );
            if (!redMatRes || !greenMatRes) {
                return std::unexpected(CameraLookAtError::EngineInitFailed);
            }

            // Default camera looks toward the origin. Put the subject well off that axis.
            const ZHLN::Entity target = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(4.0, 1.0, 0.0), .createPhysics = false, .materialOverride = *redMatRes}
            );
            const ZHLN::Entity decoy = ZHLN::CreativeWorksFactory::CreateBox(
                *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.0, 0.0), .createPhysics = false, .materialOverride = *greenMatRes}
            );
            ZHLN::Test::ExpectTrue(reg.IsAlive(target));
            ZHLN::Test::ExpectTrue(reg.IsAlive(decoy));

            auto cameras  = reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>();
            auto checkCam = ZHLN::Test::AssertTrue(!cameras.empty());
            if (!checkCam) {
                return checkCam;
            }

            reg.Patch<ZHLN::Components::TargetCameraComponent>(cameras[0], [&](auto& tc) {
                tc.target              = target;
                tc.distance            = 5.0f;
                tc.targetDistance      = 5.0f;
                tc.yaw                 = -90.0f;
                tc.pitch               = 0.0f;
                tc.targetOffset        = JPH::Vec3::sZero();
                tc.stiffness           = 0.0f;
                tc.fov                 = 45.0f;
                tc.targetFov           = 45.0f;
                tc.vignetteIntensity   = 0.0f;
                tc.hasInitSmoothTarget = 0;
            });

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 8; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            const std::string ppmPath    = "headless_camera_lookat.ppm";
            const auto        captureRes = rc.CaptureScreenshotPPM(ppmPath);
            if (!captureRes) {
                return std::unexpected(CameraLookAtError::RenderOutputBlank);
            }

            auto imageRes = LoadPPM(ppmPath);
            if (!imageRes) {
                return std::unexpected(imageRes.error());
            }
            const PpmImage& image = *imageRes;

            uint64_t redCount = 0, greenCount = 0;
            double   redSumX = 0.0, redSumY = 0.0;
            for (int y = 0; y < image.height; ++y) {
                for (int x = 0; x < image.width; ++x) {
                    const size_t  i = (static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * 3u;
                    const uint8_t r = image.pixels[i + 0];
                    const uint8_t g = image.pixels[i + 1];
                    const uint8_t b = image.pixels[i + 2];
                    if (r > 160 && g < 50 && b < 50) {
                        redCount++;
                        redSumX += static_cast<double>(x);
                        redSumY += static_cast<double>(y);
                    } else if (g > 160 && r < 50 && b < 50) {
                        greenCount++;
                    }
                }
            }

            ZHLN::Test::ExpectTrue(redCount > 200u);
            if (redCount < 200u) {
                return std::unexpected(CameraLookAtError::TargetNotCentered);
            }

            const double cx = redSumX / static_cast<double>(redCount);
            const double cy = redSumY / static_cast<double>(redCount);
            const double nx = cx / static_cast<double>(image.width);
            const double ny = cy / static_cast<double>(image.height);

            // Aimed camera puts the subject on-axis. Default free-cam leaves it near the right edge.
            ZHLN::Test::ExpectTrue(std::abs(nx - 0.5) < 0.18);
            ZHLN::Test::ExpectTrue(std::abs(ny - 0.5) < 0.22);
            ZHLN::Test::ExpectTrue(greenCount < redCount / 4u);

            if (std::abs(nx - 0.5) >= 0.18 || std::abs(ny - 0.5) >= 0.22 || greenCount >= redCount / 4u) {
                return std::unexpected(CameraLookAtError::TargetNotCentered);
            }

            ZHLN::Println("    [PASS] Target camera: {} red px centroid=({:.2f},{:.2f}), {} decoy green px.", redCount, nx, ny, greenCount);
            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunCameraLookAtSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<CameraLookAtTestSuite>();
}

