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
#include <algorithm>
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
    EngineInitFailed[[= ZHLN::Description<"Failed to initialize headless Engine context for parallel heap test.">{}]] = 1,
    MaterialCreationFailed[[= ZHLN::Description<"CreativeWorksFactory::CreateMaterial failed during parallel heap test.">{}]],
    RenderOutputBlank[[= ZHLN::Description<"Rendered frame is blank or failed to capture.">{}]],
    SecondaryHeapPathFailed[[= ZHLN::Description<"Not all material colors resolved through secondary-command-buffer heap draws.">{}]],
};

struct DescriptorHeapsParallelSuite {
    DescriptorHeapsParallelSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~DescriptorHeapsParallelSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> ZHLN::ScopedEngine {
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
            return {};
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
                std::array<float, 4>   baseColor {};
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
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 1.0f, .baseColor = materials[m].baseColor}
                );
                if (!matRes) {
                    return std::unexpected(DescriptorHeapsParallelTestError::MaterialCreationFailed);
                }
                gpuMaterials[m] = *matRes;
            }

            // 20 x 20 grid of cubes: 400 draw commands across 2 secondary
            // command buffers (kParallelChunkSize = 256 -> 2 chunks).
            constexpr uint32_t kGridCols  = 20;
            constexpr uint32_t kGridRows  = 20;
            constexpr float    spacing    = 0.55f;
            constexpr float    halfExtent = 0.25f;
            const float        firstCol   = -(static_cast<float>(kGridCols - 1) * spacing) * 0.5f;

            for (uint32_t row = 0; row < kGridRows; ++row) {
                for (uint32_t col = 0; col < kGridCols; ++col) {
                    const uint32_t  matIdx = (row * kGridCols + col) % 4;
                    const JPH::Vec3 pos(firstCol + static_cast<float>(col) * spacing, 0.6f + static_cast<float>(row) * spacing, 0.0f);
                    ZHLN::CreativeWorksFactory::CreateBox(
                        *engine, JPH::Vec3(halfExtent, halfExtent, halfExtent),
                        ZHLN::CreativeWorksFactory::SpawnParams {
                            .position = JPH::RVec3(pos.GetX(), pos.GetY(), pos.GetZ()), .createPhysics = false, .materialOverride = gpuMaterials[matIdx]
                        }
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

            // ====================================================================
            // Verification: fixed expected-RGB matching is too rigid for a lit
            // scene (uniform lighting/exposure scaling moves every color, and
            // tone mapping reshapes them). Instead, classify each pixel by HUE
            // over the central grid region (skipping the sky/floor): uniform
            // scaling and mild tone mapping preserve hue order, so each
            // material must light up its own hue bucket.
            // ====================================================================
            // Raw hue ranges (0..360). NOTE: red wraps the 0/360 seam and blue
            // sits in [200, 280] — no negative wrapping here (a previous
            // wrapped representation made blue pixels unclassifiable).
            struct Bucket {
                const char* name;
                uint32_t    count = 0;
            };
            const auto classifyHue = [](float h) -> int {
                if (h >= 336.0f || h < 24.0f)
                    return 0; // red
                if (h >= 36.0f && h < 84.0f)
                    return 1; // yellow
                if (h >= 96.0f && h < 150.0f)
                    return 2; // green
                if (h >= 200.0f && h < 280.0f)
                    return 3; // blue
                return -1;
            };
            std::array<Bucket, 4> buckets {{{"red"}, {"yellow"}, {"green"}, {"blue"}}};

            constexpr int kCropTop    = 85;
            constexpr int kCropBottom = 395;
            constexpr int kCropLeft   = 150;
            constexpr int kCropRight  = 490;

            uint64_t saturated = 0;
            uint64_t totalPx   = 0;
            for (int py = kCropTop; py < kCropBottom; ++py) {
                for (int px = kCropLeft; px < kCropRight; ++px) {
                    const size_t idx = (static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)) * 3;
                    const int    r   = pixels[idx + 0];
                    const int    g   = pixels[idx + 1];
                    const int    b   = pixels[idx + 2];
                    totalPx++;

                    const int mx = std::max({r, g, b});
                    const int mn = std::min({r, g, b});
                    if (mx - mn < 24) {
                        continue; // Near-gray (background/floor): skip.
                    }
                    saturated++;

                    float h = 0.0f;
                    if (mx == r) {
                        h = 60.0f * static_cast<float>(g - b) / static_cast<float>(mx - mn);
                    } else if (mx == g) {
                        h = 60.0f * (2.0f + static_cast<float>(b - r) / static_cast<float>(mx - mn));
                    } else {
                        h = 60.0f * (4.0f + static_cast<float>(r - g) / static_cast<float>(mx - mn));
                    }
                    if (h < 0.0f) {
                        h += 360.0f;
                    }

                    const int cls = classifyHue(h);
                    if (cls >= 0) {
                        buckets[static_cast<uint32_t>(cls)].count++;
                    }
                }
            }

            for (const auto& bucket: buckets) {
                ZHLN::Println("    [INFO] hue bucket '{}': {} pixels", bucket.name, bucket.count);
            }
            ZHLN::Println("    [INFO] crop region {} px, saturated {} px", totalPx, saturated);

            // --- Diagnostics: where did the unaccounted saturated pixels go? ---
            std::array<uint32_t, 12> hueHist {};
            for (int py = kCropTop; py < kCropBottom; ++py) {
                for (int px = kCropLeft; px < kCropRight; ++px) {
                    const size_t idx = (static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)) * 3;
                    const int    r   = pixels[idx + 0];
                    const int    g   = pixels[idx + 1];
                    const int    b   = pixels[idx + 2];
                    const int    mx  = std::max({r, g, b});
                    const int    mn  = std::min({r, g, b});
                    if (mx - mn < 24) {
                        continue;
                    }
                    float h = 0.0f;
                    if (mx == r) {
                        h = 60.0f * static_cast<float>(g - b) / static_cast<float>(mx - mn);
                    } else if (mx == g) {
                        h = 60.0f * (2.0f + static_cast<float>(b - r) / static_cast<float>(mx - mn));
                    } else {
                        h = 60.0f * (4.0f + static_cast<float>(r - g) / static_cast<float>(mx - mn));
                    }
                    if (h < 0.0f) {
                        h += 360.0f;
                    }
                    hueHist[static_cast<uint32_t>(h) / 30]++;
                }
            }
            ZHLN::Println("    [INFO] hue histogram (30-deg bins):");
            for (uint32_t bin = 0; bin < 12; ++bin) {
                ZHLN::Println("      [{:3}..{:3}): {:>6}", bin * 30, (bin + 1) * 30, hueHist[bin]);
            }

            // Sample the expected centers of one cube per material (grid is
            // deterministic; focal = 32 px/m at z = 0 for this camera).
            const auto sampleRgb = [&](int sx, int sy) {
                const size_t idx = (static_cast<size_t>(sy) * static_cast<size_t>(width) + static_cast<size_t>(sx)) * 3;
                ZHLN::Println("    [INFO] sample({}, {}) rgb = ({}, {}, {})", sx, sy, (int) pixels[idx + 0], (int) pixels[idx + 1], (int) pixels[idx + 2]);
            };
            sampleRgb(153, 237); // red    (col 0, row 10)
            sampleRgb(171, 237); // green  (col 1, row 10)
            sampleRgb(188, 237); // blue   (col 2, row 10)
            sampleRgb(206, 237); // yellow (col 3, row 10)

            // 100 cubes per material, ~250 px each in the cropped region: a
            // 2000-px floor per bucket is extremely conservative while still
            // failing if a material's draws never execute.
            for (const auto& bucket: buckets) {
                ZHLN::Test::ExpectTrue(bucket.count >= 2000);
                if (bucket.count < 2000) {
                    return std::unexpected(DescriptorHeapsParallelTestError::SecondaryHeapPathFailed);
                }
            }

            return {};
        }
    };
};

// Exported for the GPU_Pipeline group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunDescriptorHeapsParallelSuite() -> ZHLN::Test::TestStats {
    // Force the CPU-culling policy BEFORE the first frame records: MainPass1
    // then draws into parallel secondary command buffers that inherit the
    // primary's descriptor-heap bindings (Diag::DisableGpuCulling caches the
    // environment read on first use).
    #if defined(_WIN32)
    _putenv_s("ZHLN_NO_GPU_CULLING", "1");
    #else
    setenv("ZHLN_NO_GPU_CULLING", "1", 1);
    #endif
    return ZHLN::Test::RunSuite<DescriptorHeapsParallelSuite>();
}

