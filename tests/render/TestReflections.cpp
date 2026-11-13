// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestReflections.cpp
//
// Reflections: ray-traced reflection coverage and artifacts, multi-emissive
// sources on a mirrored surface, and dense multi-light interaction with
// emissive materials.
//
// Split out of TestLightingRayTraced.cpp; the shared error enum, frame I/O
// and engine fixture live in LightingRTCommon.hpp.

#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include "LightingRTCommon.hpp"

// ============================================================================
// Test Suite
// ============================================================================

namespace {

struct ReflectionsTestSuite {
    ReflectionsTestSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~ReflectionsTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    struct Tests {
        // ====================================================================
        // 5. Ray-Traced Reflection Coverage & Artifacts
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> raytraced_reflection_coverage_and_artifacts() {
            auto engine      = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableAA(*engine);

            {
                auto reg = engine->Entities();
                auto rc = engine->Graphics();

                engine->Graphics().PatchSettings([](auto& gfx) {
                    gfx.environment.fullBright       = 0;
                    gfx.environment.ambientExposure  = 6.0f;
                    gfx.post.enableSSR               = 1;
                    gfx.rayTracing.enableReflections = true;   // BVH ray gate: reflection.slang only traces when wantRTR
                });

                auto mirrorMatRes = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 1.0f, .roughness = 0.03f, .baseColor = {0.85f, 0.85f, 0.88f, 1.0f}});
                if (!ZHLN::Test::ExpectTrue(mirrorMatRes.has_value())) {
                    return std::unexpected(LightingRTTestError::MaterialCreationFailed);
                }
                ZHLN::PrefabFactory::CreatePlane(
                    *engine, 120.0f, {0.85f, 0.85f, 0.88f, 1.0f},
                    ZHLN::PrefabFactory::SpawnParams {.position = {0.0, 0.0, 0.0}, .createPhysics = false, .materialOverride = *mirrorMatRes}
                );

                auto emissiveMatRes = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.55f, .baseColor = {1.0f, 0.06f, 0.04f, 1.0f}, .emissive = {1.0f, 0.0f, 0.0f, 1.0f}}
                );
                if (!ZHLN::Test::ExpectTrue(emissiveMatRes.has_value())) {
                    return std::unexpected(LightingRTTestError::MaterialCreationFailed);
                }
                ZHLN::PrefabFactory::CreateBox(
                    *engine, {0.5f, 0.5f, 0.5f},
                    ZHLN::PrefabFactory::SpawnParams {.position = {0.0, 3.0, 0.0}, .createPhysics = false, .materialOverride = *emissiveMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Spawn();
                reg.Attach(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = ZHLN::FVec3{0.0f, 50.0f, 40.0f}, .rotation = ZHLN::Quat::FromEulerDegrees({{40.0f, 0.0f, 0.0f}})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = ZHLN::FVec3{1.0f, 1.0f, 1.0f},
                        .intensity = 140.0f,
                        .direction = ZHLN::FVec3{0.0f, 0.75f, 0.66f}.Normalized()
                    }
                );

                const bool camOk = engine->Entities().PatchSingleton<ZHLN::Components::CameraComponent>([](auto& c) {
                    auto& cam = c.camera;
                    cam.position = {0.0f, 5.0f, 14.0f};
                    cam.yaw      = ZHLN::DegF{-90.0f};
                    cam.pitch    = ZHLN::DegF{-22.0f};
                    cam.fov      = ZHLN::DegF{60.0f};
                });
                if (!ZHLN::Test::ExpectTrue(camOk)) {
                    return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
                }
            }

            uint32_t validationRaised = 0;
            // Set when the render could not be captured at all, which is not a
            // lighting failure and must not be reported as one.
            bool captureFailed = false;
            // Which kind of failure the scene actually saw. "No reflection at
            // all" means RTR/SSR silently fell back to IBL, which is a
            // different bug from a reflection that is present but blown out or
            // speckled -- and needs different things looked at.
            bool reflectionMissing = false;

            // Retrained against scripts/pipeline_harness/baselines/measurements.json.
            // The reflection ray now lives behind wantRTR (the TLAS/BVH-first
            // gate), while enableSSR only gates the texLighting screen cache.
            // So the A/B flips enableRTR: with it off there is no BVH ray at
            // all and the polished surface falls back to IBL. Flipping the
            // switch detects ANY reflection -- red, white, or otherwise, with
            // or without the cache. (Artifact shape -- debris, speckle -- is
            // owned by the RayTracedReflectionNoise suite, which measures it
            // against the noise floor instead of absolute counts.)
            const auto stable = RunStableScene(
                *engine, 10, "raytraced_reflection_coverage_and_artifacts",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    reflectionMissing = false;

                    // The emitter's mirror image lands mid-frame below the
                    // emitter (u ~ 0.5, v ~ 0.63); this crop holds it with
                    // wide margin while excluding most of the sky-mirror.
                    constexpr ZHLN::Test::Image::NormalizedRect kReflectionCrop {.x0 = 0.35, .y0 = 0.52, .x1 = 0.65, .y1 = 0.77};

                    TickFrames(eng, 1);
                    const RgbImage onA = Capture(eng, "headless_lighting_rt_reflect_f0.ppm");
                    TickFrames(eng, 1);
                    const RgbImage onB = Capture(eng, "headless_lighting_rt_reflect_f1.ppm");
                    if (!(ZHLN::Test::ExpectTrue(onA.Valid()) && ZHLN::Test::ExpectTrue(onB.Valid()))) {
                        captureFailed = true;
                        return false;
                    }

                    eng.Graphics().PatchSettings([](auto& gfx) { gfx.rayTracing.enableReflections = false; });
                    TickFrames(eng, 2);
                    const RgbImage off = Capture(eng, "headless_lighting_rt_reflect_rtr_off.ppm");
                    eng.Graphics().PatchSettings([](auto& gfx) { gfx.rayTracing.enableReflections = true; });
                    if (!ZHLN::Test::ExpectTrue(off.Valid())) {
                        captureFailed = true;
                        return false;
                    }

                    const RgbImage cropA         = ZHLN::Test::Image::CropImage(onA, kReflectionCrop);
                    const RgbImage cropB         = ZHLN::Test::Image::CropImage(onB, kReflectionCrop);
                    const RgbImage cropOff       = ZHLN::Test::Image::CropImage(off, kReflectionCrop);
                    const double   cropPixels    = static_cast<double>(cropA.width * cropA.height);
                    const double   noiseFrac     = CompareFrames(cropA, cropB).frac32;
                    const double   signalFrac    = CompareFrames(cropA, cropOff).frac32;
                    ZHLN::Println(
                        "    [INFO] RTR reflection A/B in mirror crop: on/on frac32={:.4f} ({:.1f}px), on/off frac32={:.4f} ({:.1f}px), {} crop px",
                        noiseFrac, noiseFrac * cropPixels, signalFrac, signalFrac * cropPixels, cropA.width * cropA.height
                    );

                    // Pooled fraction, not a pixel count: the 24px floor graded
                    // blob size on one GPU and failed when the ray gate moved.
                    // Harness baseline: signalFrac=0.729, noiseFrac=0.000 in
                    // this crop (bit-exact repeat frames). 0.001 is the
                    // adopted presence floor (tests/INVARIANT_TESTING.md) --
                    // "a reflection exists", not "the blob is this big".
                    // The 5x noise term keeps bit-exact runs honest.
                    constexpr double kReflectionPresenceFrac = 0.001;
                    const bool reflectionPresent = ZHLN::Test::ExpectGt(
                        signalFrac, std::max(kReflectionPresenceFrac, 5.0 * noiseFrac));
                    reflectionMissing = !reflectionPresent;
                    return reflectionPresent;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::RenderOutputBlank);
                }
                return std::unexpected(reflectionMissing ? LightingRTTestError::ReflectionMissing
                                                         : LightingRTTestError::ReflectionArtifacts);
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
        // 7. Multi-Emissive Sources & Surface Reflection Interaction
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> multi_emissive_sources_and_surface_reflection_interaction() {
            auto engine      = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableAA(*engine);

            {
                auto reg = engine->Entities();
                auto rc = engine->Graphics();

                engine->Graphics().PatchSettings([](auto& gfx) {
                    gfx.environment.fullBright       = 0;
                    gfx.environment.ambientExposure  = 6.0f;
                    gfx.post.enableSSR               = 1;
                    gfx.rayTracing.enableReflections = true;   // BVH ray gate: reflection.slang only traces when wantRTR
                    gfx.post.mode                    = 0;
                });

                // Sun illumination to support scene depth and HDR tone mapping
                const ZHLN::Entity sunEnt = reg.Spawn();
                reg.Attach(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = ZHLN::FVec3{0.0f, 50.0f, 40.0f}, .rotation = ZHLN::Quat::FromEulerDegrees({{40.0f, 0.0f, 0.0f}})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = ZHLN::FVec3{1.0f, 1.0f, 1.0f},
                        .intensity = 100.0f,
                        .direction = ZHLN::FVec3{0.0f, 0.75f, 0.66f}.Normalized()
                    }
                );

                // Polished metallic mirror floor
                auto mirrorMat = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 1.0f, .roughness = 0.02f, .baseColor = {0.9f, 0.9f, 0.95f, 1.0f}});
                if (!ZHLN::Test::ExpectTrue(mirrorMat.has_value())) {
                    return std::unexpected(LightingRTTestError::MaterialCreationFailed);
                }

                ZHLN::PrefabFactory::CreatePlane(
                    *engine, 120.0f, {0.9f, 0.9f, 0.95f, 1.0f},
                    ZHLN::PrefabFactory::SpawnParams {.position = {0, 0, 0}, .createPhysics = false, .materialOverride = *mirrorMat}
                );

                // 4 Distinct High-Luminance Emissive Geometric Emitters at Y = 3.0, Z = 0.0:
                //   Emitter 1: X = -4.5 (Pure Red)
                //   Emitter 2: X = -1.5 (Pure Green)
                //   Emitter 3: X = +1.5 (Pure Blue)
                //   Emitter 4: X = +4.5 (Golden Yellow)
                auto matEmissiveRed = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.05f, 0.05f, 1.0f}, .emissive = {6.0f, 0.0f, 0.0f, 1.0f}}
                );
                auto matEmissiveGrn = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.05f, 1.0f, 0.05f, 1.0f}, .emissive = {0.0f, 6.0f, 0.0f, 1.0f}}
                );
                auto matEmissiveBlu = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.05f, 0.05f, 1.0f, 1.0f}, .emissive = {0.0f, 0.0f, 6.0f, 1.0f}}
                );
                auto matEmissiveYel = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.9f, 0.05f, 1.0f}, .emissive = {5.0f, 4.5f, 0.0f, 1.0f}}
                );

                ZHLN::PrefabFactory::CreateBox(
                    *engine, {0.5f, 0.5f, 0.5f},
                    ZHLN::PrefabFactory::SpawnParams {.position = {-4.5, 3.0, 0.0}, .createPhysics = false, .materialOverride = *matEmissiveRed}
                );
                ZHLN::PrefabFactory::CreateBox(
                    *engine, {0.5f, 0.5f, 0.5f},
                    ZHLN::PrefabFactory::SpawnParams {.position = {-1.5, 3.0, 0.0}, .createPhysics = false, .materialOverride = *matEmissiveGrn}
                );
                ZHLN::PrefabFactory::CreateBox(
                    *engine, {0.5f, 0.5f, 0.5f},
                    ZHLN::PrefabFactory::SpawnParams {.position = {1.5, 3.0, 0.0}, .createPhysics = false, .materialOverride = *matEmissiveBlu}
                );
                ZHLN::PrefabFactory::CreateBox(
                    *engine, {0.5f, 0.5f, 0.5f},
                    ZHLN::PrefabFactory::SpawnParams {.position = {4.5, 3.0, 0.0}, .createPhysics = false, .materialOverride = *matEmissiveYel}
                );

                const bool camOk = engine->Entities().PatchSingleton<ZHLN::Components::CameraComponent>([](auto& c) {
                    auto& cam = c.camera;
                    cam.position = {0.0f, 5.0f, 14.0f};
                    cam.yaw      = ZHLN::DegF{-90.0f};
                    cam.pitch    = ZHLN::DegF{-22.0f};
                    cam.fov      = ZHLN::DegF{60.0f};
                });
                if (!ZHLN::Test::ExpectTrue(camOk)) {
                    return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
                }
            }

            uint32_t validationRaised = 0;
            // Set when the render could not be captured at all, which is not a
            // lighting failure and must not be reported as one.
            bool captureFailed = false;

            const auto stable = RunStableScene(
                *engine, 8, "multi_emissive_sources_and_surface_reflection_interaction",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    // Two RTR-on frames (noise baseline) plus one RTR-off
                    // frame: the A/B proves a reflection lands in each strip,
                    // the cross-strip ordering below proves each strip carries
                    // its own emitter's hue. enableRTR is the ray gate now;
                    // flipping enableSSR would only toggle the screen cache.
                    // The switch is restored before returning so RunStableScene
                    // retries see the same scene.
                    const RgbImage frame = Capture(eng, "headless_lighting_multi_emissive.ppm");
                    const RgbImage onB   = Capture(eng, "headless_lighting_multi_emissive_b.ppm");
                    if (!(ZHLN::Test::ExpectTrue(frame.Valid()) && ZHLN::Test::ExpectTrue(onB.Valid()))) {
                        captureFailed = true;
                        return false;
                    }

                    eng.Graphics().PatchSettings([](auto& gfx) { gfx.rayTracing.enableReflections = false; });
                    TickFrames(eng, 2);
                    const RgbImage off = Capture(eng, "headless_lighting_multi_emissive_rtr_off.ppm");
                    eng.Graphics().PatchSettings([](auto& gfx) { gfx.rayTracing.enableReflections = true; });
                    if (!ZHLN::Test::ExpectTrue(off.Valid())) {
                        captureFailed = true;
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

                    // Per-strip RTR A/B: pooled frac32 when the ray gate flips,
                    // against the on/on noise in the same strip. Harness
                    // baseline: signal frac32 0.984..1.000 in every strip,
                    // noise 0.000 (bit-exact repeat frames). The 2px floor was
                    // a count on one frame; 0.001 is the adopted presence floor
                    // and does not grade blob size (red and green blobs differ
                    // by ~7x in area, which is exactly why a count was brittle).
                    const auto stripAB = [&](double x0, double x1) {
                        const ZHLN::Test::Image::NormalizedRect rect {.x0 = x0, .y0 = 0.52, .x1 = x1, .y1 = 0.95};
                        const RgbImage a = ZHLN::Test::Image::CropImage(frame, rect);
                        const RgbImage b = ZHLN::Test::Image::CropImage(onB, rect);
                        const RgbImage o = ZHLN::Test::Image::CropImage(off, rect);
                        return std::array<double, 2> {CompareFrames(a, o).frac32, CompareFrames(a, b).frac32};
                    };
                    const auto [redSig, redNoise] = stripAB(0.20, 0.36);
                    const auto [grnSig, grnNoise] = stripAB(0.38, 0.48);
                    const auto [bluSig, bluNoise] = stripAB(0.52, 0.62);
                    const auto [yelSig, yelNoise] = stripAB(0.64, 0.80);
                    ZHLN::Println(
                        "      strip on/off frac32 (noise): red={:.4f} ({:.4f}) green={:.4f} ({:.4f}) blue={:.4f} ({:.4f}) yellow={:.4f} ({:.4f})",
                        redSig, redNoise, grnSig, grnNoise, bluSig, bluNoise, yelSig, yelNoise
                    );
                    constexpr double kStripPresenceFrac = 0.001;
                    const bool redAB = ZHLN::Test::ExpectGt(redSig, std::max(kStripPresenceFrac, 5.0 * redNoise));
                    const bool grnAB = ZHLN::Test::ExpectGt(grnSig, std::max(kStripPresenceFrac, 5.0 * grnNoise));
                    const bool bluAB = ZHLN::Test::ExpectGt(bluSig, std::max(kStripPresenceFrac, 5.0 * bluNoise));
                    const bool yelAB = ZHLN::Test::ExpectGt(yelSig, std::max(kStripPresenceFrac, 5.0 * yelNoise));

                    // 1. Spatial mirror correspondence. The strips are ~100x
                    // the blob area, so strip MEANS can never show the
                    // reflection (a perfect 200px blob moves a 21kpx sky-blue
                    // mean by <1 luma). Dominant COUNTS were the second
                    // attempt: their r>=45 floor and +10u/+5u margins encoded
                    // one frame on one GPU and read all-zero after the ray gate
                    // moved. Ask for a hue SHARE instead (the adopted shape in
                    // tests/INVARIANT_TESTING.md): the level floor is relative
                    // to each window's own brightest pixel, so exposure,
                    // tonemap and IBL changes move both sides together.
                    // Harness baseline: owning strip 0.99..1.00, all others
                    // 0.00. Blue keeps its A/B gate below (sky passes any blue
                    // detector), so it has no colour-ordering gate.
                    ZHLN::Println(
                        "      dominant red/grn per strip: s1={}/{} s2={}/{} s3={}/{} s4={}/{}", reflStripRed.dominantRed, reflStripRed.dominantGrn,
                        reflStripGrn.dominantRed, reflStripGrn.dominantGrn, reflStripBlu.dominantRed, reflStripBlu.dominantGrn,
                        reflStripYel.dominantRed, reflStripYel.dominantGrn
                    );
                    ZHLN::Println(
                        "      dominant blu/yel per strip: s1={}/{} s2={}/{} s3={}/{} s4={}/{}", reflStripRed.dominantBlu, reflStripRed.yellowMix,
                        reflStripGrn.dominantBlu, reflStripGrn.yellowMix, reflStripBlu.dominantBlu, reflStripBlu.yellowMix,
                        reflStripYel.dominantBlu, reflStripYel.yellowMix
                    );
                    const auto stripRect = [](double x0, double x1) {
                        return ZHLN::Test::Image::NormalizedRect {.x0 = x0, .y0 = 0.52, .x1 = x1, .y1 = 0.95};
                    };
                    using ZHLN::Test::Image::HueChannel;
                    const auto hueShares = [&](double x0, double x1) {
                        const auto r = stripRect(x0, x1);
                        // [0]=red [1]=green [2]=blue [3]=yellow
                        return std::array<double, 4> {
                            ZHLN::Test::Image::DominantHueShare(frame, r, HueChannel::Red),
                            ZHLN::Test::Image::DominantHueShare(frame, r, HueChannel::Green),
                            ZHLN::Test::Image::DominantHueShare(frame, r, HueChannel::Blue),
                            ZHLN::Test::Image::YellowShare(frame, r),
                        };
                    };
                    const auto sRed = hueShares(0.20, 0.36);
                    const auto sGrn = hueShares(0.38, 0.48);
                    const auto sBlu = hueShares(0.52, 0.62);
                    const auto sYel = hueShares(0.64, 0.80);
                    ZHLN::Println(
                        "      hue share red/grn/blu/yel: s1={:.3f}/{:.3f}/{:.3f}/{:.3f} s2={:.3f}/{:.3f}/{:.3f}/{:.3f} "
                        "s3={:.3f}/{:.3f}/{:.3f}/{:.3f} s4={:.3f}/{:.3f}/{:.3f}/{:.3f}",
                        sRed[0], sRed[1], sRed[2], sRed[3], sGrn[0], sGrn[1], sGrn[2], sGrn[3],
                        sBlu[0], sBlu[1], sBlu[2], sBlu[3], sYel[0], sYel[1], sYel[2], sYel[3]
                    );

                    // 0.01 is the adopted presence share ("existence, not
                    // look"); 1.5x is a ratio, so a grade change cannot fail
                    // it. Harness measured 0.99..1.00 vs 0.00 on every hue.
                    constexpr double kHuePresenceShare = 0.01;
                    constexpr double kHueLeadRatio     = 1.5;
                    const auto       max3              = [](double a, double b, double c) { return std::max(a, std::max(b, c)); };
                    // strip 1 mirrors the red emitter
                    const bool reflRedOk = ZHLN::Test::ExpectGt(sRed[0], kHuePresenceShare) &&
                                           ZHLN::Test::ExpectGt(sRed[0], kHueLeadRatio * max3(sGrn[0], sBlu[0], sYel[0]));
                    // strip 2 mirrors the green emitter
                    const bool reflGrnOk = ZHLN::Test::ExpectGt(sGrn[1], kHuePresenceShare) &&
                                           ZHLN::Test::ExpectGt(sGrn[1], kHueLeadRatio * max3(sRed[1], sBlu[1], sYel[1]));
                    // strip 3 mirrors the blue emitter: blue-on-blue is
                    // unmeasurable by colour (the sky passes any blue detector),
                    // so the strip A/B above owns blue and there is no colour
                    // ordering gate for it.
                    const bool reflBluOk = bluAB;
                    // Yellow has no single dominant channel, so its signature
                    // is the R+G share leading the other strips.
                    const bool reflYelOk = ZHLN::Test::ExpectGt(sYel[3], kHuePresenceShare) &&
                                           ZHLN::Test::ExpectGt(sYel[3], kHueLeadRatio * max3(sRed[3], sGrn[3], sBlu[3]));

                    // 2. Validate Upper Direct Emission visibility. The peak-luma guard
                    // stays absolute: it only asserts the emitters are directly visible
                    // somewhere in the upper frame, not that the scene is bright.
                    const auto     upperDirect      = MeasureSubRegion(frame, {.x0 = 0.0, .y0 = 0.05, .x1 = 1.0, .y1 = 0.45});
                    const uint32_t directChroma     = upperDirect.dominantRed + upperDirect.dominantGrn + upperDirect.dominantBlu + upperDirect.yellowMix;
                    // emitters are directly visible in the upper frame
                    const bool directVisible = ZHLN::Test::ExpectGt(upperDirect.maxLuma, 60.0) && ZHLN::Test::ExpectGt(directChroma * 1000, upperDirect.pixels);

                    return redAB && grnAB && yelAB && reflRedOk && reflGrnOk && reflBluOk && reflYelOk && directVisible;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::RenderOutputBlank);
                }
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
        std::expected<void, ZHLN::ErrorCode> dense_multi_light_emissive_materials_cross_interaction() {
            auto engine      = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine.Valid())) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableAA(*engine);

            {
                auto reg = engine->Entities();
                auto rc = engine->Graphics();

                engine->Graphics().PatchSettings([](auto& gfx) {
                    gfx.environment.fullBright       = 0;
                    gfx.post.exposure                = 0.015f;
                    gfx.environment.ambientExposure  = 6.0f;
                    gfx.post.enableSSR               = 1;
                    gfx.rayTracing.enableReflections = true;   // BVH ray gate: reflection.slang only traces when wantRTR
                    gfx.post.mode                    = 0;
                });

                auto floorMat = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 0.5f, .roughness = 0.25f, .baseColor = {0.6f, 0.6f, 0.65f, 1.0f}});
                ZHLN::PrefabFactory::CreatePlane(
                    *engine, 80.0f, {0.6f, 0.6f, 0.65f, 1.0f},
                    ZHLN::PrefabFactory::SpawnParams {.position = {0, 0, 0}, .createPhysics = false, .materialOverride = *floorMat}
                );

                auto monolithMat = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.2f, .baseColor = {0.0f, 1.0f, 1.0f, 1.0f}, .emissive = {0.0f, 20.0f, 20.0f, 1.0f}}
                );
                ZHLN::PrefabFactory::CreateBox(
                    *engine, {0.8f, 2.5f, 0.8f},
                    ZHLN::PrefabFactory::SpawnParams {.position = {0.0, 2.5, 0.0}, .createPhysics = false, .materialOverride = *monolithMat}
                );

                auto goldMat      = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 1.0f, .roughness = 0.15f, .baseColor = {1.0f, 0.76f, 0.14f, 1.0f}});
                auto roughPlastic = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.8f, .baseColor = {0.8f, 0.2f, 0.2f, 1.0f}});

                for (int x = -2; x <= 2; ++x) {
                    for (int z = -2; z <= 2; ++z) {
                        if (x == 0 && z == 0)
                            continue;
                        float px = static_cast<float>(x) * 3.5f;
                        float pz = static_cast<float>(z) * 3.5f;
                        ZHLN::PrefabFactory::CreateBox(
                            *engine, {0.5f, 0.5f, 0.5f},
                            ZHLN::PrefabFactory::SpawnParams {
                                .position = {px, 0.5, pz}, .createPhysics = false, .materialOverride = ((x + z) % 2 == 0) ? *goldMat : *roughPlastic
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

                    ZHLN::FVec3 lightCol{std::sin(angle) * 0.5f + 0.5f, std::cos(angle * 0.5f) * 0.5f + 0.5f, std::sin(angle * 1.5f + 1.0f) * 0.5f + 0.5f};

                    const ZHLN::Entity lt = reg.Spawn();
                    reg.Attach(
                        lt, ZHLN::Components::TransformComponent {.position = ZHLN::FVec3{lx, ly, lz}},
                        ZHLN::Components::LightComponent {
                            .type      = ZHLN::LightType::Point,
                            .color     = lightCol,
                            .intensity = 220.0f,
                            .range     = 12.0f,
                        }
                    );
                }

                const bool camOk = engine->Entities().PatchSingleton<ZHLN::Components::CameraComponent>([](auto& c) {
                    auto& cam = c.camera;
                    cam.position = {0.0f, 8.0f, -18.0f};
                    cam.yaw      = ZHLN::DegF{90.0f};
                    cam.pitch    = ZHLN::DegF{-22.0f};
                    cam.fov      = ZHLN::DegF{60.0f};
                });
                if (!ZHLN::Test::ExpectTrue(camOk)) {
                    return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
                }
            }

            uint32_t validationRaised = 0;
            // Set when the render could not be captured at all, which is not a
            // lighting failure and must not be reported as one.
            bool captureFailed = false;

            const auto stable = RunStableScene(
                *engine, 10, "dense_multi_light_emissive_materials_cross_interaction",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    const RgbImage frame      = Capture(eng, "headless_lighting_dense_interaction.ppm");
                    if (!ZHLN::Test::ExpectTrue(frame.Valid())) {
                        captureFailed = true;
                        return false;
                    }

                    const FrameMetrics m = MeasureImage(frame);

                    ZHLN::Println("    [INFO] Dense 32-Light + Emissive Monolith Scene Metrics:");
                    ZHLN::Println("      Lit Pixels: {}, Dark Pixels: {}, Saturated Pixels: {}, Mean Luma: {:.2f}", m.lit, m.dark, m.saturated, m.meanLuma);
                    ZHLN::Println(
                        "      Cyan (Monolith Reflection) Pixels: {}, Red Pixels: {}, Green Pixels: {}, Blue Pixels: {}", m.cyan, m.red, m.green, m.blue
                    );

                    // Which primaries the 32-light palette can even produce: the
                    // palette formula authors 1 red-dominant light, 11
                    // blue-dominant, and ZERO green-dominant, so green > 200
                    // could never pass (mixed lights on neutral surfaces only
                    // dilute further). Derive the expectation from the palette
                    // instead of asserting all three primaries blindly.
                    // 32 mirrors kLightCount in the scene block: same palette, same order.
                    constexpr size_t kPaletteLights = 32;
                    size_t             paletteRed   = 0;
                    size_t             paletteGrn   = 0;
                    size_t             paletteBlu   = 0;
                    for (size_t i = 0; i < kPaletteLights; ++i) {
                        const float angle = (static_cast<float>(i) / static_cast<float>(kPaletteLights)) * 6.283185f;
                        const float r     = std::sin(angle) * 0.5f + 0.5f;
                        const float g     = std::cos(angle * 0.5f) * 0.5f + 0.5f;
                        const float b     = std::sin(angle * 1.5f + 1.0f) * 0.5f + 0.5f;
                        // MeasureImage's strict classifiers, run on the palette.
                        if (r >= 1.6f * g && r >= 1.6f * b) {
                            ++paletteRed;
                        }
                        if (g >= 1.6f * r && g >= 1.6f * b) {
                            ++paletteGrn;
                        }
                        if (b >= 1.6f * r && b >= 1.6f * g) {
                            ++paletteBlu;
                        }
                    }
                    ZHLN::Println("      palette-authored primaries: red={} green={} blue={}", paletteRed, paletteGrn, paletteBlu);

                    const bool wellLit = ZHLN::Test::ExpectGt(m.lit, (m.total * 0.35));
                    // 32x220 lights plus 20x emissive at exposure 6 renders
                    // ~19% near-white BY CONSTRUCTION; the old 5% cap graded
                    // the look, not the lighting. The bound is a white-out
                    // sanity bound with headroom, not a calibration.
                    const bool limitedBlowout = ZHLN::Test::ExpectLt(m.saturated, (m.total * 0.50));
                    const bool cyanObserved   = ZHLN::Test::ExpectGt(m.cyan, 200u);
                    // The only emissive geometry is cyan, not pure blue. Each
                    // primary is gated only when the palette authors it: red
                    // needs its >200 pool (one red-dominant light plus gold
                    // surfaces), blue a visible >24 patch (the coverage gate
                    // the single-reflection case uses), green nothing at all.
                    const bool multiColorActive = (paletteRed == 0 || ZHLN::Test::ExpectGt(m.red, 200u)) &&
                                                  (paletteGrn == 0 || ZHLN::Test::ExpectGt(m.green, 200u)) &&
                                                  (paletteBlu == 0 || ZHLN::Test::ExpectGt(m.blue, 24u));

                    return wellLit && limitedBlowout && cyanObserved && multiColorActive;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::RenderOutputBlank);
                }
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

} // namespace
