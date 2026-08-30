// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestLightingRayTraced.cpp
//
// Verification for the lighting + raytracing pipeline:
//
//   1. Flicker: a fully static, fully lit scene must produce temporally
//      stable frames. A light popping in/out of a cluster, a reflection
//      cache missing for one frame or a TLAS rebuild hitch shows up here.
//   2. Accidental light culling: a single point light glides across the
//      screen, crossing cluster-cell and z-slice boundaries, while its red
//      signature on the ground/box must never vanish or collapse. After the
//      sweep the light is frozen in place and 8 consecutive frames must be
//      identical (no oscillation, no step).
//   3. Bisect for the above: the same static light on a FRESH engine with no
//      motion history pins whether an observed instability is carried over
//      from the sweep (stale double-buffered state) or is a pure per-frame
//      race. Both scenarios are HARD gates.
//   4. Ray-traced shadows: an occluder between the sun and the ground must
//      carve a real, stable shadow (not a full-scene blackout, not nothing),
//      and removing it must restore a near-uniformly lit floor.
//   5. Reflections: a polished plane must mirror a bright emissive object
//      with the engine's DEFAULT reflection path (SSR), stable frame to frame.
//   6. Multi-Light Clustered Illumination & Chromatic Blending: 64 point lights
//      in 4 color quadrants; tests cluster accumulation & additive mixing in .ppm.
//   7. Multi-Emissive Sources & Reflection Mapping: Multiple distinct emissive
//      objects mirrored on a polished plane with spatial .ppm column analysis.
//   8. Dense Multi-Light & Emissive Cross-Interaction: 32 dynamic point lights
//      interacting with an emissive monolith over a diverse PBR material grid.

#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
// stb_image_write's implementation comes from tests/helpers/ImageWriteImpl.cpp,
// compiled once into the group binary. Do not define ZHLN_TEST_IMAGE_WRITE_IMPL
// here -- a second definition in the same link is a duplicate-symbol error.
#include "helpers/ImageTesting.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>


// ============================================================================
// Test Error Types
// ============================================================================

enum class LightingRTTestError : uint8_t {
    EngineInitFailed[[= ZHLN::Description<"Failed to initialize headless Engine context for the lighting/raytracing test."> {}]] = 1,
    RenderOutputBlank[[= ZHLN::Description<"Rendered frame is blank or could not be captured."> {}]],
    TemporalFlickerDetected[[= ZHLN::Description<"A static fully-lit scene changed more frame-to-frame than the engine's own noise floor."> {}]],
    LightCullingPopDetected[[= ZHLN::Description<"A point light inside the frustum/range lost its lighting contribution for a frame (cluster culling)."> {}]],
    RayTracedShadowFailed[[= ZHLN::Description<"The ray-traced sun shadow did not appear, disappeared, or took out the whole frame."> {}]],
    ReflectionMissing[[= ZHLN::Description<"The polished surface shows no reflection of the emissive object (RTR/SSR fell back to IBL)."> {}]],
    ReflectionArtifacts[[= ZHLN::Description<"The reflected region contains blowout, ray-debris speckles, or flicker."> {}]],
    MultiLightClusteringFailed[[= ZHLN::Description<"Multi-light clustered accumulation or chromatic blending mismatch detected."> {}]],
    MultiEmissiveReflectionFailed[[= ZHLN::Description<"Multi-emissive source reflection analysis failed: missing spatial mirror correspondence or color "
                                                       "fidelity."> {}]],
    DenseCrossInteractionFailed[[= ZHLN::Description<"Dense multi-light & emissive interaction produced blowout, NaN/Inf, or lighting failure."> {}]],
    DeviceLostDuringTest[[= ZHLN::Description<"The Vulkan device was lost repeatedly during the scenario; the engine hot-rebuild recovered, but the GPU was "
                                              "not stable."> {}]],
    ValidationErrorsRaised[[= ZHLN::Description<"The validation layer reported errors while rendering the lighting/raytracing frames."> {}]],
};

// ============================================================================
// Shared Helpers
// ============================================================================
//
// Frame I/O, pixel statistics and the headless engine lifecycle live in
// tests/helpers/. The using declarations below keep the call sites reading as
// they did when every one of these was defined locally in this file.

namespace {

using ZHLN::Test::Image::ChangedRegion;
using ZHLN::Test::Image::CoefficientOfVariation;
using ZHLN::Test::Image::CompareFrames;
using ZHLN::Test::Image::DiffRegion;
using ZHLN::Test::Image::FrameDiff;
using ZHLN::Test::Image::FrameMetrics;
using ZHLN::Test::Image::LoadPPM;
using ZHLN::Test::Image::Luma;
using ZHLN::Test::Image::Mean;
using ZHLN::Test::Image::MeasureImage;
using ZHLN::Test::Image::MeasureSubRegion;
using ZHLN::Test::Image::PngPathOf;
using ZHLN::Test::Image::RgbImage;
using ZHLN::Test::Image::SavePNG;
using ZHLN::Test::Image::WriteAmplifiedDiff;
using ZHLN::Test::Image::WriteRegionCrop;

using ZHLN::Test::Headless::Capture;
using ZHLN::Test::Headless::DisableTAA;
using ZHLN::Test::Headless::RunStableScene;
using ZHLN::Test::Headless::StableRunResult;
using ZHLN::Test::Headless::TickFrames;

/// This suite needs a larger physics slab than the fixture default, and the
/// window title identifies it in a capture directory shared with other suites.
[[nodiscard]] inline auto CreateTestEngine(uint32_t width = 640, uint32_t height = 480) -> std::unique_ptr<ZHLN::Engine> {
    return ZHLN::Test::Headless::CreateEngine(ZHLN::Test::Headless::EngineOptions {
        .appName               = "Headless Lighting RT Test",
        .width                 = width,
        .height                = height,
        .maxBodies             = 512,
        .maxBodyPairs          = 1024,
        .maxContactConstraints = 1024,
    });
}

} // namespace

// ============================================================================
// Test Suite
// ============================================================================

struct LightingRTTestSuite {
    LightingRTTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~LightingRTTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // ====================================================================
        // 1. Lit Scene Static Frame Stability
        // ====================================================================
        std::expected<void, ZHLN::Error> lit_scene_static_frame_stability() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 10.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                    });
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.55f, 0.55f, 0.58f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false}
                );

                auto grayMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.65f, .baseColor = {0.8f, 0.8f, 0.8f, 1.0f}}
                );
                auto redMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.7f, .baseColor = {0.9f, 0.1f, 0.1f, 1.0f}}
                );
                auto blueMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.1f, 0.2f, 0.9f, 1.0f}}
                );

                auto checkMaterials = ZHLN::Test::AssertTrue(grayMatRes && redMatRes && blueMatRes);
                if (!checkMaterials) {
                    return checkMaterials;
                }

                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(-2.2, 1.0, 0.0), .createPhysics = false, .materialOverride = *grayMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.0, -2.0), .createPhysics = false, .materialOverride = *redMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(2.2, 1.0, 1.0), .createPhysics = false, .materialOverride = *blueMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 40.0f, 30.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({45.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 220.0f,
                        .direction = JPH::Vec3(0.0f, 0.6f, 0.8f).Normalized()
                    }
                );

                auto addPointLight = [&](const JPH::Vec3& pos, const JPH::Vec3& color, float intensity) {
                    const ZHLN::Entity e = reg.Create();
                    reg.Add(
                        e, ZHLN::Components::TransformComponent {.position = pos},
                        ZHLN::Components::LightComponent {.type = ZHLN::LightType::Point, .color = color, .intensity = intensity, .range = 30.0f}
                    );
                };
                addPointLight(JPH::Vec3(-4.0f, 3.0f, 2.0f), JPH::Vec3(1.0f, 0.55f, 0.3f), 800.0f);
                addPointLight(JPH::Vec3(4.0f, 3.0f, -3.0f), JPH::Vec3(0.3f, 0.5f, 1.0f), 800.0f);

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 2.5f, 8.0f);
                cam.yaw      = -90.0f;
                cam.pitch    = -12.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 14, "lit_scene_static_frame_stability",
                [](ZHLN::Engine& eng) -> bool {
                    const RgbImage  repeatA    = Capture(eng, "headless_lighting_rt_static_r0.ppm");
                    const RgbImage  repeatB    = Capture(eng, "headless_lighting_rt_static_r1.ppm");
                    const FrameDiff repeatDiff = CompareFrames(repeatA, repeatB);

                    std::vector<double>    litSeries;
                    std::vector<double>    lumaSeries;
                    std::vector<double>    redSeries;
                    std::vector<double>    satSeries;
                    std::vector<FrameDiff> temporalDiffs;

                    RgbImage prev;
                    for (uint32_t f = 0; f < 4; ++f) {
                        TickFrames(eng, 1);
                        const RgbImage frame = Capture(eng, "headless_lighting_rt_static_f" + std::to_string(f) + ".ppm");

                        auto checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                        if (!checkFrame) {
                            return false;
                        }

                        const FrameMetrics m = MeasureImage(frame);
                        litSeries.push_back(static_cast<double>(m.lit));
                        lumaSeries.push_back(m.meanLuma);
                        redSeries.push_back(static_cast<double>(m.red));
                        satSeries.push_back(static_cast<double>(m.saturated));

                        if (prev.Valid()) {
                            temporalDiffs.push_back(CompareFrames(prev, frame));
                        }
                        prev = frame;
                    }

                    ZHLN::Test::ExpectFalse(eng.GetVisibleEntities().empty());
                    ZHLN::Test::ExpectTrue(ZHLN::CullingStats::TotalTriangles > 0);

                    const bool frameProduced   = ZHLN::Test::ExpectTrue(Mean(lumaSeries) > 1.0);
                    const bool geometryVisible = ZHLN::Test::ExpectTrue(Mean(litSeries) > 500.0);
                    if (!frameProduced || !geometryVisible) {
                        return false;
                    }

                    const double litCV   = CoefficientOfVariation(litSeries);
                    const double lumaCV  = CoefficientOfVariation(lumaSeries);
                    const double redCV   = CoefficientOfVariation(redSeries);
                    const double satCV   = CoefficientOfVariation(satSeries);
                    const double litMean = Mean(litSeries);

                    double worstFrac32 = 0.0;
                    double maxJump     = 0.0;
                    for (size_t i = 0; i < temporalDiffs.size(); ++i) {
                        worstFrac32 = std::max(worstFrac32, temporalDiffs[i].frac32);
                        if (i + 1 < litSeries.size() && litMean > 1.0) {
                            const double jump = std::abs(litSeries[i + 1] - litSeries[i]) / litMean;
                            maxJump           = std::max(maxJump, jump);
                        }
                    }

                    const double meanSaturated = Mean(satSeries);
                    const bool   stableLit     = ZHLN::Test::ExpectTrue(litCV < 0.03);
                    const bool   stableLuma    = ZHLN::Test::ExpectTrue(lumaCV < 0.01);
                    const bool   stableRed     = ZHLN::Test::ExpectTrue(redCV < 0.05);
                    const bool   stableSat     = ZHLN::Test::ExpectTrue(satCV < 0.25 || meanSaturated < 100.0);
                    const bool   noPixelPop    = ZHLN::Test::ExpectTrue(worstFrac32 < 0.01);
                    const bool   noJump        = ZHLN::Test::ExpectTrue(maxJump < 0.08);
                    const bool   noBlowout     = ZHLN::Test::ExpectTrue(meanSaturated < 0.02 * static_cast<double>(640 * 480));

                    return stableLit && stableLuma && stableRed && stableSat && noPixelPop && noJump && noBlowout;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::TemporalFlickerDetected);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            return {};
        }

        // ====================================================================
        // 2. Point Light Cluster Culling Sweep
        // ====================================================================
        std::expected<void, ZHLN::Error> point_light_cluster_culling_sweep() {
            auto engine      = CreateTestEngine(320, 240);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            ZHLN::Entity redLight = ZHLN::Entity::Null();
            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 2.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                auto diffuseMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.75f, 0.75f, 0.75f, 1.0f}}
                );
                auto checkDiffuse = ZHLN::Test::AssertTrue(diffuseMatRes.has_value());
                if (!checkDiffuse) {
                    return checkDiffuse;
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.5f, 0.5f, 0.52f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.7, 5.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 30.0f, 20.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({30.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 40.0f,
                        .direction = JPH::Vec3(0.0f, 0.5f, 0.85f).Normalized()
                    }
                );

                redLight = reg.Create();
                reg.Add(
                    redLight, ZHLN::Components::TransformComponent {.position = JPH::Vec3(-6.4f, 2.4f, 5.5f)},
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Point, .color = JPH::Vec3(1.0f, 0.06f, 0.03f), .intensity = 1600.0f, .range = 40.0f
                    }
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 2.6f, -4.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -8.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 8, "point_light_cluster_culling_sweep",
                [&](ZHLN::Engine& eng) -> bool {
                    auto& reg = eng.GetRegistry();
                    ZHLN::Test::ExpectTrue(reg.IsAlive(redLight));

                    std::vector<double>   redCounts;
                    std::vector<uint32_t> redPeaks;
                    uint32_t              isolatedDips = 0;

                    constexpr int   kSteps = 21;
                    constexpr float kStepX = 0.64f;
                    for (int step = 0; step < kSteps; ++step) {
                        const float x = -6.4f + static_cast<float>(step) * kStepX;
                        const float z = 5.5f - 0.078125f * x;

                        reg.Patch<ZHLN::Components::TransformComponent>(redLight, [&](auto& t) { t.position = JPH::Vec3(x, 2.4f, z); });

                        TickFrames(eng, 2);

                        const RgbImage frame      = Capture(eng, "headless_lighting_rt_cull_" + std::to_string(step) + ".ppm");
                        auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                        if (!checkFrame) {
                            return false;
                        }

                        const FrameMetrics m = MeasureImage(frame);
                        redCounts.push_back(static_cast<double>(m.red));
                        redPeaks.push_back(m.redPeak);
                    }

                    for (size_t i = 1; i + 1 < redCounts.size(); ++i) {
                        const double left  = redCounts[i - 1];
                        const double right = redCounts[i + 1];
                        const double base  = std::min(left, right);
                        if (base > 64.0 && redCounts[i] < 0.5 * base) {
                            ++isolatedDips;
                        }
                    }

                    TickFrames(eng, 1);
                    constexpr uint32_t                  kStableFrames = 8;
                    std::array<RgbImage, kStableFrames> stableFrames {};
                    std::vector<double>                 stableCounts;
                    std::vector<double>                 stableLuma;
                    std::vector<double>                 stableLit;
                    std::vector<uint64_t>               stableFrameIdx;
                    for (uint32_t r = 0; r < kStableFrames; ++r) {
                        stableFrameIdx.push_back(eng.GetCurrentFrame());
                        stableFrames[r]  = Capture(eng, "headless_lighting_rt_cull_stable_" + std::to_string(r) + ".ppm");
                        auto checkStable = ZHLN::Test::AssertTrue(stableFrames[r].Valid());
                        if (!checkStable) {
                            return false;
                        }
                        const FrameMetrics m = MeasureImage(stableFrames[r]);
                        stableCounts.push_back(static_cast<double>(m.red));
                        stableLuma.push_back(m.meanLuma);
                        stableLit.push_back(static_cast<double>(m.lit));

                        TickFrames(eng, 1);
                    }

                    double    worstPairFrac = 0.0;
                    size_t    worstPair     = 0;
                    FrameDiff worstDiffs[kStableFrames - 1] {};
                    for (uint32_t i = 0; i + 1 < kStableFrames; ++i) {
                        worstDiffs[i] = CompareFrames(stableFrames[i], stableFrames[i + 1]);
                        if (worstDiffs[i].frac32 > worstPairFrac) {
                            worstPairFrac = worstDiffs[i].frac32;
                            worstPair     = i;
                        }
                    }
                    const ChangedRegion region = DiffRegion(stableFrames[worstPair], stableFrames[worstPair + 1]);
                    WriteAmplifiedDiff("headless_lighting_rt_cull_parity_diff.ppm", stableFrames[worstPair], stableFrames[worstPair + 1]);
                    WriteRegionCrop("headless_lighting_rt_cull_region_a.ppm", stableFrames[worstPair], region);
                    WriteRegionCrop("headless_lighting_rt_cull_region_b.ppm", stableFrames[worstPair + 1], region);

                    const double   minRed  = *std::ranges::min_element(redCounts);
                    const uint32_t minPeak = *std::ranges::min_element(redPeaks);

                    const bool neverCulled      = ZHLN::Test::ExpectTrue(minRed > 16.0);
                    const bool brightEverywhere = ZHLN::Test::ExpectTrue(minPeak > 60u);
                    const bool noIsolatedCull   = ZHLN::Test::ExpectTrue(isolatedDips == 0u);
                    const bool noStaticStep     = ZHLN::Test::ExpectTrue(worstPairFrac < 0.005);

                    return neverCulled && brightEverywhere && noIsolatedCull && noStaticStep;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::TemporalFlickerDetected);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            return {};
        }

        // ====================================================================
        // 3. Point Light Static Reference (No History)
        // ====================================================================
        std::expected<void, ZHLN::Error> point_light_static_reference_no_history() {
            auto engine      = CreateTestEngine(320, 240);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            const JPH::Vec3 lightPos(6.4f, 2.4f, 5.0f);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 2.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                auto diffuseMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.75f, 0.75f, 0.75f, 1.0f}}
                );
                auto checkDiffuse = ZHLN::Test::AssertTrue(diffuseMatRes.has_value());
                if (!checkDiffuse) {
                    return checkDiffuse;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.5f, 0.5f, 0.52f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.7, 5.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 30.0f, 20.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({30.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 40.0f,
                        .direction = JPH::Vec3(0.0f, 0.5f, 0.85f).Normalized()
                    }
                );
                const ZHLN::Entity redLight = reg.Create();
                reg.Add(
                    redLight, ZHLN::Components::TransformComponent {.position = lightPos},
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Point, .color = JPH::Vec3(1.0f, 0.06f, 0.03f), .intensity = 1600.0f, .range = 40.0f
                    }
                );
                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 2.6f, -4.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -8.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 8, "point_light_static_reference_no_history",
                [&](ZHLN::Engine& eng) -> bool {
                    constexpr uint32_t                  kStableFrames = 8;
                    std::array<RgbImage, kStableFrames> frames {};
                    std::vector<double>                 counts;
                    for (uint32_t r = 0; r < kStableFrames; ++r) {
                        frames[r]       = Capture(eng, "headless_lighting_rt_ref_stable_" + std::to_string(r) + ".ppm");
                        auto checkFrame = ZHLN::Test::AssertTrue(frames[r].Valid());
                        if (!checkFrame) {
                            return false;
                        }
                        counts.push_back(static_cast<double>(MeasureImage(frames[r]).red));

                        TickFrames(eng, 1);
                    }

                    double worstFrac = 0.0;
                    size_t worstPair = 0;
                    for (uint32_t i = 0; i + 1 < kStableFrames; ++i) {
                        const double f = CompareFrames(frames[i], frames[i + 1]).frac32;
                        if (f > worstFrac) {
                            worstFrac = f;
                            worstPair = i;
                        }
                    }

                    const ChangedRegion region = DiffRegion(frames[worstPair], frames[worstPair + 1]);
                    WriteAmplifiedDiff("headless_lighting_rt_ref_parity_diff.ppm", frames[worstPair], frames[worstPair + 1]);
                    WriteRegionCrop("headless_lighting_rt_ref_region_a.ppm", frames[worstPair], region);
                    WriteRegionCrop("headless_lighting_rt_ref_region_b.ppm", frames[worstPair + 1], region);

                    const bool noStaticStep = ZHLN::Test::ExpectTrue(worstFrac < 0.005);
                    const bool lightVisible = ZHLN::Test::ExpectTrue(Mean(counts) > 16.0);

                    return noStaticStep && lightVisible;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::TemporalFlickerDetected);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            return {};
        }

        // ====================================================================
        // 4. Ray-Traced Shadow Occlusion & Stability
        // ====================================================================
        std::expected<void, ZHLN::Error> raytraced_shadow_occlusion_and_stability() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] No raytracing support on this device; nothing to verify for RT shadows.");
                return {};
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 1.5f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 1;
                    });
                }

                auto floorMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.55f, 0.55f, 0.55f, 1.0f}}
                );
                auto checkFloorMat = ZHLN::Test::AssertTrue(floorMatRes.has_value());
                if (!checkFloorMat) {
                    return checkFloorMat;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 400.0f, {0.55f, 0.55f, 0.55f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *floorMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(60.0f, 45.0f, 0.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({0.0f, 90.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 240.0f,
                        .direction = JPH::Vec3(0.8f, 0.6f, 0.0f).Normalized()
                    }
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(-10.0f, 6.0f, -8.0f);
                cam.yaw      = 0.0f;
                cam.pitch    = -20.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 8, "raytraced_shadow_occlusion_and_stability",
                [](ZHLN::Engine& eng) -> bool {
                    auto& reg = eng.GetRegistry();

                    const ZHLN::Entity occluder = ZHLN::CreativeWorksFactory::CreateBox(
                        eng, JPH::Vec3(0.5f, 3.0f, 4.0f),
                        ZHLN::CreativeWorksFactory::SpawnParams {
                            .position = JPH::RVec3(0.0, 3.0, -8.0), .createPhysics = false, .color = {0.7f, 0.7f, 0.7f, 1.0f}
                        }
                    );

                    TickFrames(eng, 2);

                    const RgbImage shadowA       = Capture(eng, "headless_lighting_rt_shadow_a.ppm");
                    const RgbImage shadowARepeat = Capture(eng, "headless_lighting_rt_shadow_a_repeat.ppm");
                    TickFrames(eng, 1);
                    const RgbImage shadowB = Capture(eng, "headless_lighting_rt_shadow_b.ppm");

                    auto checkFrame = ZHLN::Test::AssertTrue(shadowA.Valid() && shadowARepeat.Valid() && shadowB.Valid());
                    if (!checkFrame) {
                        reg.Destroy(occluder);
                        return false;
                    }

                    reg.Destroy(occluder);
                    ZHLN::Test::ExpectFalse(reg.IsAlive(occluder));

                    TickFrames(eng, 2);
                    const RgbImage shadowClear = Capture(eng, "headless_lighting_rt_shadow_clear.ppm");
                    checkFrame                 = ZHLN::Test::AssertTrue(shadowClear.Valid());
                    if (!checkFrame) {
                        return false;
                    }

                    constexpr double   kFloorRowFraction = 0.72;
                    const FrameMetrics mA                = MeasureImage(shadowA, kFloorRowFraction);
                    const FrameMetrics mA2               = MeasureImage(shadowARepeat, kFloorRowFraction);
                    const FrameMetrics mB                = MeasureImage(shadowB, kFloorRowFraction);
                    const FrameMetrics mClear            = MeasureImage(shadowClear, kFloorRowFraction);

                    const uint32_t darkA     = mA.dark;
                    const uint32_t darkA2    = mA2.dark;
                    const uint32_t darkB     = mB.dark;
                    const uint32_t darkClear = mClear.dark;
                    const uint32_t litA      = mA.lit;
                    const uint32_t litClear  = mClear.lit;

                    const FrameDiff repeatDiff   = CompareFrames(shadowA, shadowARepeat);
                    const FrameDiff temporalDiff = CompareFrames(shadowA, shadowB);

                    const double darkJump = (darkA > 0) ? static_cast<double>(std::abs(static_cast<int64_t>(darkB) - static_cast<int64_t>(darkA))) /
                                                              static_cast<double>(darkA) :
                                                          0.0;

                    const bool shadowExist     = ZHLN::Test::ExpectTrue(darkA > darkClear + 1500u);
                    const bool lightRestored   = ZHLN::Test::ExpectTrue(darkClear < darkA / 3u);
                    const bool meanBrightens   = ZHLN::Test::ExpectTrue(mClear.meanLuma > mA.meanLuma * 1.25 + 1.0);
                    const bool notBlackout     = ZHLN::Test::ExpectTrue(darkA < 0.85 * static_cast<double>(mA.total));
                    const bool shadowStable    = ZHLN::Test::ExpectTrue(darkJump < 0.15);
                    const bool noShadowFlicker = ZHLN::Test::ExpectTrue(temporalDiff.frac32 < 0.015);
                    const bool repeatClean     = ZHLN::Test::ExpectTrue(repeatDiff.frac32 == 0.0);

                    return shadowExist && lightRestored && meanBrightens && notBlackout && shadowStable && noShadowFlicker && repeatClean;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::RayTracedShadowFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            return {};
        }

        // ====================================================================
        // 5. Ray-Traced Reflection Coverage & Artifacts
        // ====================================================================
        std::expected<void, ZHLN::Error> raytraced_reflection_coverage_and_artifacts() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 6.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                    });
                }

                auto mirrorMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.03f, .baseColor = {0.85f, 0.85f, 0.88f, 1.0f}}
                );
                auto checkMirror = ZHLN::Test::AssertTrue(mirrorMatRes.has_value());
                if (!checkMirror) {
                    return checkMirror;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.85f, 0.85f, 0.88f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *mirrorMatRes}
                );

                auto emissiveMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.55f, .baseColor = {1.0f, 0.06f, 0.04f, 1.0f}, .emissive = {1.0f, 0.0f, 0.0f, 1.0f}
                        }
                );
                auto checkEmissive = ZHLN::Test::AssertTrue(emissiveMatRes.has_value());
                if (!checkEmissive) {
                    return checkEmissive;
                }
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 3.0, 0.0), .createPhysics = false, .materialOverride = *emissiveMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 50.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({40.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 140.0f,
                        .direction = JPH::Vec3(0.0f, 0.75f, 0.66f).Normalized()
                    }
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 5.0f, 14.0f);
                cam.yaw      = -90.0f;
                cam.pitch    = -22.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 10, "raytraced_reflection_coverage_and_artifacts",
                [](ZHLN::Engine& eng) -> bool {
                    std::vector<double> reflectionSeries;
                    std::vector<double> saturationSeries;
                    std::vector<double> isolatedSeries;
                    for (uint32_t f = 0; f < 4; ++f) {
                        TickFrames(eng, 1);
                        const RgbImage frame      = Capture(eng, "headless_lighting_rt_reflect_f" + std::to_string(f) + ".ppm");
                        auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                        if (!checkFrame) {
                            return false;
                        }

                        const FrameMetrics m = MeasureImage(frame, 0.5);
                        reflectionSeries.push_back(static_cast<double>(m.red));
                        saturationSeries.push_back(static_cast<double>(m.saturated));
                        isolatedSeries.push_back(m.red > 0 ? static_cast<double>(m.redIsolated) / static_cast<double>(m.red) : 0.0);
                    }

                    const double   meanReflection  = Mean(reflectionSeries);
                    const double   reflectionCV    = CoefficientOfVariation(reflectionSeries);
                    const double   saturationCV    = CoefficientOfVariation(saturationSeries);
                    const double   meanSaturation  = Mean(saturationSeries);
                    const double   isolatedRatio   = Mean(isolatedSeries);
                    const uint32_t lowerHalfPixels = static_cast<uint32_t>(640 * 480 / 2);

                    const bool reflectionPresent = ZHLN::Test::ExpectTrue(meanReflection > 24.0);
                    const bool reflectionStable  = ZHLN::Test::ExpectTrue(reflectionCV < 0.15);
                    const bool noBlowout         = ZHLN::Test::ExpectTrue(meanSaturation < 0.04 * static_cast<double>(lowerHalfPixels));
                    const bool noRayDebris       = ZHLN::Test::ExpectTrue(isolatedRatio < 0.35);
                    const bool saturationStable  = ZHLN::Test::ExpectTrue(saturationCV < 0.25 || meanSaturation < 100.0);

                    return reflectionPresent && reflectionStable && noBlowout && noRayDebris && saturationStable;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::ReflectionArtifacts);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            return {};
        }

        // ====================================================================
        // 6. Multi-Light Clustered Accumulation & Chromatic Interaction
        // ====================================================================
        std::expected<void, ZHLN::Error> multi_light_cluster_accumulation_and_chromatic_interaction() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 1.0f;
                        pp.enableSSR       = 0;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                // Explicit 0-intensity Sun to suppress the default 180-nit white sun injection
                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 40.0f, 30.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({45.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 0.0f,
                        .direction = JPH::Vec3(0.0f, 0.6f, 0.8f).Normalized()
                    }
                );

                // Neutral diffuse gray floor
                auto neutralMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.8f, 0.8f, 0.8f, 1.0f}}
                );
                auto checkNeutral = ZHLN::Test::AssertTrue(neutralMat.has_value());
                if (!checkNeutral) {
                    return checkNeutral;
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 80.0f, {0.8f, 0.8f, 0.8f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = false, .materialOverride = *neutralMat}
                );

                // Central pedestal at quadrant boundary to test additive color mixing
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(1.0f, 0.6f, 1.0f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.6, 0.0), .createPhysics = false, .materialOverride = *neutralMat}
                );

                // 64 Point Lights partitioned into 4 chromatic quadrants (perspective-aligned for yaw=+90 look along +Z):
                //   World X > 0, Z > 0 -> Screen Top-Left:     Pure Red   (1.0, 0.02, 0.02)
                //   World X <= 0, Z > 0 -> Screen Top-Right:   Pure Green (0.02, 1.0, 0.02)
                //   World X > 0, Z < 0 -> Screen Bottom-Left:  Pure Blue  (0.02, 0.02, 1.0)
                //   World X <= 0, Z < 0 -> Screen Bottom-Right: Amber     (1.0, 0.85, 0.15)
                constexpr int kGridDim = 8;
                for (int gx = 0; gx < kGridDim; ++gx) {
                    for (int gz = 0; gz < kGridDim; ++gz) {
                        float posX = -14.0f + static_cast<float>(gx) * 4.0f;
                        float posZ = -14.0f + static_cast<float>(gz) * 4.0f;
                        float posY = 2.0f;

                        JPH::Vec3 lightColor(1.0f, 1.0f, 1.0f);
                        if (posX > 0.0f && posZ >= 0.0f) {
                            lightColor = JPH::Vec3(1.0f, 0.02f, 0.02f); // Red -> Screen Top-Left
                        } else if (posX <= 0.0f && posZ >= 0.0f) {
                            lightColor = JPH::Vec3(0.02f, 1.0f, 0.02f); // Green -> Screen Top-Right
                        } else if (posX > 0.0f && posZ < 0.0f) {
                            lightColor = JPH::Vec3(0.02f, 0.02f, 1.0f); // Blue -> Screen Bottom-Left
                        } else {
                            lightColor = JPH::Vec3(1.0f, 0.85f, 0.15f); // Amber -> Screen Bottom-Right
                        }

                        const ZHLN::Entity lt = reg.Create();
                        reg.Add(
                            lt, ZHLN::Components::TransformComponent {.position = JPH::Vec3(posX, posY, posZ)},
                            ZHLN::Components::LightComponent {
                                .type      = ZHLN::LightType::Point,
                                .color     = lightColor,
                                .intensity = 250.0f,
                                .range     = 14.0f,
                            }
                        );
                    }
                }

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 22.0f, -24.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -42.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 8, "multi_light_cluster_accumulation_and_chromatic_interaction",
                [](ZHLN::Engine& eng) -> bool {
                    const RgbImage frame      = Capture(eng, "headless_lighting_multi_cluster.ppm");
                    auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                    if (!checkFrame) {
                        return false;
                    }

                    // Normalized Quadrant Sampling:
                    const auto quadTL    = MeasureSubRegion(frame, {.x0 = 0.05, .y0 = 0.05, .x1 = 0.40, .y1 = 0.45});
                    const auto quadTR    = MeasureSubRegion(frame, {.x0 = 0.60, .y0 = 0.05, .x1 = 0.95, .y1 = 0.45});
                    const auto quadBL    = MeasureSubRegion(frame, {.x0 = 0.05, .y0 = 0.55, .x1 = 0.40, .y1 = 0.95});
                    const auto centerMix = MeasureSubRegion(frame, {.x0 = 0.45, .y0 = 0.35, .x1 = 0.55, .y1 = 0.65});

                    ZHLN::Println("    [INFO] Multi-light 64-Light Clustered Grid:");
                    ZHLN::Println(
                        "      Top-Left Quad (Red):    MeanRGB=({:.1f},{:.1f},{:.1f}), DominantRed={}/{}", quadTL.meanR, quadTL.meanG, quadTL.meanB,
                        quadTL.dominantRed, quadTL.pixels
                    );
                    ZHLN::Println(
                        "      Top-Right Quad (Green): MeanRGB=({:.1f},{:.1f},{:.1f}), DominantGreen={}/{}", quadTR.meanR, quadTR.meanG, quadTR.meanB,
                        quadTR.dominantGrn, quadTR.pixels
                    );
                    ZHLN::Println(
                        "      Bottom-Left Quad (Blue):MeanRGB=({:.1f},{:.1f},{:.1f}), DominantBlue={}/{}", quadBL.meanR, quadBL.meanG, quadBL.meanB,
                        quadBL.dominantBlu, quadBL.pixels
                    );
                    ZHLN::Println(
                        "      Center Mixing (R+G->Y): MeanRGB=({:.1f},{:.1f},{:.1f}), YellowMixPixels={}/{}", centerMix.meanR, centerMix.meanG,
                        centerMix.meanB, centerMix.yellowMix, centerMix.pixels
                    );

                    // Every gate below is a ratio -- channel against channel, or
                    // saturated pixels as a share of their region -- so the scene can be
                    // re-exposed without the assertions moving. Absolute means are
                    // exposure/tone-map outputs, not lighting behaviour.

                    // 1. Quadrant Chromatic Purity
                    const bool redDominant = ZHLN_CHECK(
                        quadTL.meanR > 1.3 * quadTL.meanG && quadTL.meanR > 1.3 * quadTL.meanB && quadTL.dominantRed * 100 > quadTL.pixels,
                        "top-left quadrant is red-dominant",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantRed={}/{} px (need >1% of the quad)", quadTL.meanR, quadTL.meanG, quadTL.meanB,
                        quadTL.dominantRed, quadTL.pixels
                    );
                    const bool greenDominant = ZHLN_CHECK(
                        quadTR.meanG > 1.3 * quadTR.meanR && quadTR.meanG > 1.3 * quadTR.meanB && quadTR.dominantGrn * 100 > quadTR.pixels,
                        "top-right quadrant is green-dominant",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantGreen={}/{} px (need >1% of the quad)", quadTR.meanR, quadTR.meanG, quadTR.meanB,
                        quadTR.dominantGrn, quadTR.pixels
                    );
                    const bool blueDominant = ZHLN_CHECK(
                        quadBL.meanB > 1.3 * quadBL.meanR && quadBL.meanB > 1.3 * quadBL.meanG && quadBL.dominantBlu * 100 > quadBL.pixels,
                        "bottom-left quadrant is blue-dominant",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantBlue={}/{} px (need >1% of the quad)", quadBL.meanR, quadBL.meanG, quadBL.meanB,
                        quadBL.dominantBlu, quadBL.pixels
                    );

                    // 2. Additive Color Superposition at boundary (Red + Green -> Yellow).
                    // The pedestal sits under all four quadrants, so it must out-shine
                    // each pure quadrant in its own channel -- lights accumulating rather
                    // than the nearest one winning.
                    const bool additiveMixing = ZHLN_CHECK(
                        centerMix.meanR > 1.3 * centerMix.meanB && centerMix.meanG > 1.3 * centerMix.meanB && centerMix.meanR > 0.6 * centerMix.meanG &&
                            centerMix.meanG > 0.6 * centerMix.meanR && centerMix.meanR > 0.5 * quadTL.meanR && centerMix.meanG > 0.5 * quadTR.meanG &&
                            centerMix.yellowMix * 100 > centerMix.pixels,
                        "quadrant boundary mixes red + green into yellow",
                        "centerMeanRGB=({:.1f},{:.1f},{:.1f}) vs redQuad.meanR={:.1f} greenQuad.meanG={:.1f}, yellowMix={}/{} px (need >1%)", centerMix.meanR,
                        centerMix.meanG, centerMix.meanB, quadTL.meanR, quadTR.meanG, centerMix.yellowMix, centerMix.pixels
                    );

                    // 3. Coverage & headroom. "lit" counts Luma > 40, which a pure blue
                    // pixel can never reach (0.0722 * 255 = 18.4), so in a scene that is
                    // a quarter blue by construction that metric grades the palette
                    // instead of the lighting. Count saturated chroma instead: it is
                    // hue-aware and, as a share of the sampled area, exposure-relative.
                    const uint32_t chromaticPixels = quadTL.dominantRed + quadTR.dominantGrn + quadBL.dominantBlu + centerMix.yellowMix;
                    const uint32_t sampledPixels   = quadTL.pixels + quadTR.pixels + quadBL.pixels + centerMix.pixels;
                    const bool     lightCovered    = ZHLN_CHECK(
                        chromaticPixels * 10 > sampledPixels, "clustered lights cover a meaningful share of the frame",
                        "chromaticPixels={}/{} sampled px (need >10%)", chromaticPixels, sampledPixels
                    );

                    const FrameMetrics fullFrame         = MeasureImage(frame);
                    const bool         noBlackout        = ZHLN_CHECK(
                        fullFrame.meanLuma > 1.0, "frame is not blacked out", "meanLuma={:.2f} over {} px", fullFrame.meanLuma, fullFrame.total
                    );
                    const bool         noExtremeOverflow = ZHLN_CHECK(
                        fullFrame.saturated * 20 < fullFrame.total, "frame is not blown out", "saturated={}/{} px (need <5%)", fullFrame.saturated,
                        fullFrame.total
                    );

                    return redDominant && greenDominant && blueDominant && additiveMixing && lightCovered && noBlackout && noExtremeOverflow;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::MultiLightClusteringFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            ZHLN::Println("    [PASS] 64 Clustered lights correctly accumulated with clean chromatic superposition.");
            return {};
        }

        // ====================================================================
        // 7. Multi-Emissive Sources & Surface Reflection Interaction
        // ====================================================================
        std::expected<void, ZHLN::Error> multi_emissive_sources_and_surface_reflection_interaction() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 6.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                // Sun illumination to support scene depth and HDR tone mapping
                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 50.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({40.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 100.0f,
                        .direction = JPH::Vec3(0.0f, 0.75f, 0.66f).Normalized()
                    }
                );

                // Polished metallic mirror floor
                auto mirrorMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.02f, .baseColor = {0.9f, 0.9f, 0.95f, 1.0f}}
                );
                auto checkMirror = ZHLN::Test::AssertTrue(mirrorMat.has_value());
                if (!checkMirror) {
                    return checkMirror;
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.9f, 0.9f, 0.95f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = false, .materialOverride = *mirrorMat}
                );

                // 4 Distinct High-Luminance Emissive Geometric Emitters at Y = 3.0, Z = 0.0:
                //   Emitter 1: X = -4.5 (Pure Red)
                //   Emitter 2: X = -1.5 (Pure Green)
                //   Emitter 3: X = +1.5 (Pure Blue)
                //   Emitter 4: X = +4.5 (Golden Yellow)
                auto matEmissiveRed = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.05f, 0.05f, 1.0f}, .emissive = {6.0f, 0.0f, 0.0f, 1.0f}
                        }
                );
                auto matEmissiveGrn = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.05f, 1.0f, 0.05f, 1.0f}, .emissive = {0.0f, 6.0f, 0.0f, 1.0f}
                        }
                );
                auto matEmissiveBlu = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.05f, 0.05f, 1.0f, 1.0f}, .emissive = {0.0f, 0.0f, 6.0f, 1.0f}
                        }
                );
                auto matEmissiveYel = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.9f, 0.05f, 1.0f}, .emissive = {5.0f, 4.5f, 0.0f, 1.0f}
                        }
                );

                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(-4.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveRed
                    }
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(-1.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveGrn
                    }
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(1.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveBlu}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(4.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveYel}
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 5.0f, 14.0f);
                cam.yaw      = -90.0f;
                cam.pitch    = -22.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 8, "multi_emissive_sources_and_surface_reflection_interaction",
                [](ZHLN::Engine& eng) -> bool {
                    const RgbImage frame      = Capture(eng, "headless_lighting_multi_emissive.ppm");
                    auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                    if (!checkFrame) {
                        return false;
                    }

                    // Slicing lower reflection half into 4 horizontal column bands centered on planar reflection centroids:
                    //   Strip 1 (Red Refl Centroid   ~0.30): X in [0.20, 0.36], Y in [0.52, 0.95]
                    //   Strip 2 (Green Refl Centroid ~0.43): X in [0.38, 0.48], Y in [0.52, 0.95]
                    //   Strip 3 (Blue Refl Centroid  ~0.57): X in [0.52, 0.62], Y in [0.52, 0.95]
                    //   Strip 4 (Yellow Refl Centroid~0.70): X in [0.64, 0.80], Y in [0.52, 0.95]
                    const auto reflStripRed = MeasureSubRegion(frame, {.x0 = 0.20, .y0 = 0.52, .x1 = 0.36, .y1 = 0.95});
                    const auto reflStripGrn = MeasureSubRegion(frame, {.x0 = 0.38, .y0 = 0.52, .x1 = 0.48, .y1 = 0.95});
                    const auto reflStripBlu = MeasureSubRegion(frame, {.x0 = 0.52, .y0 = 0.52, .x1 = 0.62, .y1 = 0.95});
                    const auto reflStripYel = MeasureSubRegion(frame, {.x0 = 0.64, .y0 = 0.52, .x1 = 0.80, .y1 = 0.95});

                    ZHLN::Println("    [INFO] Multi-Emissive Planar Mirror Reflection Slices:");
                    ZHLN::Println(
                        "      Strip 1 (Refl Red):    MeanRGB=({:.1f},{:.1f},{:.1f}), DominantRed={}", reflStripRed.meanR, reflStripRed.meanG,
                        reflStripRed.meanB, reflStripRed.dominantRed
                    );
                    ZHLN::Println(
                        "      Strip 2 (Refl Green):  MeanRGB=({:.1f},{:.1f},{:.1f}), DominantGreen={}", reflStripGrn.meanR, reflStripGrn.meanG,
                        reflStripGrn.meanB, reflStripGrn.dominantGrn
                    );
                    ZHLN::Println(
                        "      Strip 3 (Refl Blue):   MeanRGB=({:.1f},{:.1f},{:.1f}), DominantBlue={}", reflStripBlu.meanR, reflStripBlu.meanG,
                        reflStripBlu.meanB, reflStripBlu.dominantBlu
                    );
                    ZHLN::Println(
                        "      Strip 4 (Refl Yellow): MeanRGB=({:.1f},{:.1f},{:.1f}), YellowMixPixels={}", reflStripYel.meanR, reflStripYel.meanG,
                        reflStripYel.meanB, reflStripYel.yellowMix
                    );

                    // 1. Spatial mirror correspondence. Each gate is a ratio: channel
                    // against channel inside the strip, or bright pixels as a share of
                    // the strip. Absolute means are exposure outputs -- the same correct
                    // reflection measures 5.9 or 59.0 depending on ambientExposure -- and
                    // the strips are not even the same width, so a shared absolute floor
                    // grades geometry rather than the reflection.

                    const bool reflRedOk = ZHLN_CHECK(
                        reflStripRed.dominantRed * 200 > reflStripRed.pixels && reflStripRed.meanR > 1.3 * reflStripRed.meanG &&
                            reflStripRed.meanR > 1.3 * reflStripRed.meanB,
                        "strip 1 mirrors the red emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantRed={}/{} px (need >0.5% of the strip)", reflStripRed.meanR, reflStripRed.meanG,
                        reflStripRed.meanB, reflStripRed.dominantRed, reflStripRed.pixels
                    );
                    const bool reflGrnOk = ZHLN_CHECK(
                        reflStripGrn.dominantGrn * 200 > reflStripGrn.pixels && reflStripGrn.meanG > 1.3 * reflStripGrn.meanR &&
                            reflStripGrn.meanG > 1.3 * reflStripGrn.meanB,
                        "strip 2 mirrors the green emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantGreen={}/{} px (need >0.5% of the strip)", reflStripGrn.meanR, reflStripGrn.meanG,
                        reflStripGrn.meanB, reflStripGrn.dominantGrn, reflStripGrn.pixels
                    );
                    const bool reflBluOk = ZHLN_CHECK(
                        reflStripBlu.dominantBlu * 200 > reflStripBlu.pixels && reflStripBlu.meanB > 1.3 * reflStripBlu.meanR &&
                            reflStripBlu.meanB > 1.3 * reflStripBlu.meanG,
                        "strip 3 mirrors the blue emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantBlue={}/{} px (need >0.5% of the strip)", reflStripBlu.meanR, reflStripBlu.meanG,
                        reflStripBlu.meanB, reflStripBlu.dominantBlu, reflStripBlu.pixels
                    );
                    // Yellow has no single dominant channel to lean on, so its signature is
                    // the R+G mix count plus R and G clearing B by the same 1.3x the pure
                    // strips use and staying within 0.6x of each other (yellow, not amber).
                    const bool reflYelOk = ZHLN_CHECK(
                        reflStripYel.yellowMix * 200 > reflStripYel.pixels && reflStripYel.meanR > 1.3 * reflStripYel.meanB &&
                            reflStripYel.meanG > 1.3 * reflStripYel.meanB && reflStripYel.meanR > 0.6 * reflStripYel.meanG &&
                            reflStripYel.meanG > 0.6 * reflStripYel.meanR,
                        "strip 4 mirrors the yellow emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), yellowMix={}/{} px (need >0.5% of the strip)", reflStripYel.meanR, reflStripYel.meanG,
                        reflStripYel.meanB, reflStripYel.yellowMix, reflStripYel.pixels
                    );

                    // 2. Validate Upper Direct Emission visibility. The peak-luma guard
                    // stays absolute: it only asserts the emitters are directly visible
                    // somewhere in the upper frame, not that the scene is bright.
                    const auto     upperDirect      = MeasureSubRegion(frame, {.x0 = 0.0, .y0 = 0.05, .x1 = 1.0, .y1 = 0.45});
                    const uint32_t directChroma     = upperDirect.dominantRed + upperDirect.dominantGrn + upperDirect.dominantBlu + upperDirect.yellowMix;
                    const bool     directVisible    = ZHLN_CHECK(
                        upperDirect.maxLuma > 60.0 && directChroma * 1000 > upperDirect.pixels, "emitters are directly visible in the upper frame",
                        "maxLuma={:.1f} (need >60), chromaticPixels={}/{} px (need >0.1%)", upperDirect.maxLuma, directChroma, upperDirect.pixels
                    );

                    return reflRedOk && reflGrnOk && reflBluOk && reflYelOk && directVisible;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::MultiEmissiveReflectionFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            ZHLN::Println("    [PASS] Multi-emissive objects correctly mapped to their respective mirror reflections.");
            return {};
        }

        // ====================================================================
        // 8. Dense Multi-Light & Emissive Materials Cross-Interaction
        // ====================================================================
        std::expected<void, ZHLN::Error> dense_multi_light_emissive_materials_cross_interaction() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 6.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                auto floorMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.5f, .roughness = 0.25f, .baseColor = {0.6f, 0.6f, 0.65f, 1.0f}}
                );
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 80.0f, {0.6f, 0.6f, 0.65f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = false, .materialOverride = *floorMat}
                );

                auto monolithMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.2f, .baseColor = {0.0f, 1.0f, 1.0f, 1.0f}, .emissive = {0.0f, 20.0f, 20.0f, 1.0f}
                        }
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.8f, 2.5f, 0.8f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 2.5, 0.0), .createPhysics = false, .materialOverride = *monolithMat}
                );

                auto goldMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.15f, .baseColor = {1.0f, 0.76f, 0.14f, 1.0f}}
                );
                auto roughPlastic = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.8f, .baseColor = {0.8f, 0.2f, 0.2f, 1.0f}}
                );

                for (int x = -2; x <= 2; ++x) {
                    for (int z = -2; z <= 2; ++z) {
                        if (x == 0 && z == 0)
                            continue;
                        float px = static_cast<float>(x) * 3.5f;
                        float pz = static_cast<float>(z) * 3.5f;
                        ZHLN::CreativeWorksFactory::CreateBox(
                            *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                            ZHLN::CreativeWorksFactory::SpawnParams {
                                .position = JPH::RVec3(px, 0.5, pz), .createPhysics = false, .materialOverride = ((x + z) % 2 == 0) ? *goldMat : *roughPlastic
                            }
                        );
                    }
                }

                constexpr size_t kLightCount = 32;
                for (size_t i = 0; i < kLightCount; ++i) {
                    float angle  = (static_cast<float>(i) / static_cast<float>(kLightCount)) * 6.283185f;
                    float radius = 5.0f + (static_cast<float>(i % 3) * 2.0f);
                    float lx     = std::sin(angle) * radius;
                    float lz     = std::cos(angle) * radius;
                    float ly     = 1.0f + static_cast<float>(i % 4) * 0.8f;

                    JPH::Vec3 lightCol(std::sin(angle) * 0.5f + 0.5f, std::cos(angle * 0.5f) * 0.5f + 0.5f, std::sin(angle * 1.5f + 1.0f) * 0.5f + 0.5f);

                    const ZHLN::Entity lt = reg.Create();
                    reg.Add(
                        lt, ZHLN::Components::TransformComponent {.position = JPH::Vec3(lx, ly, lz)},
                        ZHLN::Components::LightComponent {
                            .type      = ZHLN::LightType::Point,
                            .color     = lightCol,
                            .intensity = 220.0f,
                            .range     = 12.0f,
                        }
                    );
                }

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 8.0f, -18.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -22.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;

            const auto stable = RunStableScene(
                *engine, 10, "dense_multi_light_emissive_materials_cross_interaction",
                [](ZHLN::Engine& eng) -> bool {
                    const RgbImage frame      = Capture(eng, "headless_lighting_dense_interaction.ppm");
                    auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                    if (!checkFrame) {
                        return false;
                    }

                    const FrameMetrics m = MeasureImage(frame);

                    ZHLN::Println("    [INFO] Dense 32-Light + Emissive Monolith Scene Metrics:");
                    ZHLN::Println("      Lit Pixels: {}, Dark Pixels: {}, Saturated Pixels: {}, Mean Luma: {:.2f}", m.lit, m.dark, m.saturated, m.meanLuma);
                    ZHLN::Println(
                        "      Cyan (Monolith Reflection) Pixels: {}, Red Pixels: {}, Green Pixels: {}, Blue Pixels: {}", m.cyan, m.red, m.green, m.blue
                    );

                    const bool wellLit          = ZHLN::Test::ExpectTrue(m.lit > (m.total * 0.35));
                    const bool limitedBlowout   = ZHLN::Test::ExpectTrue(m.saturated < (m.total * 0.05));
                    const bool cyanObserved     = ZHLN::Test::ExpectTrue(m.cyan > 200u);
                    const bool multiColorActive = ZHLN::Test::ExpectTrue(m.red > 200u && m.green > 200u && m.blue > 200u);

                    return wellLit && limitedBlowout && cyanObserved && multiColorActive;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                return std::unexpected(LightingRTTestError::DenseCrossInteractionFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            ZHLN::Println("    [PASS] 32 Dynamic point lights and emissive monolith cross-interaction verified.");
            return {};
        }
    };
};
// Exported for the GPU_Lighting group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunLightingRTSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<LightingRTTestSuite>();
}

