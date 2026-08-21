// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestDescriptorHeapsParallel.cpp
//
// Exercises the descriptor-heap path inside parallel-recorded SECONDARY
// command buffers. With ZHLN_NO_GPU_CULLING forced, MainPass1 records its
// geometry through ParallelDrawDispatch: the secondaries inherit the
// primary's heap bindings (VkCommandBufferInheritanceDescriptorHeapInfoEXT),
// re-push the per-frame device-address block via vkCmdPushDataEXT, and draw
// with heap pipelines. 400 cubes in 4 shared materials verify the pipeline.

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

enum class DescriptorHeapsParallelTestError : uint8_t {
    Success = 0,
    EngineInitFailed[[= ZHLN::Reflect::Description("Failed to initialize headless Engine context for parallel heap test.")]],
    MaterialCreationFailed[[= ZHLN::Reflect::Description("CreativeWorksFactory::CreateMaterial failed during parallel heap test.")]],
    RenderOutputBlank[[= ZHLN::Reflect::Description("Rendered frame is blank or failed to capture.")]],
    SecondaryHeapPathFailed[[= ZHLN::Reflect::Description("Not all material colors resolved through secondary-command-buffer heap draws.")]],
};

struct DescriptorHeapsParallelSuite {
    DescriptorHeapsParallelSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~DescriptorHeapsParallelSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);

        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 1024, .maxBodyPairs = 2048, .maxContactConstraints = 2048, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                .appName        = "Headless Parallel Descriptor Heap Test",
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
        // 400 cubes / 4 shared materials, forced down the CPU-culling path:
        // MainPass1 records its draws into parallel secondary command buffers
        // that inherit the primary's descriptor-heap bindings.
        // ====================================================================
        std::expected<void, ZHLN::Error> secondary_command_buffers_inherit_and_draw_from_heaps() {
            auto engine      = DescriptorHeapsParallelSuite::CreateTestEngine();
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine)
                return checkEngine;

            auto& reg = engine->GetRegistry();
            auto& rc  = engine->GetRenderContext();

            auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
            if (!settingsEnts.empty()) {
                reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) { pp.fullBright = 1; });
            }

            auto& cam    = engine->GetCamera();
            cam.position = JPH::Vec3(0.0f, 6.0f, 13.0f);
            cam.yaw      = -90.0f;
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;

            // 4 strongly separated material colors.
            struct TestMaterial {
                std::array<float, 4> baseColor {};
                std::array<uint8_t, 3> rgb {};
            };
            const std::array<TestMaterial, 4> materials {{
                {{1.0f, 0.1f, 0.1f, 1.0f}, {255, 40, 40}},
                {{0.1f, 1.0f, 0.15f, 1.0f}, {40, 230, 60}},
                {{0.15f, 0.2f, 1.0f, 1.0f}, {50, 80, 255}},
                {{1.0f, 0.9f, 0.1f, 1.0f}, {250, 230, 50}},
            }};

            std::array<ZHLN::Material, 4> gpuMaterials {};
            for (uint32_t m = 0; m < 4; ++m) {
                auto matRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 1.0f, .baseColor = materials[m].baseColor}
                );
                if (!matRes) {
                    return std::unexpected(DescriptorHeapsParallelTestError::MaterialCreationFailed);
                }
                gpuMaterials[m] = *matRes;
            }

            // 20 x 20 grid of cubes: 400 draw commands across 2 secondary
            // command buffers (kParallelChunkSize = 256 -> 2 chunks).
            constexpr uint32_t kGridCols   = 20;
            constexpr uint32_t kGridRows   = 20;
            constexpr float    spacing     = 0.55f;
            constexpr float    halfExtent  = 0.25f;
            const float        firstCol    = -(static_cast<float>(kGridCols - 1) * spacing) * 0.5f;

            for (uint32_t row = 0; row < kGridRows; ++row) {
                for (uint32_t col = 0; col < kGridCols; ++col) {
                    const uint32_t matIdx  = (row * kGridCols + col) % 4;
                    const JPH::Vec3 pos(
                        firstCol + static_cast<float>(col) * spacing, 0.6f + static_cast<float>(row) * spacing, 0.0f
                    );
                    ZHLN::CreativeWorksFactory::CreateBox(
                        *engine, JPH::Vec3(halfExtent, halfExtent, halfExtent),
                        ZHLN::CreativeWorksFactory::SpawnParams {
                            .position = JPH::RVec3(pos.GetX(), pos.GetY(), pos.GetZ()), .createPhysics = false, .materialOverride = gpuMaterials[matIdx]}
                    );
                }
            }

            constexpr float dt = 1.0f / 60.0f;
            for (uint32_t frame = 0; frame < 8; ++frame) {
                engine->ProcessEvents();
                const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
                ZHLN::Test::ExpectEq(status, ZHLN::GameplayStatus::OK);
            }

            const std::string ppmPath = "headless_dheap_parallel.ppm";
            if (!rc.CaptureScreenshotPPM(ppmPath)) {
                return std::unexpected(DescriptorHeapsParallelTestError::RenderOutputBlank);
            }

            std::ifstream ppm(ppmPath, std::ios::binary);
            if (!ppm.is_open()) {
                return std::unexpected(DescriptorHeapsParallelTestError::RenderOutputBlank);
            }

            std::string header;
            int         width = 0, height = 0, maxColor = 0;
            ppm >> header >> width >> height >> maxColor;
            ppm.get();

            std::vector<uint8_t> pixels(static_cast<size_t>(width * height * 3));
            ppm.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));

            std::array<uint32_t, 4> matched {};
            for (size_t px = 0; px < pixels.size(); px += 3) {
                int      bestIdx = -1;
                uint32_t bestDst = ~0U;
                for (uint32_t m = 0; m < 4; ++m) {
                    const int dr = static_cast<int>(pixels[px + 0]) - static_cast<int>(materials[m].rgb[0]);
                    const int dg = static_cast<int>(pixels[px + 1]) - static_cast<int>(materials[m].rgb[1]);
                    const int db = static_cast<int>(pixels[px + 2]) - static_cast<int>(materials[m].rgb[2]);
                    const uint32_t d = static_cast<uint32_t>(dr * dr + dg * dg + db * db);
                    if (d < bestDst) {
                        bestDst = d;
                        bestIdx = static_cast<int>(m);
                    }
                }
                if (bestIdx >= 0 && bestDst < 60 * 60) {
                    matched[static_cast<uint32_t>(bestIdx)]++;
                }
            }

            for (uint32_t m = 0; m < 4; ++m) {
                ZHLN::Println("    [INFO] Material {} resolved {} pixels through secondary heap draws.", m, matched[m]);
                ZHLN::Test::ExpectTrue(matched[m] >= 200);
                if (matched[m] < 200) {
                    return std::unexpected(DescriptorHeapsParallelTestError::SecondaryHeapPathFailed);
                }
            }

            return {};
        }
    };
};

int main() {
    // Force the CPU-culling policy BEFORE the first frame records: MainPass1
    // then draws into parallel secondary command buffers that inherit the
    // primary's descriptor-heap bindings (Diag::DisableGpuCulling caches the
    // environment read on first use).
#if defined(_WIN32)
    _putenv_s("ZHLN_NO_GPU_CULLING", "1");
#else
    setenv("ZHLN_NO_GPU_CULLING", "1", 1);
#endif
    return ZHLN::Test::Runner::Run<DescriptorHeapsParallelSuite>();
}
